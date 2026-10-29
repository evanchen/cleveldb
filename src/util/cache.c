#include "cleveldb/cache.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cleveldb/status.h"
#include "port/port.h"
#include "port/port_atomic.h"
#include "port/port_mutex.h"
#include "util/hash.h"

/* 分片 LRU cache；对应上游 util/cache.cc。每个 shard 独立加锁，key 的哈希
 * 高 4 位决定 shard，避免全局锁争用。条目在 cache 内时同时受 shard 锁和
 * refs 引用计数保护。 */

enum { kNumShardBits = 4, kNumShards = 1 << kNumShardBits };

/* 环形双向链表节点；作为句柄首成员或独立哨兵，对应上游 next/prev。 */
typedef struct cldb_lru_link {
  struct cldb_lru_link *next;
  struct cldb_lru_link *prev;
} cldb_lru_link_t;

typedef struct cldb_lru_handle cldb_lru_handle_t;

/* 缓存条目；对应上游 LRUHandle。key 紧跟在结构体后一次分配。refs 包含
 * cache 自身的引用（若 in_cache），最后一个客户端 handle 释放后归零。
 * link 必须为首成员，以便链表哨兵与条目互转。 */
struct cldb_lru_handle {
  cldb_lru_link_t link; /* LRU/in-use 链表节点；对应上游 next/prev */
  void *value; /* 由 deleter 最终回收 */
  cldb_cache_deleter_fn_t deleter; /* 可为 NULL；持 shard 锁调用 */
  void *deleter_arg; /* 仅引用；与 deleter 一起保存 */
  cldb_lru_handle_t *next_hash; /* 同桶哈希链 */
  size_t charge; /* 计入 shard usage 的容量 */
  size_t key_size; /* key_data 的字节数 */
  bool in_cache; /* cache 是否仍持有引用 */
  uint32_t refs; /* 引用数；受 shard 锁保护 */
  uint32_t hash; /* key 哈希；shard 与桶定位 */
  char key_data[]; /* key 副本；无 NUL 结尾 */
};

/* 分离链式哈希表；对应上游 HandleTable。所有读写都要求持有所属 shard 锁，
 * 桶数量始终为 2 的幂。 */
typedef struct cldb_handle_table {
  size_t length; /* 桶数量；2 的幂 */
  size_t elems; /* 条目数量 */
  cldb_lru_handle_t **buckets; /* 由本对象释放；length 个桶 */
} cldb_handle_table_t;

typedef struct cldb_lru_cache_shard {
  cldb_mutex_t mutex; /* 保护本 shard 的所有字段、链表和哈希表 */
  size_t capacity; /* 0 表示不缓存；GUARDED_BY(mutex) */
  size_t usage; /* 已缓存条目的 charge 之和；GUARDED_BY(mutex) */
  cldb_lru_link_t lru; /* LRU 哨兵；next 最旧、prev 最新，元素 refs==1 */
  cldb_lru_link_t in_use; /* 客户端引用哨兵；元素 refs>=2 */
  cldb_handle_table_t table; /* GUARDED_BY(mutex) */
} cldb_lru_cache_shard_t;

typedef struct cldb_sharded_lru_cache {
  cldb_cache_t base; /* 必须为首成员，便于 vtable 回调转回本类型 */
  cldb_atomic_u64_t last_id; /* NewId 计数；原子递增 */
  cldb_lru_cache_shard_t shards[kNumShards]; /* 每个 shard 独立锁与链表 */
} cldb_sharded_lru_cache_t;

/* 把首成员为 link 的条目转回句柄；两者地址相同。 */
static cldb_lru_handle_t *handle_from_link(cldb_lru_link_t *link) {
  return (cldb_lru_handle_t *)link;
}

static void lru_link_init(cldb_lru_link_t *link) {
  link->next = link;
  link->prev = link;
}

/* REQUIRES: link 已在某个链表中。 */
static void lru_link_remove(cldb_lru_link_t *link) {
  link->prev->next = link->next;
  link->next->prev = link->prev;
}

/* 把 link 插到 head 之前，使其成为最新元素。 */
static void lru_link_append(cldb_lru_link_t *head, cldb_lru_link_t *link) {
  link->next = head;
  link->prev = head->prev;
  link->prev->next = link;
  link->next->prev = link;
}

static cldb_slice_t handle_key(const cldb_lru_handle_t *entry) {
  return cldb_slice_make(entry->key_data, entry->key_size);
}

