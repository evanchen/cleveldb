#include "db/memtable.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "db/skiplist.h"
#include "util/arena.h"
#include "util/coding.h"

/* 内存表实现；对应上游 db/memtable.cc。条目和跳表节点都由内部 Arena
 * 统一释放；refs 使用原子计数以支持跨线程 unref。 */

enum { kVarint32MaxBytes = 5 };

struct cldb_memtable {
  cldb_internal_key_comparator_t comparator; /* 按值复制；user 比较器仅引用 */
  cldb_atomic_u32_t refs; /* 初始为 1；最后一个 unref 释放对象 */
  cldb_arena_t arena;     /* 拥有条目与跳表节点 */
  cldb_buf_t scratch;     /* Add 的临时条目缓冲；仅写者使用 */
  cldb_skiplist_t table;  /* compare_arg 指向本对象 */
};

/* 读取条目开头的 varint32 长度及其内容；对应上游 GetLengthPrefixedSlice。
 * 条目由 Add 生成，假定长度前缀完整，因此最多检查 5 字节前缀。 */
static bool entry_length_prefixed_slice(const char *entry,
                                        cldb_slice_t *result) {
  uint32_t length;
  const char *p;

  if (entry == NULL || result == NULL) {
    return false;
  }
  p = cldb_get_varint32_ptr(entry, entry + kVarint32MaxBytes, &length);
  if (p == NULL) {
    return false;
  }
  *result = cldb_slice_make(p, (size_t)length);
  return true;
}

/* SkipList 比较回调；先剥去条目长度前缀再交给内部键比较器，对应上游
 * MemTable::KeyComparator::operator()。 */
static int memtable_compare(void *arg, const char *a, const char *b) {
  const cldb_memtable_t *memtable = arg;
  cldb_slice_t akey;
  cldb_slice_t bkey;

  if (memtable == NULL || !entry_length_prefixed_slice(a, &akey) ||
      !entry_length_prefixed_slice(b, &bkey)) {
    CLDB_ASSERT(false);
    return 0;
  }
  return cldb_comparator_compare(&memtable->comparator.base, akey, bkey);
}

/* 释放全部资源；只在引用计数归零后调用。跳表节点由 arena 释放，无需
 * 单独销毁。 */
static void memtable_destroy(cldb_memtable_t *memtable) {
  CLDB_ASSERT(cldb_atomic_u32_load(&memtable->refs, memory_order_relaxed) ==
              0);
  cldb_buf_free(&memtable->scratch);
  cldb_arena_destroy(&memtable->arena);
  free(memtable);
}

cldb_memtable_t *cldb_memtable_create(
    const cldb_internal_key_comparator_t *comparator) {
  cldb_memtable_t *memtable;

  if (comparator == NULL ||
      cldb_internal_key_comparator_user_comparator(comparator) == NULL) {
    return NULL;
  }
  memtable = malloc(sizeof(*memtable));
  if (memtable == NULL) {
    return NULL;
  }
  cldb_internal_key_comparator_init(
      &memtable->comparator,
      cldb_internal_key_comparator_user_comparator(comparator));
  cldb_atomic_u32_init(&memtable->refs, 1);
  cldb_arena_init(&memtable->arena);
  cldb_buf_init(&memtable->scratch);
  if (!cldb_skiplist_init(&memtable->table, &memtable->arena,
                          memtable_compare, memtable)) {
    cldb_buf_free(&memtable->scratch);
    cldb_arena_destroy(&memtable->arena);
    free(memtable);
    return NULL;
  }
  return memtable;
}

void cldb_memtable_ref(cldb_memtable_t *memtable) {
  uint32_t old;

  if (memtable == NULL) {
    return;
  }
  old = cldb_atomic_u32_fetch_add(&memtable->refs, 1, memory_order_relaxed);
  CLDB_ASSERT(old > 0 && old < UINT32_MAX);
  CLDB_UNUSED(old);
}

void cldb_memtable_unref(cldb_memtable_t *memtable) {
  uint32_t old;

  if (memtable == NULL) {
    return;
  }
  old = atomic_fetch_sub_explicit(&memtable->refs, 1, memory_order_acq_rel);
  CLDB_ASSERT(old > 0);
  if (old == 1) {
    memtable_destroy(memtable);
  }
}

size_t cldb_memtable_approximate_memory_usage(
    const cldb_memtable_t *memtable) {
  return memtable != NULL ? cldb_arena_memory_usage(&memtable->arena) : 0;
}

