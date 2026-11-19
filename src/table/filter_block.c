#include "table/filter_block.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "util/coding.h"

enum { CLDB_FILTER_BLOCK_INITIAL_CAPACITY = 4 };

/* 计算扩容后的容量；needed <= max_count 时成功写入 *new_capacity。
 * 仅算容量，不分配，避免重复处理倍增溢出。 */
static bool grow_capacity(size_t capacity, size_t needed, size_t elem_size,
                          size_t *new_capacity) {
  size_t max_count = SIZE_MAX / elem_size;
  size_t result;

  if (needed > max_count) {
    return false;
  }
  if (needed <= capacity) {
    *new_capacity = capacity;
    return true;
  }
  result = capacity < (size_t)CLDB_FILTER_BLOCK_INITIAL_CAPACITY
               ? (size_t)CLDB_FILTER_BLOCK_INITIAL_CAPACITY
               : capacity;
  while (result < needed) {
    if (result > max_count / 2) {
      result = needed;
      break;
    }
    result *= 2;
  }
  *new_capacity = result;
  return true;
}

static bool ensure_starts_capacity(cldb_filter_block_builder_t *builder,
                                   size_t needed) {
  size_t new_capacity;
  size_t bytes;
  void *new_starts;

  if (needed <= builder->starts_capacity) {
    return true;
  }
  if (!grow_capacity(builder->starts_capacity, needed, sizeof(size_t),
                     &new_capacity)) {
    return false;
  }
  bytes = new_capacity * sizeof(size_t);
  new_starts = realloc(builder->starts, bytes);
  if (new_starts == NULL) {
    return false;
  }
  builder->starts = new_starts;
  builder->starts_capacity = new_capacity;
  return true;
}

static bool ensure_tmp_keys_capacity(cldb_filter_block_builder_t *builder,
                                     size_t needed) {
  size_t new_capacity;
  size_t bytes;
  void *new_keys;

  if (needed <= builder->tmp_keys_capacity) {
    return true;
  }
  if (!grow_capacity(builder->tmp_keys_capacity, needed,
                     sizeof(cldb_slice_t), &new_capacity)) {
    return false;
  }
  bytes = new_capacity * sizeof(cldb_slice_t);
  new_keys = realloc(builder->tmp_keys, bytes);
  if (new_keys == NULL) {
    return false;
  }
  builder->tmp_keys = new_keys;
  builder->tmp_keys_capacity = new_capacity;
  return true;
}

static bool ensure_filter_offsets_capacity(
    cldb_filter_block_builder_t *builder, size_t needed) {
  size_t new_capacity;
  size_t bytes;
  void *new_offsets;

  if (needed <= builder->filter_offsets_capacity) {
    return true;
  }
  if (!grow_capacity(builder->filter_offsets_capacity, needed,
                     sizeof(uint32_t), &new_capacity)) {
    return false;
  }
  bytes = new_capacity * sizeof(uint32_t);
  new_offsets = realloc(builder->filter_offsets, bytes);
  if (new_offsets == NULL) {
    return false;
  }
  builder->filter_offsets = new_offsets;
  builder->filter_offsets_capacity = new_capacity;
  return true;
}

/* 策略的追加目标；data 为 NULL 时保留 resize 填入的零字节。 */
static char *filter_sink_append(void *arg, const void *data, size_t size) {
  cldb_buf_t *buf = (cldb_buf_t *)arg;
  size_t old_size;

  if (buf == NULL || size == 0 || size > SIZE_MAX - buf->size) {
    return NULL;
  }
  old_size = buf->size;
  if (!cldb_buf_resize(buf, old_size + size)) {
    return NULL;
  }
  if (data != NULL) {
    memcpy(buf->data + old_size, data, size);
  }
  return buf->data + old_size;
}

/* 丢弃策略失败时可能追加的部分内容，保持过滤器数据边界。 */
static void truncate_result(cldb_filter_block_builder_t *builder,
                            size_t old_size) {
  builder->result.size = old_size;
  if (builder->result.data != NULL) {
    builder->result.data[old_size] = '\0';
  }
}

/* 把当前 keys 交给策略生成一个过滤器并记录偏移；没有 key 时只记录空区间。
 * 失败时 result、filter_offsets 和待处理 key 均不变。 */