/* 初始化空表；对应上游 HandleTable 构造。失败返回 false。 */
static bool handle_table_init(cldb_handle_table_t *table) {
  table->length = 4;
  table->elems = 0;
  table->buckets = calloc(table->length, sizeof(*table->buckets));
  return table->buckets != NULL;
}

static void handle_table_destroy(cldb_handle_table_t *table) {
  free(table->buckets);
  table->buckets = NULL;
  table->length = 0;
  table->elems = 0;
}

/* REQUIRES: 表锁已持有。返回指向匹配槽的指针；无匹配时指向链尾槽。 */
static cldb_lru_handle_t **handle_table_find_locked(
    cldb_handle_table_t *table, cldb_slice_t key, uint32_t hash) {
  cldb_lru_handle_t **ptr =
      &table->buckets[(size_t)hash & (table->length - 1)];

  while (*ptr != NULL) {
    cldb_lru_handle_t *entry = *ptr;

    if (entry->hash == hash && entry->key_size == key.size &&
        (key.size == 0 || memcmp(entry->key_data, key.data, key.size) == 0)) {
      break;
    }
    ptr = &entry->next_hash;
  }
  return ptr;
}

/* REQUIRES: 表锁已持有。扩容失败时保留旧桶，只退化为较长链，不影响正确性。 */
static void handle_table_resize_locked(cldb_handle_table_t *table) {
  size_t new_length = 4;
  cldb_lru_handle_t **new_buckets;
  size_t i;

  while (new_length < table->elems) {
    if (new_length > SIZE_MAX / 2) {
      return;
    }
    new_length *= 2;
  }
  if (new_length > SIZE_MAX / sizeof(*new_buckets)) {
    return;
  }
  new_buckets = calloc(new_length, sizeof(*new_buckets));
  if (new_buckets == NULL) {
    return;
  }

  for (i = 0; i < table->length; ++i) {
    cldb_lru_handle_t *entry = table->buckets[i];

    while (entry != NULL) {
      cldb_lru_handle_t *next = entry->next_hash;
      size_t index = (size_t)entry->hash & (new_length - 1);

      entry->next_hash = new_buckets[index];
      new_buckets[index] = entry;
      entry = next;
    }
  }
  free(table->buckets);
  table->buckets = new_buckets;
  table->length = new_length;
}

/* REQUIRES: 表锁已持有。 */
static cldb_lru_handle_t *handle_table_lookup_locked(
    cldb_handle_table_t *table, cldb_slice_t key, uint32_t hash) {
  return *handle_table_find_locked(table, key, hash);
}

/* REQUIRES: 表锁已持有。插入并返回被替换的旧条目，无旧条目返回 NULL。 */
static cldb_lru_handle_t *handle_table_insert_locked(
    cldb_handle_table_t *table, cldb_lru_handle_t *entry) {
  cldb_lru_handle_t **ptr =
      handle_table_find_locked(table, handle_key(entry), entry->hash);
  cldb_lru_handle_t *old = *ptr;

  entry->next_hash = old == NULL ? NULL : old->next_hash;
  *ptr = entry;
  if (old == NULL) {
    ++table->elems;
    if (table->elems > table->length) {
      handle_table_resize_locked(table);
    }
  }
  return old;
}

/* REQUIRES: 表锁已持有。移除并返回条目，未找到返回 NULL。 */
static cldb_lru_handle_t *handle_table_remove_locked(
    cldb_handle_table_t *table, cldb_slice_t key, uint32_t hash) {
  cldb_lru_handle_t **ptr = handle_table_find_locked(table, key, hash);
  cldb_lru_handle_t *result = *ptr;

  if (result != NULL) {
    *ptr = result->next_hash;
    --table->elems;
  }
  return result;
}

/* 对内部互斥量的失败只可能是生命周期误用；正常路径必须成功。 */
static void shard_lock(cldb_lru_cache_shard_t *shard) {
  cldb_status_t status = cldb_mutex_lock(&shard->mutex);

  CLDB_UNUSED(status);
  CLDB_ASSERT(cldb_status_is_ok(status));
}

static void shard_unlock(cldb_lru_cache_shard_t *shard) {
  cldb_status_t status = cldb_mutex_unlock(&shard->mutex);

  CLDB_UNUSED(status);
  CLDB_ASSERT(cldb_status_is_ok(status));
}