cldb_status_t cldb_memtable_add(cldb_memtable_t *memtable,
                                cldb_sequence_number_t sequence,
                                cldb_value_type_t type, cldb_slice_t key,
                                cldb_slice_t value) {
  uint32_t internal_key_size;
  char *entry;
  cldb_status_t status;

  if (memtable == NULL || sequence > CLDB_MAX_SEQUENCE_NUMBER ||
      (type != CLDB_VALUE_TYPE_VALUE &&
       type != CLDB_VALUE_TYPE_DELETION) ||
      !cldb_slice_is_valid(key) || !cldb_slice_is_valid(value) ||
      key.size > (size_t)UINT32_MAX - CLDB_INTERNAL_KEY_TAG_SIZE ||
      value.size > (size_t)UINT32_MAX) {
    return cldb_status_invalid_argument("MemTable::Add",
                                        "invalid argument");
  }

  internal_key_size =
      (uint32_t)(key.size + CLDB_INTERNAL_KEY_TAG_SIZE);
  cldb_buf_clear(&memtable->scratch);
  if (!cldb_put_varint32(&memtable->scratch, internal_key_size) ||
      (key.size > 0 &&
       !cldb_buf_append(&memtable->scratch, key.data, key.size)) ||
      !cldb_put_fixed64(
          &memtable->scratch,
          (sequence << 8) | (uint64_t)(unsigned int)type) ||
      !cldb_put_varint32(&memtable->scratch, (uint32_t)value.size) ||
      (value.size > 0 &&
       !cldb_buf_append(&memtable->scratch, value.data, value.size))) {
    return cldb_status_io("MemTable::Add", "out of memory");
  }

  entry = cldb_arena_allocate(&memtable->arena, memtable->scratch.size);
  if (entry == NULL) {
    return cldb_status_io("MemTable::Add", "out of memory");
  }
  memcpy(entry, memtable->scratch.data, memtable->scratch.size);

  if (!cldb_skiplist_insert(&memtable->table, entry)) {
    if (cldb_skiplist_contains(&memtable->table, entry)) {
      status = cldb_status_invalid_argument("MemTable::Add",
                                            "duplicate internal key");
    } else {
      status = cldb_status_io("MemTable::Add", "out of memory");
    }
    return status;
  }
  return cldb_status_ok();
}

bool cldb_memtable_get(const cldb_memtable_t *memtable,
                       const cldb_lookup_key_t *key, cldb_slice_t *value,
                       cldb_status_t *status) {
  cldb_slice_t memkey;
  cldb_slice_t lookup_user;
  cldb_skiplist_iterator_t iter;
  const char *entry;
  cldb_slice_t entry_key;
  cldb_parsed_internal_key_t entry_parsed;

  if (value == NULL || status == NULL) {
    return false;
  }
  *value = cldb_slice_empty();
  *status = cldb_status_ok();
  if (memtable == NULL || key == NULL) {
    *status = cldb_status_invalid_argument("MemTable::Get",
                                           "invalid argument");
    return false;
  }

  /* lookup key 由 cldb_lookup_key_init 内部构造，格式可信；直接取
   * memtable key 与 user_key 视图，对应上游信任 LookupKey 的写法。 */
  memkey = cldb_lookup_key_memtable_key(key);
  lookup_user = cldb_lookup_key_user_key(key);

  cldb_skiplist_iterator_init(&iter, &memtable->table);
  cldb_skiplist_iterator_seek(&iter, memkey.data);
  if (!cldb_skiplist_iterator_valid(&iter)) {
    return false;
  }

  entry = cldb_skiplist_iterator_key(&iter);
  if (!entry_length_prefixed_slice(entry, &entry_key) ||
      !cldb_parse_internal_key(entry_key, &entry_parsed)) {
    *status = cldb_status_corruption("MemTable::Get", "corrupt entry");
    return false;
  }
  if (cldb_comparator_compare(
          cldb_internal_key_comparator_user_comparator(&memtable->comparator),
          entry_parsed.user_key, lookup_user) != 0) {
    return false;
  }

  /* Seek 已跳过 tag 大于 lookup 的条目，因此命中项一定是可见的最新版本。 */
  if (entry_parsed.type == CLDB_VALUE_TYPE_VALUE) {
    cldb_slice_t entry_value;

    if (!entry_length_prefixed_slice(entry_key.data + entry_key.size,
                                     &entry_value)) {
      *status = cldb_status_corruption("MemTable::Get", "corrupt value");
      return false;
    }
    *value = entry_value;
    return true;
  }
  if (entry_parsed.type == CLDB_VALUE_TYPE_DELETION) {
    *status = cldb_status_not_found("MemTable::Get", "deleted");
    return true;
  }
  *status = cldb_status_corruption("MemTable::Get", "corrupt entry");
  return false;
}

/* 迭代器外层的具体类型；base 必须为首成员。 */
typedef struct cldb_memtable_iterator {
  cldb_iterator_t base;          /* 必须为首成员 */
  cldb_skiplist_iterator_t iter; /* 跳表位置 */
  cldb_buf_t tmp;                /* Seek 的长度前缀临时缓冲 */
  cldb_status_t status;          /* 仅供 Seek 参数错误或分配失败 */
} cldb_memtable_iterator_t;

static cldb_memtable_iterator_t *memtable_iterator_self(
    const cldb_iterator_t *iterator) {
  return (cldb_memtable_iterator_t *)(void *)iterator;
}

static bool memtable_iterator_valid(const cldb_iterator_t *iterator) {
  const cldb_memtable_iterator_t *self = memtable_iterator_self(iterator);

  return cldb_skiplist_iterator_valid(&self->iter);
}