static cldb_status_t generate_filter(cldb_filter_block_builder_t *builder) {
  size_t num_keys = builder->starts_count;
  size_t old_size = builder->result.size;
  cldb_filter_sink_t sink;
  cldb_status_t status;
  size_t i;

  if (old_size > UINT32_MAX) {
    return cldb_status_io("FilterBlockBuilder::GenerateFilter",
                          "filter block exceeds fixed32 offsets");
  }
  if (!ensure_filter_offsets_capacity(builder,
                                      builder->filter_offsets_count + 1U)) {
    return cldb_status_io("FilterBlockBuilder::GenerateFilter",
                          "out of memory");
  }
  if (num_keys == 0) {
    builder->filter_offsets[builder->filter_offsets_count] =
        (uint32_t)old_size;
    ++builder->filter_offsets_count;
    return cldb_status_ok();
  }
  if (!ensure_tmp_keys_capacity(builder, num_keys)) {
    return cldb_status_io("FilterBlockBuilder::GenerateFilter",
                          "out of memory");
  }

  for (i = 0; i < num_keys; ++i) {
    size_t begin = builder->starts[i];
    size_t end = i + 1U < num_keys ? builder->starts[i + 1U]
                                   : builder->keys.size;

    if (begin > end || end > builder->keys.size) {
      return cldb_status_invalid_argument(
          "FilterBlockBuilder::GenerateFilter", "key offsets are invalid");
    }
    builder->tmp_keys[i] =
        cldb_slice_make(cldb_buf_cstr(&builder->keys) + begin, end - begin);
  }

  sink.arg = &builder->result;
  sink.append = filter_sink_append;
  status = cldb_filter_policy_create_filter(builder->policy, builder->tmp_keys,
                                            num_keys, &sink);
  if (!cldb_status_is_ok(status)) {
    truncate_result(builder, old_size);
    return status;
  }
  if (builder->result.size > UINT32_MAX) {
    truncate_result(builder, old_size);
    return cldb_status_io("FilterBlockBuilder::GenerateFilter",
                          "filter block exceeds fixed32 offsets");
  }

  builder->filter_offsets[builder->filter_offsets_count] =
      (uint32_t)old_size;
  ++builder->filter_offsets_count;
  cldb_buf_clear(&builder->keys);
  builder->starts_count = 0;
  return cldb_status_ok();
}

static bool builder_is_ready(const cldb_filter_block_builder_t *builder) {
  return builder != NULL && builder->policy != NULL;
}

cldb_status_t cldb_filter_block_builder_init(
    cldb_filter_block_builder_t *builder,
    const cldb_filter_policy_t *policy) {
  if (builder == NULL) {
    return cldb_status_invalid_argument("FilterBlockBuilder::FilterBlockBuilder",
                                        "builder is NULL");
  }
  memset(builder, 0, sizeof(*builder));
  cldb_buf_init(&builder->keys);
  cldb_buf_init(&builder->result);
  if (policy == NULL || policy->vtbl == NULL ||
      policy->vtbl->create_filter == NULL ||
      policy->vtbl->key_may_match == NULL) {
    return cldb_status_invalid_argument("FilterBlockBuilder::FilterBlockBuilder",
                                        "invalid filter policy");
  }
  builder->policy = policy;
  return cldb_status_ok();
}

void cldb_filter_block_builder_destroy(cldb_filter_block_builder_t *builder) {
  if (builder == NULL) {
    return;
  }
  cldb_buf_free(&builder->keys);
  cldb_buf_free(&builder->result);
  free(builder->starts);
  free(builder->tmp_keys);
  free(builder->filter_offsets);
  memset(builder, 0, sizeof(*builder));
}

cldb_status_t cldb_filter_block_builder_start_block(
    cldb_filter_block_builder_t *builder, uint64_t block_offset) {
  uint64_t filter_index;

  if (!builder_is_ready(builder) || builder->finished) {
    return cldb_status_invalid_argument(
        "FilterBlockBuilder::StartBlock", "builder is not usable");
  }
  filter_index = block_offset / CLDB_FILTER_BLOCK_BASE;
  if (filter_index < (uint64_t)builder->filter_offsets_count) {
    return cldb_status_invalid_argument(
        "FilterBlockBuilder::StartBlock", "block offset moved backwards");
  }
  /* 先一次性备好空区间所需的偏移槽；循环中即使策略失败也只在第一轮
   * 发生，保证失败时构建器状态不变。 */
  if (filter_index > (uint64_t)SIZE_MAX ||
      !ensure_filter_offsets_capacity(builder, (size_t)filter_index)) {
    return cldb_status_io("FilterBlockBuilder::StartBlock", "out of memory");
  }
  while (filter_index > (uint64_t)builder->filter_offsets_count) {
    cldb_status_t status = generate_filter(builder);

    if (!cldb_status_is_ok(status)) {
      return status;
    }
  }
  return cldb_status_ok();
}

cldb_status_t cldb_filter_block_builder_add_key(
    cldb_filter_block_builder_t *builder, cldb_slice_t key) {
  size_t key_offset;

  if (!builder_is_ready(builder) || builder->finished) {
    return cldb_status_invalid_argument("FilterBlockBuilder::AddKey",
                                        "builder is not usable");
  }
  if (!cldb_slice_normalize(&key)) {
    return cldb_status_invalid_argument("FilterBlockBuilder::AddKey",
                                        "invalid key slice");
  }
  key_offset = builder->keys.size;
  if (!ensure_starts_capacity(builder, builder->starts_count + 1U)) {
    return cldb_status_io("FilterBlockBuilder::AddKey", "out of memory");
  }
  if (!cldb_buf_append(&builder->keys, key.data, key.size)) {
    return cldb_status_io("FilterBlockBuilder::AddKey", "out of memory");
  }
  builder->starts[builder->starts_count] = key_offset;
  ++builder->starts_count;
  return cldb_status_ok();
}

