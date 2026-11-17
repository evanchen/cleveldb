#include "table/block.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cleveldb/buf.h"
#include "port/port.h"
#include "util/coding.h"

/* 解析 BlockBuilder 生成的条目：varint32 shared | varint32 non_shared |
 * varint32 value_length | key 后缀 | value。返回 key 后缀起点；越界或
 * varint 非法返回 NULL。limit 指向重启数组起点，调用期间不解引用其后。 */
static const char *decode_entry(const char *p, const char *limit,
                                uint32_t *shared, uint32_t *non_shared,
                                uint32_t *value_length) {
  size_t available;
  size_t remaining;

  if (p == NULL || limit == NULL || p > limit || shared == NULL ||
      non_shared == NULL || value_length == NULL) {
    return NULL;
  }
  available = (size_t)(limit - p);
  if (available < 3) {
    return NULL;
  }

  *shared = (uint32_t)(uint8_t)p[0];
  *non_shared = (uint32_t)(uint8_t)p[1];
  *value_length = (uint32_t)(uint8_t)p[2];
  if ((*shared | *non_shared | *value_length) < 128U) {
    p += 3;
  } else {
    p = cldb_get_varint32_ptr(p, limit, shared);
    if (p == NULL) {
      return NULL;
    }
    p = cldb_get_varint32_ptr(p, limit, non_shared);
    if (p == NULL) {
      return NULL;
    }
    p = cldb_get_varint32_ptr(p, limit, value_length);
    if (p == NULL) {
      return NULL;
    }
  }

  remaining = (size_t)(limit - p);
  if ((size_t)*non_shared > remaining ||
      (size_t)*value_length > remaining - (size_t)*non_shared) {
    return NULL;
  }
  return p;
}

cldb_status_t cldb_block_init(cldb_block_t *block,
                              cldb_block_contents_t contents) {
  size_t max_restarts;
  uint32_t num_restarts;
  size_t trailer_size;

  if (block == NULL) {
    return cldb_status_invalid_argument("Block::Block", "block is NULL");
  }
  memset(block, 0, sizeof(*block));
  if (!cldb_slice_is_valid(contents.data)) {
    return cldb_status_invalid_argument("Block::Block",
                                        "invalid block contents");
  }

  block->data = contents.data;
  block->cachable = contents.cachable;
  block->heap_allocated = contents.heap_allocated;
  if (block->data.size < sizeof(uint32_t)) {
    block->data.size = 0; /* 与上游一致：用 size=0 标记头部损坏 */
    return cldb_status_ok();
  }

  num_restarts = cldb_decode_fixed32(block->data.data + block->data.size -
                                     sizeof(uint32_t));
  max_restarts = (block->data.size - sizeof(uint32_t)) / sizeof(uint32_t);
  if ((size_t)num_restarts > max_restarts) {
    block->data.size = 0;
    return cldb_status_ok();
  }
  trailer_size = ((size_t)num_restarts + 1U) * sizeof(uint32_t);
  block->num_restarts = num_restarts;
  block->restart_offset = block->data.size - trailer_size;
  return cldb_status_ok();
}

void cldb_block_destroy(cldb_block_t *block) {
  if (block == NULL) {
    return;
  }
  if (block->heap_allocated && block->data.data != NULL) {
    free((void *)block->data.data);
  }
  memset(block, 0, sizeof(*block));
}

size_t cldb_block_size(const cldb_block_t *block) {
  return block != NULL ? block->data.size : 0;
}

bool cldb_block_is_cachable(const cldb_block_t *block) {
  return block != NULL && block->cachable;
}

typedef struct cldb_block_iter {
  cldb_iterator_t base; /* 首成员，vtable 回调可转回本类型 */
  const cldb_comparator_t *comparator; /* 仅引用，须比迭代器活得久 */
  const char *data;                    /* 仅引用块数据 */
  size_t data_size;
  size_t restart_offset; /* 条目区末尾，也是 restart 数组起点 */
  uint32_t num_restarts;
  size_t current;       /* 当前条目起点；>= restart_offset 表示无效 */
  size_t restart_index; /* current 所在的 restart 组 */
  cldb_buf_t key;       /* 当前重建出的完整 key */
  cldb_slice_t value;   /* 指向块内当前 value */
  cldb_status_t status;
} cldb_block_iter_t;

static bool block_iter_valid(const cldb_iterator_t *iterator) {
  const cldb_block_iter_t *self = (const cldb_block_iter_t *)iterator;

  return self->current < self->restart_offset;
}

static cldb_slice_t block_iter_key(const cldb_iterator_t *iterator) {
  const cldb_block_iter_t *self = (const cldb_block_iter_t *)iterator;

  CLDB_ASSERT(block_iter_valid(iterator));
  if (!block_iter_valid(iterator)) {
    return cldb_slice_empty();
  }
  return cldb_buf_slice(&self->key);
}

static cldb_slice_t block_iter_value(const cldb_iterator_t *iterator) {
  const cldb_block_iter_t *self = (const cldb_block_iter_t *)iterator;

  CLDB_ASSERT(block_iter_valid(iterator));
  if (!block_iter_valid(iterator)) {
    return cldb_slice_empty();
  }
  return self->value;
}