static void memtable_iterator_seek(cldb_iterator_t *iterator,
                                   cldb_slice_t target) {
  cldb_memtable_iterator_t *self = memtable_iterator_self(iterator);
  cldb_parsed_internal_key_t parsed;
  const char *entry;

  self->status = cldb_status_ok();
  if (!cldb_slice_is_valid(target) ||
      !cldb_parse_internal_key(target, &parsed)) {
    cldb_skiplist_iterator_seek(&self->iter, NULL);
    self->status = cldb_status_invalid_argument("MemTableIterator::Seek",
                                                "invalid internal key");
    return;
  }
  cldb_buf_clear(&self->tmp);
  if (!cldb_put_length_prefixed_slice(&self->tmp, target)) {
    cldb_skiplist_iterator_seek(&self->iter, NULL);
    self->status = cldb_status_io("MemTableIterator::Seek", "out of memory");
    return;
  }
  entry = cldb_buf_slice(&self->tmp).data;
  cldb_skiplist_iterator_seek(&self->iter, entry);
}

static void memtable_iterator_seek_to_first(cldb_iterator_t *iterator) {
  cldb_memtable_iterator_t *self = memtable_iterator_self(iterator);

  self->status = cldb_status_ok();
  cldb_skiplist_iterator_seek_to_first(&self->iter);
}

static void memtable_iterator_seek_to_last(cldb_iterator_t *iterator) {
  cldb_memtable_iterator_t *self = memtable_iterator_self(iterator);

  self->status = cldb_status_ok();
  cldb_skiplist_iterator_seek_to_last(&self->iter);
}

static void memtable_iterator_next(cldb_iterator_t *iterator) {
  cldb_memtable_iterator_t *self = memtable_iterator_self(iterator);

  CLDB_ASSERT(cldb_skiplist_iterator_valid(&self->iter));
  self->status = cldb_status_ok();
  cldb_skiplist_iterator_next(&self->iter);
}

static void memtable_iterator_prev(cldb_iterator_t *iterator) {
  cldb_memtable_iterator_t *self = memtable_iterator_self(iterator);

  CLDB_ASSERT(cldb_skiplist_iterator_valid(&self->iter));
  self->status = cldb_status_ok();
  cldb_skiplist_iterator_prev(&self->iter);
}

static cldb_slice_t memtable_iterator_key(const cldb_iterator_t *iterator) {
  const cldb_memtable_iterator_t *self = memtable_iterator_self(iterator);
  const char *entry;
  cldb_slice_t key;

  CLDB_ASSERT(cldb_skiplist_iterator_valid(&self->iter));
  if (!cldb_skiplist_iterator_valid(&self->iter)) {
    return cldb_slice_empty();
  }
  entry = cldb_skiplist_iterator_key(&self->iter);
  if (!entry_length_prefixed_slice(entry, &key)) {
    CLDB_ASSERT(false);
    return cldb_slice_empty();
  }
  return key;
}

static cldb_slice_t memtable_iterator_value(
    const cldb_iterator_t *iterator) {
  cldb_slice_t key;
  cldb_slice_t value;

  key = memtable_iterator_key(iterator);
  if (key.size < CLDB_INTERNAL_KEY_TAG_SIZE) {
    CLDB_ASSERT(false);
    return cldb_slice_empty();
  }
  if (!entry_length_prefixed_slice(key.data + key.size, &value)) {
    CLDB_ASSERT(false);
    return cldb_slice_empty();
  }
  return value;
}

static cldb_status_t memtable_iterator_status(
    const cldb_iterator_t *iterator) {
  return memtable_iterator_self(iterator)->status;
}

static void memtable_iterator_destroy(cldb_iterator_t *iterator) {
  cldb_memtable_iterator_t *self = memtable_iterator_self(iterator);

  cldb_buf_free(&self->tmp);
  cldb_iter_run_cleanup(iterator);
  free(self);
}

static const cldb_iterator_vtbl_t kMemTableIteratorVtbl = {
    .valid = memtable_iterator_valid,
    .seek_to_first = memtable_iterator_seek_to_first,
    .seek_to_last = memtable_iterator_seek_to_last,
    .seek = memtable_iterator_seek,
    .next = memtable_iterator_next,
    .prev = memtable_iterator_prev,
    .key = memtable_iterator_key,
    .value = memtable_iterator_value,
    .status = memtable_iterator_status,
    .destroy = memtable_iterator_destroy,
};

cldb_iterator_t *cldb_memtable_new_iterator(
    const cldb_memtable_t *memtable) {
  cldb_memtable_iterator_t *iterator;

  if (memtable == NULL) {
    return NULL;
  }
  iterator = malloc(sizeof(*iterator));
  if (iterator == NULL) {
    return NULL;
  }
  cldb_buf_init(&iterator->tmp);
  cldb_skiplist_iterator_init(&iterator->iter, &memtable->table);
  iterator->status = cldb_status_ok();
  cldb_iter_init(&iterator->base, &kMemTableIteratorVtbl);
  return &iterator->base;
}