cldb_status_t cldb_filter_block_builder_finish(
    cldb_filter_block_builder_t *builder, cldb_slice_t *block) {
  size_t array_offset;
  size_t payload_size;
  size_t total_size;
  size_t count;
  size_t i;
  char *dst;

  if (block == NULL) {
    return cldb_status_invalid_argument("FilterBlockBuilder::Finish",
                                        "block is NULL");
  }
  *block = cldb_slice_empty();
  if (!builder_is_ready(builder) || builder->finished) {
    return cldb_status_invalid_argument("FilterBlockBuilder::Finish",
                                        "builder is not usable");
  }
  if (builder->starts_count != 0) {
    cldb_status_t status = generate_filter(builder);

    if (!cldb_status_is_ok(status)) {
      return status;
    }
  }
  if (builder->result.size > UINT32_MAX) {
    return cldb_status_io("FilterBlockBuilder::Finish",
                          "filter block exceeds fixed32 offsets");
  }

  array_offset = builder->result.size;
  count = builder->filter_offsets_count;
  /* offsets 数组 + array_offset + base_lg；再加 NUL 仍在 size_t 内。 */
  if (count > (SIZE_MAX - 6U) / sizeof(uint32_t) ||
      array_offset > SIZE_MAX - 6U - count * sizeof(uint32_t)) {
    return cldb_status_io("FilterBlockBuilder::Finish",
                          "filter block size overflows");
  }
  payload_size = count * sizeof(uint32_t) + 5U;
  total_size = array_offset + payload_size;
  if (!cldb_buf_reserve(&builder->result, total_size + 1U)) {
    return cldb_status_io("FilterBlockBuilder::Finish", "out of memory");
  }

  dst = builder->result.data + array_offset;
  for (i = 0; i < count; ++i) {
    cldb_encode_fixed32(dst, builder->filter_offsets[i]);
    dst += sizeof(uint32_t);
  }
  cldb_encode_fixed32(dst, (uint32_t)array_offset);
  dst += sizeof(uint32_t);
  *dst = (char)CLDB_FILTER_BLOCK_BASE_LG;
  builder->result.size = total_size;
  builder->result.data[total_size] = '\0';
  builder->finished = true;
  *block = cldb_buf_slice(&builder->result);
  return cldb_status_ok();
}

void cldb_filter_block_reader_init(cldb_filter_block_reader_t *reader,
                                   const cldb_filter_policy_t *policy,
                                   cldb_slice_t contents) {
  size_t n;
  uint32_t array_offset;

  if (reader == NULL) {
    return;
  }
  memset(reader, 0, sizeof(*reader));
  if (policy == NULL || policy->vtbl == NULL ||
      policy->vtbl->key_may_match == NULL) {
    return;
  }
  reader->policy = policy;
  if (contents.data == NULL || contents.size < 5U) {
    return;
  }
  n = contents.size;
  array_offset = cldb_decode_fixed32(contents.data + n - 5U);
  if ((size_t)array_offset > n - 5U) {
    return;
  }

  reader->data = contents.data;
  reader->offsets = contents.data + array_offset;
  reader->data_size = (size_t)array_offset;
  reader->num_offsets = (n - 5U - (size_t)array_offset) / sizeof(uint32_t);
  reader->base_lg = (uint8_t)contents.data[n - 1U];
}

bool cldb_filter_block_reader_key_may_match(
    const cldb_filter_block_reader_t *reader, uint64_t block_offset,
    cldb_slice_t key) {
  uint64_t index;
  size_t i;
  uint32_t start;
  uint32_t limit;

  if (reader == NULL || reader->policy == NULL || reader->data == NULL) {
    return true;
  }
  if (!cldb_slice_normalize(&key)) {
    return true;
  }
  /* 超宽移位是未定义行为；损坏的 base_lg 一律保守放行。 */
  if (reader->base_lg >= 64U) {
    return true;
  }
  index = block_offset >> reader->base_lg;
  if (index >= (uint64_t)reader->num_offsets) {
    return true;
  }

  i = (size_t)index;
  start = cldb_decode_fixed32(reader->offsets + i * sizeof(uint32_t));
  limit = cldb_decode_fixed32(reader->offsets + (i + 1U) * sizeof(uint32_t));
  if (start <= limit && (size_t)limit <= reader->data_size) {
    cldb_slice_t filter = cldb_slice_make(
        reader->data + start, (size_t)(limit - start));

    return cldb_filter_policy_key_may_match(reader->policy, key, filter);
  }
  /* 区间越界或损坏时放行；只有在数据范围内的空过滤器才交给策略判负。 */
  return true;
}