/* REQUIRES: shard 锁已持有。对应上游 Ref：首个外部引用把条目从 LRU 移到
 * in-use；之后只增加计数。 */
static void lru_ref_locked(cldb_lru_cache_shard_t *shard,
                           cldb_lru_handle_t *entry) {
  CLDB_ASSERT(entry->refs > 0);
  if (entry->refs == 1 && entry->in_cache) {
    lru_link_remove(&entry->link);
    lru_link_append(&shard->in_use, &entry->link);
  }
  ++entry->refs;
}

/* REQUIRES: shard 锁已持有。对应上游 Unref：归零时在锁内调用 deleter；
 * 只剩 cache 引用时移回 LRU。 */
static void lru_unref_locked(cldb_lru_cache_shard_t *shard,
                             cldb_lru_handle_t *entry) {
  CLDB_ASSERT(entry->refs > 0);
  --entry->refs;
  if (entry->refs == 0) {
    CLDB_ASSERT(!entry->in_cache);
    if (entry->deleter != NULL) {
      entry->deleter(entry->deleter_arg,
                     cldb_slice_make(entry->key_data, entry->key_size),
                     entry->value);
    }
    free(entry);
  } else if (entry->in_cache && entry->refs == 1) {
    lru_link_remove(&entry->link);
    lru_link_append(&shard->lru, &entry->link);
  }
}

/* REQUIRES: shard 锁已持有，条目已从哈希表移除。完成移除并释放 cache 的
 * 引用；仍有客户端引用时 deleter 延迟到最后一次 Release。 */
static void lru_finish_erase_locked(cldb_lru_cache_shard_t *shard,
                                    cldb_lru_handle_t *entry) {
  if (entry == NULL) {
    return;
  }
  CLDB_ASSERT(entry->in_cache);
  lru_link_remove(&entry->link);
  entry->in_cache = false;
  CLDB_ASSERT(shard->usage >= entry->charge);
  shard->usage -= entry->charge;
  lru_unref_locked(shard, entry);
}

/* REQUIRES: shard 锁已持有且 LRU 非空。驱逐最旧条目。 */
static void lru_erase_front_locked(cldb_lru_cache_shard_t *shard) {
  cldb_lru_handle_t *entry = handle_from_link(shard->lru.next);
  cldb_lru_handle_t *removed;

  CLDB_ASSERT(entry->refs == 1);
  removed = handle_table_remove_locked(&shard->table, handle_key(entry),
                                        entry->hash);
  CLDB_ASSERT(removed == entry);
  CLDB_UNUSED(removed);
  lru_finish_erase_locked(shard, entry);
}

/* REQUIRES: shard 锁已持有。usage 超限时只驱逐无客户端引用的条目。 */
static void lru_evict_locked(cldb_lru_cache_shard_t *shard) {
  while (shard->usage > shard->capacity && shard->lru.next != &shard->lru) {
    lru_erase_front_locked(shard);
  }
}

/* REQUIRES: shard 锁已持有。移除所有未被客户端引用的条目。 */
static void lru_prune_locked(cldb_lru_cache_shard_t *shard) {
  while (shard->lru.next != &shard->lru) {
    lru_erase_front_locked(shard);
  }
}

/* 上游按 (capacity + 15) / 16 向上取整；这里拆开写避免加法溢出。 */
static size_t per_shard_capacity(size_t capacity) {
  const size_t shards = (size_t)kNumShards;

  return capacity / shards + (capacity % shards != 0 ? 1u : 0u);
}

static size_t shard_index(uint32_t hash) {
  return (size_t)(hash >> (32 - kNumShardBits));
}

/* 初始化一个 shard；失败时清理已获取资源并返回 false。 */
static bool shard_init(cldb_lru_cache_shard_t *shard, size_t capacity) {
  if (!cldb_status_is_ok(cldb_mutex_init(&shard->mutex))) {
    return false;
  }
  shard->capacity = capacity;
  shard->usage = 0;
  lru_link_init(&shard->lru);
  lru_link_init(&shard->in_use);
  if (!handle_table_init(&shard->table)) {
    cldb_status_t status = cldb_mutex_destroy(&shard->mutex);

    CLDB_UNUSED(status);
    CLDB_ASSERT(cldb_status_is_ok(status));
    return false;
  }
  return true;
}

/* 锁内释放所有未被引用的条目，再释放表与互斥量。仍有未 Release 的 handle
 * 属于调用者违约，debug 构建会断言。 */