static cldb_status_t block_iter_status(const cldb_iterator_t *iterator) {
  const cldb_block_iter_t *self = (const cldb_block_iter_t *)iterator;

  return self->status;
}

/* 正常耗尽：无效但不设置错误状态，与上游只改 current_/restart_index_ 一致。 */
static void block_iter_invalidate(cldb_block_iter_t *self) {
  self->current = self->restart_offset;
  self->restart_index = self->num_restarts;
}

static void block_iter_set_error(cldb_block_iter_t *self,
                                 cldb_status_t status) {
  self->current = self->restart_offset;
  self->restart_index = self->num_restarts;
  self->status = status;
  cldb_buf_clear(&self->key);
  self->value = cldb_slice_empty();
}

static void block_iter_corruption(cldb_block_iter_t *self) {
  block_iter_set_error(
      self, cldb_status_corruption("Block::Iter", "bad entry in block"));
}

/* 读取第 index 个 restart 偏移并检查它落在条目区内；越界视为损坏。 */
static bool block_iter_restart_point(const cldb_block_iter_t *self,
                                     size_t index, size_t *offset) {
  const char *entry;
  uint32_t raw_offset;

  if (index >= (size_t)self->num_restarts) {
    return false;
  }
  entry = self->data + self->restart_offset + index * sizeof(uint32_t);
  raw_offset = cldb_decode_fixed32(entry);
  if ((size_t)raw_offset >= self->restart_offset) {
    return false;
  }
  *offset = (size_t)raw_offset;
  return true;
}

/* 当前条目末尾偏移；value 始终指向 data 内。 */
static size_t block_iter_next_entry_offset(const cldb_block_iter_t *self) {
  CLDB_ASSERT(self->value.data >= self->data);
  CLDB_ASSERT(self->value.data + self->value.size <=
              self->data + self->data_size);
  return (size_t)(self->value.data - self->data) + self->value.size;
}

/* 把 key 恢复为空并把 value 指到 restart 条目起点，等待 ParseNextKey。 */
static bool block_iter_seek_to_restart(cldb_block_iter_t *self,
                                       size_t index) {
  size_t offset;

  if (!block_iter_restart_point(self, index, &offset)) {
    block_iter_corruption(self);
    return false;
  }
  cldb_buf_clear(&self->key);
  self->restart_index = index;
  self->value = cldb_slice_make(self->data + offset, 0);
  return true;
}

/* 从 value 末尾解析下一条；正常到达条目区末尾时仅置无效。 */
static bool block_iter_parse_next(cldb_block_iter_t *self) {
  const char *p;
  const char *limit;
  const char *key_delta;
  uint32_t shared;
  uint32_t non_shared;
  uint32_t value_length;
  size_t key_size;
  size_t next_restart;

  self->current = block_iter_next_entry_offset(self);
  p = self->data + self->current;
  limit = self->data + self->restart_offset;
  if (p >= limit) {
    block_iter_invalidate(self);
    return false;
  }

  key_delta =
      decode_entry(p, limit, &shared, &non_shared, &value_length);
  if (key_delta == NULL || (size_t)shared > self->key.size) {
    block_iter_corruption(self);
    return false;
  }
  key_size = (size_t)shared + (size_t)non_shared;
  if (key_size < (size_t)shared || key_size < (size_t)non_shared ||
      key_size == SIZE_MAX) {
    block_iter_corruption(self);
    return false;
  }
  if (!cldb_buf_reserve(&self->key, key_size + 1U)) {
    block_iter_set_error(
        self, cldb_status_io("Block::Iter", "out of memory"));
    return false;
  }

  self->key.size = (size_t)shared;
  if (non_shared != 0) {
    memcpy(self->key.data + self->key.size, key_delta, non_shared);
    self->key.size += non_shared;
  }
  self->key.data[self->key.size] = '\0';
  self->value = cldb_slice_make(key_delta + non_shared, value_length);

  while (self->restart_index + 1U < self->num_restarts) {
    if (!block_iter_restart_point(self, self->restart_index + 1U,
                                  &next_restart)) {
      block_iter_corruption(self);
      return false;
    }
    if (next_restart >= self->current) {
      break;
    }
    ++self->restart_index;
  }
  return true;
}

static void block_iter_next(cldb_iterator_t *iterator) {
  cldb_block_iter_t *self = (cldb_block_iter_t *)iterator;

  CLDB_ASSERT(block_iter_valid(iterator));
  if (block_iter_valid(iterator)) {
    (void)block_iter_parse_next(self);
  }
}

