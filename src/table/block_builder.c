#include "table/block_builder.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "util/coding.h"

enum { CLDB_BLOCK_BUILDER_INITIAL_RESTARTS = 4 };

/* 保证还能追加一个 restart 偏移；失败时数组和计数都不变。 */
static bool ensure_restart_capacity(cldb_block_builder_t *builder) {
  size_t max_count = SIZE_MAX / sizeof(uint32_t);
  size_t new_capacity;
  uint32_t *new_restarts;

  if (builder->restarts_count < builder->restarts_capacity) {
    return true;
  }
  if (builder->restarts_capacity >= max_count) {
    return false;
  }
  new_capacity = builder->restarts_capacity == 0
                     ? (size_t)CLDB_BLOCK_BUILDER_INITIAL_RESTARTS
                     : builder->restarts_capacity * 2U;
  if (new_capacity > max_count) {
    new_capacity = max_count;
  }
  new_restarts =
      realloc(builder->restarts, new_capacity * sizeof(uint32_t));
  if (new_restarts == NULL) {
    return false;
  }
  builder->restarts = new_restarts;
  builder->restarts_capacity = new_capacity;
  return true;
}

cldb_status_t cldb_block_builder_init(cldb_block_builder_t *builder,
                                      const cldb_options_t *options) {
  if (builder == NULL) {
    return cldb_status_invalid_argument("BlockBuilder::BlockBuilder",
                                        "builder is NULL");
  }
  memset(builder, 0, sizeof(*builder));
  cldb_buf_init(&builder->buffer);
  cldb_buf_init(&builder->last_key);
  if (options == NULL) {
    return cldb_status_invalid_argument("BlockBuilder::BlockBuilder",
                                        "options is NULL");
  }
  if (options->block_restart_interval < 1) {
    return cldb_status_invalid_argument("BlockBuilder::BlockBuilder",
                                        "block_restart_interval must be >= 1");
  }

  builder->comparator = options->comparator != NULL
                            ? options->comparator
                            : cldb_bytewise_comparator();
  builder->restart_interval = options->block_restart_interval;
  if (!ensure_restart_capacity(builder)) {
    cldb_block_builder_destroy(builder);
    return cldb_status_io("BlockBuilder::BlockBuilder", "out of memory");
  }
  builder->restarts[0] = 0; /* 首个 restart 固定指向条目区起点 */
  builder->restarts_count = 1;
  return cldb_status_ok();
}

void cldb_block_builder_destroy(cldb_block_builder_t *builder) {
  if (builder == NULL) {
    return;
  }
  cldb_buf_free(&builder->buffer);
  cldb_buf_free(&builder->last_key);
  free(builder->restarts);
  memset(builder, 0, sizeof(*builder));
}

void cldb_block_builder_reset(cldb_block_builder_t *builder) {
  if (builder == NULL || builder->restarts == NULL) {
    return;
  }
  cldb_buf_clear(&builder->buffer);
  cldb_buf_clear(&builder->last_key);
  builder->restarts_count = 1;
  builder->restarts[0] = 0;
  builder->counter = 0;
  builder->finished = false;
}

bool cldb_block_builder_empty(const cldb_block_builder_t *builder) {
  return builder == NULL || builder->buffer.size == 0;
}

cldb_slice_t cldb_block_builder_last_key(
    const cldb_block_builder_t *builder) {
  if (builder == NULL) {
    return cldb_slice_empty();
  }
  return cldb_buf_slice(&builder->last_key);
}

size_t cldb_block_builder_current_size_estimate(
    const cldb_block_builder_t *builder) {
  if (builder == NULL) {
    return 0;
  }
  return builder->buffer.size + builder->restarts_count * sizeof(uint32_t) +
         sizeof(uint32_t);
}

/* 构建器是否处于可 Add/Finish 的初始化状态。 */
static bool builder_is_ready(const cldb_block_builder_t *builder) {
  return builder != NULL && builder->comparator != NULL &&
         builder->restart_interval >= 1 && builder->restarts != NULL &&
         builder->restarts_count > 0;
}