static void shard_destroy(cldb_lru_cache_shard_t *shard) {
  cldb_status_t status;

  shard_lock(shard);
  CLDB_ASSERT(shard->in_use.next == &shard->in_use);
  while (shard->lru.next != &shard->lru) {
    cldb_lru_handle_t *entry = handle_from_link(shard->lru.next);

    CLDB_ASSERT(entry->in_cache);
    CLDB_ASSERT(entry->refs == 1);
    lru_link_remove(&entry->link);
    entry->in_cache = false;
    CLDB_ASSERT(shard->usage >= entry->charge);
    shard->usage -= entry->charge;
    lru_unref_locked(shard, entry);
  }
  shard_unlock(shard);

  handle_table_destroy(&shard->table);
  status = cldb_mutex_destroy(&shard->mutex);
  CLDB_UNUSED(status);
  CLDB_ASSERT(cldb_status_is_ok(status));
}

static cldb_cache_handle_t *lru_cache_insert(
    cldb_cache_t *cache, cldb_slice_t key, void *value, size_t charge,
    cldb_cache_deleter_fn_t deleter, void *deleter_arg) {
  cldb_sharded_lru_cache_t *self = (cldb_sharded_lru_cache_t *)cache;
  uint32_t hash = cldb_hash(key.data, key.size, 0);
  cldb_lru_cache_shard_t *shard = &self->shards[shard_index(hash)];
  cldb_lru_handle_t *entry;
  size_t allocation_size;

  if (key.size > SIZE_MAX - sizeof(*entry)) {
    return NULL;
  }
  allocation_size = sizeof(*entry) + key.size;
  entry = malloc(allocation_size);
  if (entry == NULL) {
    return NULL;
  }
  entry->link.next = NULL;
  entry->link.prev = NULL;
  entry->value = value;
  entry->deleter = deleter;
  entry->deleter_arg = deleter_arg;
  entry->next_hash = NULL;
  entry->charge = charge;
  entry->key_size = key.size;
  entry->in_cache = false;
  entry->refs = 1; /* 返回给调用者的引用 */
  entry->hash = hash;
  if (key.size != 0) {
    memcpy(entry->key_data, key.data, key.size);
  }

  shard_lock(shard);
  if (shard->capacity > 0) {
    cldb_lru_handle_t *old;

    /* usage 不能回绕；失败时 value 仍归调用者。 */
    if (charge > SIZE_MAX - shard->usage) {
      shard_unlock(shard);
      free(entry);
      return NULL;
    }
    ++entry->refs; /* cache 自身引用 */
    entry->in_cache = true;
    lru_link_append(&shard->in_use, &entry->link);
    shard->usage += charge;
    old = handle_table_insert_locked(&shard->table, entry);
    lru_finish_erase_locked(shard, old);
  }
  lru_evict_locked(shard);
  shard_unlock(shard);

  return (cldb_cache_handle_t *)entry;
}

static cldb_cache_handle_t *lru_cache_lookup(cldb_cache_t *cache,
                                             cldb_slice_t key) {
  cldb_sharded_lru_cache_t *self = (cldb_sharded_lru_cache_t *)cache;
  uint32_t hash = cldb_hash(key.data, key.size, 0);
  cldb_lru_cache_shard_t *shard = &self->shards[shard_index(hash)];
  cldb_lru_handle_t *entry;

  shard_lock(shard);
  entry = handle_table_lookup_locked(&shard->table, key, hash);
  if (entry != NULL) {
    lru_ref_locked(shard, entry);
  }
  shard_unlock(shard);
  return (cldb_cache_handle_t *)entry;
}

static void lru_cache_release(cldb_cache_t *cache,
                              cldb_cache_handle_t *handle) {
  cldb_sharded_lru_cache_t *self = (cldb_sharded_lru_cache_t *)cache;
  cldb_lru_handle_t *entry = (cldb_lru_handle_t *)handle;
  cldb_lru_cache_shard_t *shard;

  CLDB_ASSERT(entry != NULL);
  shard = &self->shards[shard_index(entry->hash)];
  shard_lock(shard);
  lru_unref_locked(shard, entry);
  shard_unlock(shard);
}

static void *lru_cache_value(cldb_cache_t *cache,
                             cldb_cache_handle_t *handle) {
  cldb_lru_handle_t *entry = (cldb_lru_handle_t *)handle;

  CLDB_UNUSED(cache);
  CLDB_ASSERT(entry != NULL);
  return entry->value;
}