static void block_iter_prev(cldb_iterator_t *iterator) {
  cldb_block_iter_t *self = (cldb_block_iter_t *)iterator;
  size_t original;

  CLDB_ASSERT(block_iter_valid(iterator));
  if (!block_iter_valid(iterator)) {
    return;
  }
  original = self->current;
  while (true) {
    size_t restart;

    if (!block_iter_restart_point(self, self->restart_index, &restart)) {
      block_iter_corruption(self);
      return;
    }
    if (restart < original) {
      break;
    }
    if (self->restart_index == 0) {
      block_iter_invalidate(self);
      return;
    }
    --self->restart_index;
  }
  if (!block_iter_seek_to_restart(self, self->restart_index)) {
    return;
  }
  do {
    if (!block_iter_parse_next(self)) {
      return;
    }
  } while (block_iter_next_entry_offset(self) < original);
}

static void block_iter_seek(cldb_iterator_t *iterator, cldb_slice_t target) {
  cldb_block_iter_t *self = (cldb_block_iter_t *)iterator;
  size_t left;
  size_t right;
  int current_key_compare = 0;
  bool skip_seek;

  if (block_iter_valid(iterator)) {
    current_key_compare =
        cldb_comparator_compare(self->comparator, cldb_buf_slice(&self->key),
                                target);
    if (current_key_compare == 0) {
      return;
    }
  }

  left = 0;
  right = (size_t)self->num_restarts - 1U;
  if (block_iter_valid(iterator)) {
    if (current_key_compare < 0) {
      left = self->restart_index;
    } else {
      right = self->restart_index;
    }
  }

  while (left < right) {
    size_t mid = left + (right - left + 1U) / 2U;
    size_t restart;
    const char *mid_key;
    uint32_t shared;
    uint32_t non_shared;
    uint32_t value_length;

    if (!block_iter_restart_point(self, mid, &restart)) {
      block_iter_corruption(self);
      return;
    }
    mid_key = decode_entry(self->data + restart,
                           self->data + self->restart_offset, &shared,
                           &non_shared, &value_length);
    if (mid_key == NULL || shared != 0) {
      block_iter_corruption(self);
      return;
    }
    if (cldb_comparator_compare(self->comparator,
                                cldb_slice_make(mid_key, non_shared),
                                target) < 0) {
      left = mid;
    } else {
      right = mid - 1U;
    }
  }

  skip_seek =
      left == self->restart_index && current_key_compare < 0;
  if (!skip_seek && !block_iter_seek_to_restart(self, left)) {
    return;
  }
  while (block_iter_parse_next(self)) {
    if (cldb_comparator_compare(self->comparator,
                                cldb_buf_slice(&self->key), target) >= 0) {
      return;
    }
  }
}

static void block_iter_seek_to_first(cldb_iterator_t *iterator) {
  cldb_block_iter_t *self = (cldb_block_iter_t *)iterator;

  if (block_iter_seek_to_restart(self, 0)) {
    (void)block_iter_parse_next(self);
  }
}

static void block_iter_seek_to_last(cldb_iterator_t *iterator) {
  cldb_block_iter_t *self = (cldb_block_iter_t *)iterator;

  if (!block_iter_seek_to_restart(self, (size_t)self->num_restarts - 1U)) {
    return;
  }
  while (block_iter_parse_next(self) &&
         block_iter_next_entry_offset(self) < self->restart_offset) {
    /* 继续移动到条目区最后一个条目 */
  }
}

static void block_iter_destroy(cldb_iterator_t *iterator) {
  cldb_block_iter_t *self = (cldb_block_iter_t *)iterator;

  cldb_buf_free(&self->key);
  cldb_iter_run_cleanup(iterator);
  free(self);
}

static const cldb_iterator_vtbl_t kBlockIterVtbl = {
    .valid = block_iter_valid,
    .seek_to_first = block_iter_seek_to_first,
    .seek_to_last = block_iter_seek_to_last,
    .seek = block_iter_seek,
    .next = block_iter_next,
    .prev = block_iter_prev,
    .key = block_iter_key,
    .value = block_iter_value,
    .status = block_iter_status,
    .destroy = block_iter_destroy,
};

cldb_iterator_t *cldb_block_new_iterator(const cldb_block_t *block,
                                         const cldb_comparator_t *comparator) {
  cldb_block_iter_t *iterator;

  if (block == NULL) {
    return NULL;
  }
  if (block->data.size == 0) {
    return cldb_new_error_iterator(
        cldb_status_corruption("Block::NewIterator", "bad block contents"));
  }
  if (block->num_restarts == 0 || block->restart_offset == 0) {
    return cldb_new_empty_iterator();
  }

  iterator = malloc(sizeof(*iterator));
  if (iterator == NULL) {
    return NULL;
  }
  iterator->comparator = comparator != NULL ? comparator
                                            : cldb_bytewise_comparator();
  iterator->data = block->data.data;
  iterator->data_size = block->data.size;
  iterator->restart_offset = block->restart_offset;
  iterator->num_restarts = block->num_restarts;
  iterator->current = iterator->restart_offset;
  iterator->restart_index = iterator->num_restarts;
  cldb_buf_init(&iterator->key);
  iterator->value =
      cldb_slice_make(iterator->data + iterator->restart_offset, 0);
  iterator->status = cldb_status_ok();
  cldb_iter_init(&iterator->base, &kBlockIterVtbl);
  return &iterator->base;
}