cldb_status_t cldb_block_builder_add(cldb_block_builder_t *builder,
                                     cldb_slice_t key, cldb_slice_t value) {
  cldb_slice_t last_key;
  size_t key_size;
  size_t value_size;
  size_t shared = 0;
  size_t non_shared;
  size_t data_size;
  size_t entry_size;
  size_t new_buffer_size;
  size_t trailer_offset;
  bool need_restart;

  if (!builder_is_ready(builder)) {
    return cldb_status_invalid_argument("BlockBuilder::Add",
                                        "builder is not initialized");
  }
  if (builder->finished) {
    return cldb_status_invalid_argument("BlockBuilder::Add",
                                        "Finish has already been called");
  }
  if (!cldb_slice_normalize(&key) || !cldb_slice_normalize(&value)) {
    return cldb_status_invalid_argument("BlockBuilder::Add",
                                        "invalid key or value slice");
  }
  if (key.size > UINT32_MAX || value.size > UINT32_MAX) {
    return cldb_status_invalid_argument("BlockBuilder::Add",
                                        "key or value is too large");
  }
  if (builder->buffer.size != 0) {
    last_key = cldb_buf_slice(&builder->last_key);
    if (cldb_comparator_compare(builder->comparator, key, last_key) <= 0) {
      return cldb_status_invalid_argument("BlockBuilder::Add",
                                          "key is not strictly increasing");
    }
  }

  key_size = key.size;
  value_size = value.size;
  need_restart = builder->counter == builder->restart_interval;
  if (!need_restart) {
    size_t last_size = builder->last_key.size;
    size_t min_size = last_size < key_size ? last_size : key_size;

    while (shared < min_size &&
           builder->last_key.data[shared] == key.data[shared]) {
      ++shared;
    }
  }
  non_shared = key_size - shared;
  if (non_shared > SIZE_MAX - value_size) {
    return cldb_status_invalid_argument("BlockBuilder::Add",
                                        "entry size overflows size_t");
  }
  data_size = non_shared + value_size;
  entry_size = cldb_varint_length((uint64_t)shared) +
               cldb_varint_length((uint64_t)non_shared) +
               cldb_varint_length((uint64_t)value_size);
  if (entry_size > SIZE_MAX - data_size) {
    return cldb_status_invalid_argument("BlockBuilder::Add",
                                        "entry size overflows size_t");
  }
  entry_size += data_size;

  /* restart 偏移是 fixed32；条目区超出该范围后块无法按格式寻址。 */
  if (builder->buffer.size > UINT32_MAX ||
      entry_size > UINT32_MAX - builder->buffer.size) {
    return cldb_status_invalid_argument("BlockBuilder::Add",
                                        "block entries exceed 4 GiB");
  }
  new_buffer_size = builder->buffer.size + entry_size;
  if (new_buffer_size == SIZE_MAX || key_size == SIZE_MAX) {
    return cldb_status_io("BlockBuilder::Add", "out of memory");
  }

  if (need_restart && !ensure_restart_capacity(builder)) {
    return cldb_status_io("BlockBuilder::Add", "out of memory");
  }
  if (!cldb_buf_reserve(&builder->buffer, new_buffer_size + 1U) ||
      !cldb_buf_reserve(&builder->last_key, key_size + 1U)) {
    return cldb_status_io("BlockBuilder::Add", "out of memory");
  }

  if (need_restart) {
    trailer_offset = builder->buffer.size;
    builder->restarts[builder->restarts_count] = (uint32_t)trailer_offset;
    ++builder->restarts_count;
    builder->counter = 0;
  }

  {
    char *dst = builder->buffer.data + builder->buffer.size;

    dst = cldb_encode_varint32(dst, (uint32_t)shared);
    dst = cldb_encode_varint32(dst, (uint32_t)non_shared);
    dst = cldb_encode_varint32(dst, (uint32_t)value_size);
    if (non_shared != 0) {
      memcpy(dst, key.data + shared, non_shared);
      dst += non_shared;
    }
    if (value_size != 0) {
      memcpy(dst, value.data, value_size);
      dst += value_size;
    }
    builder->buffer.size = new_buffer_size;
    builder->buffer.data[new_buffer_size] = '\0';
  }
  /* 已预留 key_size + 1 容量，直接覆盖可避免把状态更新放进仅 Debug
   * 生效的断言里。last_key.data 此时保证非 NULL。 */
  builder->last_key.size = key_size;
  if (key_size != 0) {
    memmove(builder->last_key.data, key.data, key_size);
  }
  builder->last_key.data[key_size] = '\0';
  ++builder->counter;
  return cldb_status_ok();
}

cldb_status_t cldb_block_builder_finish(cldb_block_builder_t *builder,
                                        cldb_slice_t *block) {
  size_t trailer_size;
  size_t total_size;
  size_t i;
  char *dst;

  if (block == NULL) {
    return cldb_status_invalid_argument("BlockBuilder::Finish",
                                        "block is NULL");
  }
  *block = cldb_slice_empty();
  if (!builder_is_ready(builder)) {
    return cldb_status_invalid_argument("BlockBuilder::Finish",
                                        "builder is not initialized");
  }
  if (builder->finished) {
    return cldb_status_invalid_argument("BlockBuilder::Finish",
                                        "Finish has already been called");
  }
  if (builder->restarts_count > UINT32_MAX ||
      builder->restarts_count > (SIZE_MAX - sizeof(uint32_t)) /
                                   sizeof(uint32_t)) {
    return cldb_status_invalid_argument("BlockBuilder::Finish",
                                        "too many restarts");
  }
  trailer_size =
      builder->restarts_count * sizeof(uint32_t) + sizeof(uint32_t);
  if (trailer_size == SIZE_MAX ||
      builder->buffer.size > SIZE_MAX - trailer_size - 1U) {
    return cldb_status_io("BlockBuilder::Finish", "out of memory");
  }
  total_size = builder->buffer.size + trailer_size;
  if (!cldb_buf_reserve(&builder->buffer, total_size + 1U)) {
    return cldb_status_io("BlockBuilder::Finish", "out of memory");
  }

  dst = builder->buffer.data + builder->buffer.size;
  for (i = 0; i < builder->restarts_count; ++i) {
    cldb_encode_fixed32(dst, builder->restarts[i]);
    dst += sizeof(uint32_t);
  }
  cldb_encode_fixed32(dst, (uint32_t)builder->restarts_count);
  builder->buffer.size = total_size;
  builder->buffer.data[total_size] = '\0';
  builder->finished = true;
  *block = cldb_buf_slice(&builder->buffer);
  return cldb_status_ok();
}