static void lru_cache_erase(cldb_cache_t *cache, cldb_slice_t key) {
  cldb_sharded_lru_cache_t *self = (cldb_sharded_lru_cache_t *)cache;
  uint32_t hash = cldb_hash(key.data, key.size, 0);
  cldb_lru_cache_shard_t *shard = &self->shards[shard_index(hash)];
  cldb_lru_handle_t *entry;

  shard_lock(shard);
  entry = handle_table_remove_locked(&shard->table, key, hash);
  lru_finish_erase_locked(shard, entry);
  shard_unlock(shard);
}

static uint64_t lru_cache_new_id(cldb_cache_t *cache) {
  cldb_sharded_lru_cache_t *self = (cldb_sharded_lru_cache_t *)cache;

  /* relaxed 足以保证唯一性；对应上游 id_mutex_ 下的 ++last_id_。 */
  return cldb_atomic_u64_fetch_add(&self->last_id, UINT64_C(1),
                                   memory_order_relaxed) +
         UINT64_C(1);
}

static void lru_cache_prune(cldb_cache_t *cache) {
  cldb_sharded_lru_cache_t *self = (cldb_sharded_lru_cache_t *)cache;
  size_t s;

  for (s = 0; s < (size_t)kNumShards; ++s) {
    cldb_lru_cache_shard_t *shard = &self->shards[s];

    shard_lock(shard);
    lru_prune_locked(shard);
    shard_unlock(shard);
  }
}

static void lru_cache_set_capacity(cldb_cache_t *cache, size_t capacity) {
  cldb_sharded_lru_cache_t *self = (cldb_sharded_lru_cache_t *)cache;
  size_t per_shard = per_shard_capacity(capacity);
  size_t s;

  for (s = 0; s < (size_t)kNumShards; ++s) {
    cldb_lru_cache_shard_t *shard = &self->shards[s];

    shard_lock(shard);
    shard->capacity = per_shard;
    lru_evict_locked(shard);
    shard_unlock(shard);
  }
}

static size_t lru_cache_total_charge(cldb_cache_t *cache) {
  cldb_sharded_lru_cache_t *self = (cldb_sharded_lru_cache_t *)cache;
  size_t total = 0;
  size_t s;

  for (s = 0; s < (size_t)kNumShards; ++s) {
    cldb_lru_cache_shard_t *shard = &self->shards[s];

    shard_lock(shard);
    total += shard->usage;
    shard_unlock(shard);
  }
  return total;
}

static void lru_cache_destroy(cldb_cache_t *cache) {
  cldb_sharded_lru_cache_t *self = (cldb_sharded_lru_cache_t *)cache;
  size_t s;

  for (s = 0; s < (size_t)kNumShards; ++s) {
    shard_destroy(&self->shards[s]);
  }
  free(self);
}

static const cldb_cache_vtbl_t kShardedLruCacheVtbl = {
    .insert = lru_cache_insert,
    .lookup = lru_cache_lookup,
    .release = lru_cache_release,
    .value = lru_cache_value,
    .erase = lru_cache_erase,
    .new_id = lru_cache_new_id,
    .prune = lru_cache_prune,
    .set_capacity = lru_cache_set_capacity,
    .total_charge = lru_cache_total_charge,
    .destroy = lru_cache_destroy,
};

cldb_cache_t *cldb_new_lru_cache(size_t capacity) {
  cldb_sharded_lru_cache_t *self;
  size_t per_shard = per_shard_capacity(capacity);
  size_t initialized = 0;
  size_t s;

  self = calloc(1, sizeof(*self));
  if (self == NULL) {
    return NULL;
  }
  self->base.vtbl = &kShardedLruCacheVtbl;
  cldb_atomic_u64_init(&self->last_id, 0);

  for (s = 0; s < (size_t)kNumShards; ++s) {
    if (!shard_init(&self->shards[s], per_shard)) {
      size_t i;

      for (i = 0; i < initialized; ++i) {
        shard_destroy(&self->shards[i]);
      }
      free(self);
      return NULL;
    }
    ++initialized;
  }
  return &self->base;
}

void cldb_cache_destroy(cldb_cache_t *cache) {
  if (cache == NULL) {
    return;
  }
  CLDB_ASSERT(cache->vtbl != NULL);
  CLDB_ASSERT(cache->vtbl->destroy != NULL);
  cache->vtbl->destroy(cache);
}
