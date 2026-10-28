#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "cleveldb/cache.h"
#include "harness.h"
#include "port/port_atomic.h"
#include "port/port_thread.h"
#include "status_test_util.h"
#include "util/coding.h"

/* 对应上游 util/cache_test.cc：同时覆盖 handle 引用、驱逐顺序、容量调整和
 * 并发访问。测试 key 使用固定 4 字节编码。 */

enum { kDeleteLogCapacity = 4096, kCacheSize = 1000 };

typedef struct {
  int keys[kDeleteLogCapacity];
  int values[kDeleteLogCapacity];
  size_t count; /* 已记录的回调次数 */
} delete_log_t;

/* 记录 deleter 收到的 key/value；仅用于单线程测试。 */
static void record_delete(void *arg, cldb_slice_t key, void *value) {
  delete_log_t *log = arg;

  CLDB_ASSERT_TRUE(log != NULL);
  CLDB_ASSERT_EQ_UINT(4, key.size);
  CLDB_ASSERT_TRUE(log->count < (size_t)kDeleteLogCapacity);
  if (key.size != 4 || log->count >= (size_t)kDeleteLogCapacity) {
    return;
  }
  log->keys[log->count] = (int)cldb_decode_fixed32(key.data);
  log->values[log->count] = (int)(uintptr_t)value;
  ++log->count;
}

static cldb_slice_t key_for_int(int key, char storage[4]) {
  cldb_encode_fixed32(storage, (uint32_t)key);
  return cldb_slice_make(storage, 4);
}

static void *value_for_int(int value) { return (void *)(uintptr_t)value; }

static int int_for_value(void *value) { return (int)(uintptr_t)value; }

typedef struct {
  cldb_cache_t *cache; /* owned；由 fixture_destroy 释放 */
  delete_log_t deleted;
} cache_fixture_t;

static void fixture_init(cache_fixture_t *fixture, size_t capacity) {
  fixture->cache = cldb_new_lru_cache(capacity);
  fixture->deleted.count = 0;
  CLDB_ASSERT_TRUE(fixture->cache != NULL);
}

static void fixture_destroy(cache_fixture_t *fixture) {
  cldb_cache_destroy(fixture->cache);
  fixture->cache = NULL;
}

static cldb_cache_handle_t *fixture_insert_handle(cache_fixture_t *fixture,
                                                  int key, int value,
                                                  size_t charge) {
  char storage[4];
  cldb_cache_handle_t *handle =
      cldb_cache_insert(fixture->cache, key_for_int(key, storage),
                        value_for_int(value), charge, record_delete,
                        &fixture->deleted);

  CLDB_ASSERT_TRUE(handle != NULL);
  return handle;
}

static void fixture_insert(cache_fixture_t *fixture, int key, int value,
                           size_t charge) {
  cldb_cache_handle_t *handle =
      fixture_insert_handle(fixture, key, value, charge);

  cldb_cache_release(fixture->cache, handle);
}

static int fixture_lookup(cache_fixture_t *fixture, int key) {
  char storage[4];
  cldb_cache_handle_t *handle =
      cldb_cache_lookup(fixture->cache, key_for_int(key, storage));
  int result = -1;

  if (handle != NULL) {
    result = int_for_value(cldb_cache_value(fixture->cache, handle));
    cldb_cache_release(fixture->cache, handle);
  }
  return result;
}

static void fixture_erase(cache_fixture_t *fixture, int key) {
  char storage[4];

  cldb_cache_erase(fixture->cache, key_for_int(key, storage));
}

static bool delete_log_contains_key(const delete_log_t *log, int key) {
  size_t i;

  for (i = 0; i < log->count; ++i) {
    if (log->keys[i] == key) {
      return true;
    }
  }
  return false;
}

CLDB_TEST(cache_hit_and_miss) {
  cache_fixture_t fixture;

  fixture_init(&fixture, kCacheSize);
  CLDB_EXPECT_EQ_INT(-1, fixture_lookup(&fixture, 100));

  fixture_insert(&fixture, 100, 101, 1);
  CLDB_EXPECT_EQ_INT(101, fixture_lookup(&fixture, 100));
  CLDB_EXPECT_EQ_INT(-1, fixture_lookup(&fixture, 200));
  CLDB_EXPECT_EQ_INT(-1, fixture_lookup(&fixture, 300));

  fixture_insert(&fixture, 200, 201, 1);
  CLDB_EXPECT_EQ_INT(101, fixture_lookup(&fixture, 100));
  CLDB_EXPECT_EQ_INT(201, fixture_lookup(&fixture, 200));
  CLDB_EXPECT_EQ_INT(-1, fixture_lookup(&fixture, 300));

  fixture_insert(&fixture, 100, 102, 1);
  CLDB_EXPECT_EQ_INT(102, fixture_lookup(&fixture, 100));
  CLDB_EXPECT_EQ_INT(201, fixture_lookup(&fixture, 200));
  CLDB_EXPECT_EQ_INT(-1, fixture_lookup(&fixture, 300));

  /* 旧条目无客户端引用，替换时立即删除。 */
  CLDB_EXPECT_EQ_UINT(1, fixture.deleted.count);
  CLDB_EXPECT_EQ_INT(100, fixture.deleted.keys[0]);
  CLDB_EXPECT_EQ_INT(101, fixture.deleted.values[0]);

  fixture_destroy(&fixture);
}

CLDB_TEST(cache_erase) {
  cache_fixture_t fixture;

  fixture_init(&fixture, kCacheSize);
  fixture_erase(&fixture, 200);
  CLDB_EXPECT_EQ_UINT(0, fixture.deleted.count);

  fixture_insert(&fixture, 100, 101, 1);
  fixture_insert(&fixture, 200, 201, 1);
  fixture_erase(&fixture, 100);
  CLDB_EXPECT_EQ_INT(-1, fixture_lookup(&fixture, 100));
  CLDB_EXPECT_EQ_INT(201, fixture_lookup(&fixture, 200));
  CLDB_EXPECT_EQ_UINT(1, fixture.deleted.count);
  CLDB_EXPECT_EQ_INT(100, fixture.deleted.keys[0]);
  CLDB_EXPECT_EQ_INT(101, fixture.deleted.values[0]);

  fixture_erase(&fixture, 100);
  CLDB_EXPECT_EQ_INT(-1, fixture_lookup(&fixture, 100));
  CLDB_EXPECT_EQ_INT(201, fixture_lookup(&fixture, 200));
  CLDB_EXPECT_EQ_UINT(1, fixture.deleted.count);

  fixture_destroy(&fixture);
}

CLDB_TEST(cache_entries_are_pinned) {
  cache_fixture_t fixture;
  char storage[4];
  cldb_cache_handle_t *h1;
  cldb_cache_handle_t *h2;

  fixture_init(&fixture, kCacheSize);
  fixture_insert(&fixture, 100, 101, 1);
  h1 = cldb_cache_lookup(fixture.cache, key_for_int(100, storage));
  CLDB_ASSERT_TRUE(h1 != NULL);
  CLDB_EXPECT_EQ_INT(101, int_for_value(cldb_cache_value(fixture.cache, h1)));

  fixture_insert(&fixture, 100, 102, 1);
  h2 = cldb_cache_lookup(fixture.cache, key_for_int(100, storage));
  CLDB_ASSERT_TRUE(h2 != NULL);
  CLDB_EXPECT_EQ_INT(102, int_for_value(cldb_cache_value(fixture.cache, h2)));
  /* h1 仍持有旧条目，替换不能立即调用 deleter。 */
  CLDB_EXPECT_EQ_UINT(0, fixture.deleted.count);

  cldb_cache_release(fixture.cache, h1);
  CLDB_EXPECT_EQ_UINT(1, fixture.deleted.count);
  CLDB_EXPECT_EQ_INT(100, fixture.deleted.keys[0]);
  CLDB_EXPECT_EQ_INT(101, fixture.deleted.values[0]);

  fixture_erase(&fixture, 100);
  CLDB_EXPECT_EQ_INT(-1, fixture_lookup(&fixture, 100));
  CLDB_EXPECT_EQ_UINT(1, fixture.deleted.count);

  cldb_cache_release(fixture.cache, h2);
  CLDB_EXPECT_EQ_UINT(2, fixture.deleted.count);
  CLDB_EXPECT_EQ_INT(100, fixture.deleted.keys[1]);
  CLDB_EXPECT_EQ_INT(102, fixture.deleted.values[1]);

  fixture_destroy(&fixture);
}

CLDB_TEST(cache_eviction_policy) {
  cache_fixture_t fixture;
  char storage[4];
  cldb_cache_handle_t *pinned;
  int i;

  fixture_init(&fixture, kCacheSize);
  fixture_insert(&fixture, 100, 101, 1);
  fixture_insert(&fixture, 200, 201, 1);
  fixture_insert(&fixture, 300, 301, 1);
  pinned = cldb_cache_lookup(fixture.cache, key_for_int(300, storage));
  CLDB_ASSERT_TRUE(pinned != NULL);

  /* 频繁访问的 100 和仍被引用的 300 应保留，长期未用的 200 被驱逐。 */
  for (i = 0; i < kCacheSize + 100; ++i) {
    fixture_insert(&fixture, 1000 + i, 2000 + i, 1);
    CLDB_ASSERT_EQ_INT(2000 + i, fixture_lookup(&fixture, 1000 + i));
    CLDB_ASSERT_EQ_INT(101, fixture_lookup(&fixture, 100));
  }
  CLDB_EXPECT_EQ_INT(101, fixture_lookup(&fixture, 100));
  CLDB_EXPECT_EQ_INT(-1, fixture_lookup(&fixture, 200));
  CLDB_EXPECT_EQ_INT(301, fixture_lookup(&fixture, 300));
  cldb_cache_release(fixture.cache, pinned);

  fixture_destroy(&fixture);
}

CLDB_TEST(cache_use_exceeds_capacity) {
  enum { kEntryCount = kCacheSize + 100 };
  cache_fixture_t fixture;
  cldb_cache_handle_t *handles[kEntryCount];
  size_t i;

  fixture_init(&fixture, kCacheSize);
  /* 全部条目都保持客户端引用，因此容量超限也不能驱逐。 */
  for (i = 0; i < (size_t)kEntryCount; ++i) {
    handles[i] = fixture_insert_handle(&fixture, 1000 + (int)i,
                                       2000 + (int)i, 1);
  }
  CLDB_EXPECT_EQ_UINT(0, fixture.deleted.count);

  for (i = 0; i < (size_t)kEntryCount; ++i) {
    CLDB_EXPECT_EQ_INT(2000 + (int)i, fixture_lookup(&fixture, 1000 + (int)i));
  }
  for (i = 0; i < (size_t)kEntryCount; ++i) {
    cldb_cache_release(fixture.cache, handles[i]);
  }

  /* Release 后条目进入 LRU，但上游不会立即补驱逐。 */
  CLDB_EXPECT_EQ_UINT(kEntryCount, cldb_cache_total_charge(fixture.cache));
  fixture_destroy(&fixture);
  CLDB_EXPECT_EQ_UINT(kEntryCount, fixture.deleted.count);
}

CLDB_TEST(cache_heavy_entries) {
  const int kLight = 1;
  const int kHeavy = 10;
  const size_t kTotalWeight = 2 * (size_t)kCacheSize;
  cache_fixture_t fixture;
  int added = 0;
  int index = 0;
  int cached_weight = 0;
  int i;

  fixture_init(&fixture, kCacheSize);
  while ((size_t)added < kTotalWeight) {
    int weight = (index & 1) != 0 ? kLight : kHeavy;

    fixture_insert(&fixture, index, 1000 + index, (size_t)weight);
    added += weight;
    ++index;
  }

  for (i = 0; i < index; ++i) {
    int weight = (i & 1) != 0 ? kLight : kHeavy;
    int result = fixture_lookup(&fixture, i);

    if (result >= 0) {
      cached_weight += weight;
      CLDB_ASSERT_EQ_INT(1000 + i, result);
    }
  }
  /* 分片取整会让总容量略大于 kCacheSize，阈值与上游一致。 */
  CLDB_EXPECT_TRUE(cached_weight <= kCacheSize + kCacheSize / 10);

  fixture_destroy(&fixture);
}

CLDB_TEST(cache_new_id) {
  cache_fixture_t fixture;
  uint64_t first;
  uint64_t second;

  fixture_init(&fixture, kCacheSize);
  first = cldb_cache_new_id(fixture.cache);
  second = cldb_cache_new_id(fixture.cache);
  CLDB_EXPECT_EQ_UINT(1, first);
  CLDB_EXPECT_EQ_UINT(2, second);
  CLDB_EXPECT_TRUE(first != second);

  fixture_destroy(&fixture);
}

enum { kIdThreads = 4, kIdsPerThread = 250 };

typedef struct {
  cldb_cache_t *cache; /* borrowed；并发只调用原子 NewId */
  uint64_t ids[kIdsPerThread];
} id_worker_t;

static void *id_worker(void *arg) {
  id_worker_t *worker = arg;
  size_t i;

  for (i = 0; i < (size_t)kIdsPerThread; ++i) {
    worker->ids[i] = cldb_cache_new_id(worker->cache);
  }
  return NULL;
}

CLDB_TEST(cache_new_id_concurrent) {
  cldb_cache_t *cache = cldb_new_lru_cache(0);
  cldb_thread_t threads[kIdThreads];
  id_worker_t workers[kIdThreads];
  size_t count = (size_t)kIdsPerThread;
  size_t total = (size_t)kIdThreads * count;
  size_t duplicates = 0;
  void *result = NULL;
  size_t i;
  size_t j;

  CLDB_ASSERT_TRUE(cache != NULL);
  for (i = 0; i < (size_t)kIdThreads; ++i) {
    workers[i].cache = cache;
    cldb_thread_init(&threads[i]);
    CLDB_ASSERT_OK(cldb_thread_start(&threads[i], id_worker, &workers[i]));
  }
  for (i = 0; i < (size_t)kIdThreads; ++i) {
    CLDB_ASSERT_OK(cldb_thread_join(&threads[i], &result));
    CLDB_EXPECT_EQ_PTR(NULL, result);
  }

  for (i = 0; i < total; ++i) {
    uint64_t a = workers[i / count].ids[i % count];

    for (j = i + 1; j < total; ++j) {
      uint64_t b = workers[j / count].ids[j % count];

      if (a == b) {
        ++duplicates;
      }
    }
  }
  CLDB_EXPECT_EQ_UINT(0, duplicates);

  cldb_cache_destroy(cache);
}

CLDB_TEST(cache_prune) {
  cache_fixture_t fixture;
  char storage[4];
  cldb_cache_handle_t *handle;

  fixture_init(&fixture, kCacheSize);
  fixture_insert(&fixture, 1, 100, 1);
  fixture_insert(&fixture, 2, 200, 1);

  handle = cldb_cache_lookup(fixture.cache, key_for_int(1, storage));
  CLDB_ASSERT_TRUE(handle != NULL);
  cldb_cache_prune(fixture.cache);
  cldb_cache_release(fixture.cache, handle);

  CLDB_EXPECT_EQ_INT(100, fixture_lookup(&fixture, 1));
  CLDB_EXPECT_EQ_INT(-1, fixture_lookup(&fixture, 2));
  CLDB_EXPECT_EQ_UINT(1, fixture.deleted.count);
  CLDB_EXPECT_EQ_INT(2, fixture.deleted.keys[0]);
  CLDB_EXPECT_EQ_INT(200, fixture.deleted.values[0]);

  fixture_destroy(&fixture);
}

CLDB_TEST(cache_zero_capacity) {
  cache_fixture_t fixture;
  cldb_cache_handle_t *handle;

  fixture_init(&fixture, 0);
  handle = fixture_insert_handle(&fixture, 1, 100, 1);
  CLDB_EXPECT_EQ_UINT(0, fixture.deleted.count);
  CLDB_EXPECT_EQ_INT(-1, fixture_lookup(&fixture, 1));
  CLDB_EXPECT_EQ_INT(
      100, int_for_value(cldb_cache_value(fixture.cache, handle)));

  cldb_cache_release(fixture.cache, handle);
  CLDB_EXPECT_EQ_UINT(1, fixture.deleted.count);
  CLDB_EXPECT_EQ_INT(1, fixture.deleted.keys[0]);
  CLDB_EXPECT_EQ_INT(100, fixture.deleted.values[0]);
  CLDB_EXPECT_EQ_INT(-1, fixture_lookup(&fixture, 1));

  fixture_destroy(&fixture);
}

CLDB_TEST(cache_set_capacity) {
  cache_fixture_t fixture;
  int i;

  fixture_init(&fixture, kCacheSize);
  for (i = 0; i < 100; ++i) {
    fixture_insert(&fixture, i, 1000 + i, 1);
  }
  CLDB_EXPECT_EQ_UINT(100, cldb_cache_total_charge(fixture.cache));

  /* 每个 shard 得到 ceil(10/16)=1，总容量最多 16。 */
  cldb_cache_set_capacity(fixture.cache, 10);
  CLDB_EXPECT_TRUE(cldb_cache_total_charge(fixture.cache) <= 16);

  cldb_cache_set_capacity(fixture.cache, 0);
  CLDB_EXPECT_EQ_UINT(0, cldb_cache_total_charge(fixture.cache));
  CLDB_EXPECT_EQ_UINT(100, fixture.deleted.count);

  fixture_destroy(&fixture);
}

CLDB_TEST(cache_set_capacity_with_pinned) {
  cache_fixture_t fixture;
  char storage[4];
  cldb_cache_handle_t *handle;

  fixture_init(&fixture, kCacheSize);
  fixture_insert(&fixture, 1, 100, 1);
  fixture_insert(&fixture, 2, 200, 1);
  handle = cldb_cache_lookup(fixture.cache, key_for_int(1, storage));
  CLDB_ASSERT_TRUE(handle != NULL);

  /* 缩容不能删除仍被引用的条目；其他条目立即驱逐。 */
  cldb_cache_set_capacity(fixture.cache, 0);
  CLDB_EXPECT_EQ_UINT(1, cldb_cache_total_charge(fixture.cache));
  CLDB_EXPECT_TRUE(delete_log_contains_key(&fixture.deleted, 2));
  CLDB_EXPECT_FALSE(delete_log_contains_key(&fixture.deleted, 1));

  /* Release 只把条目移回 LRU；上游要等下一次驱逐操作才回收。 */
  cldb_cache_release(fixture.cache, handle);
  CLDB_EXPECT_EQ_UINT(1, cldb_cache_total_charge(fixture.cache));
  CLDB_EXPECT_FALSE(delete_log_contains_key(&fixture.deleted, 1));

  cldb_cache_set_capacity(fixture.cache, 0);
  CLDB_EXPECT_EQ_UINT(0, cldb_cache_total_charge(fixture.cache));
  CLDB_EXPECT_TRUE(delete_log_contains_key(&fixture.deleted, 1));

  fixture_destroy(&fixture);
}

typedef struct {
  size_t calls;
} delete_counter_t;

/* 不解析 key 的 deleter；用于任意长度 key 的测试。 */
static void count_delete(void *arg, cldb_slice_t key, void *value) {
  delete_counter_t *counter = arg;

  (void)key;
  (void)value;
  ++counter->calls;
}

CLDB_TEST(cache_binary_and_empty_keys) {
  cldb_cache_t *cache = cldb_new_lru_cache(100);
  delete_counter_t counter = {0};
  char mutable_key[4] = {0, 0, 0, 0};
  char binary_key[4] = {0, 1, 2, (char)0xff};
  cldb_slice_t key;
  cldb_cache_handle_t *handle;

  CLDB_ASSERT_TRUE(cache != NULL);

  /* 插入后改写调用者的 key 缓冲区，验证 cache 保存了副本。 */
  cldb_encode_fixed32(mutable_key, UINT32_C(7));
  key = cldb_slice_make(mutable_key, sizeof(mutable_key));
  handle = cldb_cache_insert(cache, key, value_for_int(77), 1, count_delete,
                             &counter);
  CLDB_ASSERT_TRUE(handle != NULL);
  cldb_cache_release(cache, handle);

  memset(mutable_key, 'x', sizeof(mutable_key));
  cldb_encode_fixed32(mutable_key, UINT32_C(7));
  key = cldb_slice_make(mutable_key, sizeof(mutable_key));
  handle = cldb_cache_lookup(cache, key);
  CLDB_ASSERT_TRUE(handle != NULL);
  CLDB_EXPECT_EQ_INT(77, int_for_value(cldb_cache_value(cache, handle)));
  cldb_cache_release(cache, handle);
  cldb_cache_erase(cache, key);
  CLDB_EXPECT_EQ_UINT(1, counter.calls);

  /* 含 NUL 和 0xff 的二进制 key 按长度比较。 */
  key = cldb_slice_make(binary_key, sizeof(binary_key));
  handle = cldb_cache_insert(cache, key, value_for_int(88), 1, count_delete,
                             &counter);
  CLDB_ASSERT_TRUE(handle != NULL);
  cldb_cache_release(cache, handle);
  handle = cldb_cache_lookup(cache, key);
  CLDB_ASSERT_TRUE(handle != NULL);
  CLDB_EXPECT_EQ_INT(88, int_for_value(cldb_cache_value(cache, handle)));
  cldb_cache_release(cache, handle);
  cldb_cache_erase(cache, key);
  CLDB_EXPECT_EQ_UINT(2, counter.calls);

  /* 空 key 是合法 key。 */
  key = cldb_slice_empty();
  handle = cldb_cache_insert(cache, key, value_for_int(99), 1, count_delete,
                             &counter);
  CLDB_ASSERT_TRUE(handle != NULL);
  cldb_cache_release(cache, handle);
  handle = cldb_cache_lookup(cache, key);
  CLDB_ASSERT_TRUE(handle != NULL);
  CLDB_EXPECT_EQ_INT(99, int_for_value(cldb_cache_value(cache, handle)));
  cldb_cache_release(cache, handle);
  cldb_cache_erase(cache, key);
  CLDB_EXPECT_EQ_UINT(3, counter.calls);

  cldb_cache_destroy(cache);
}

typedef struct {
  void *value;
} custom_cache_handle_t;

typedef struct {
  cldb_cache_t base; /* 必须为首成员 */
  custom_cache_handle_t handle;
  size_t last_capacity;
  uint64_t next_id;
  unsigned insert_calls;
  unsigned lookup_calls;
  unsigned release_calls;
  unsigned value_calls;
  unsigned erase_calls;
  unsigned new_id_calls;
  unsigned prune_calls;
  unsigned set_capacity_calls;
  unsigned total_charge_calls;
  unsigned destroy_calls;
} custom_cache_t;

static cldb_cache_handle_t *custom_insert(
    cldb_cache_t *cache, cldb_slice_t key, void *value, size_t charge,
    cldb_cache_deleter_fn_t deleter, void *deleter_arg) {
  custom_cache_t *self = (custom_cache_t *)cache;

  (void)key;
  (void)charge;
  (void)deleter;
  (void)deleter_arg;
  ++self->insert_calls;
  self->handle.value = value;
  return (cldb_cache_handle_t *)&self->handle;
}

static cldb_cache_handle_t *custom_lookup(cldb_cache_t *cache,
                                          cldb_slice_t key) {
  custom_cache_t *self = (custom_cache_t *)cache;

  (void)key;
  ++self->lookup_calls;
  return (cldb_cache_handle_t *)&self->handle;
}

static void custom_release(cldb_cache_t *cache,
                           cldb_cache_handle_t *handle) {
  custom_cache_t *self = (custom_cache_t *)cache;

  (void)handle;
  ++self->release_calls;
}

static void *custom_value(cldb_cache_t *cache,
                          cldb_cache_handle_t *handle) {
  custom_cache_t *self = (custom_cache_t *)cache;
  custom_cache_handle_t *entry = (custom_cache_handle_t *)handle;

  ++self->value_calls;
  return entry->value;
}

static void custom_erase(cldb_cache_t *cache, cldb_slice_t key) {
  custom_cache_t *self = (custom_cache_t *)cache;

  (void)key;
  ++self->erase_calls;
}

static uint64_t custom_new_id(cldb_cache_t *cache) {
  custom_cache_t *self = (custom_cache_t *)cache;

  ++self->new_id_calls;
  ++self->next_id;
  return self->next_id;
}

static void custom_prune(cldb_cache_t *cache) {
  custom_cache_t *self = (custom_cache_t *)cache;

  ++self->prune_calls;
}

static void custom_set_capacity(cldb_cache_t *cache, size_t capacity) {
  custom_cache_t *self = (custom_cache_t *)cache;

  ++self->set_capacity_calls;
  self->last_capacity = capacity;
}

static size_t custom_total_charge(cldb_cache_t *cache) {
  custom_cache_t *self = (custom_cache_t *)cache;

  ++self->total_charge_calls;
  return 123;
}

static void custom_destroy(cldb_cache_t *cache) {
  custom_cache_t *self = (custom_cache_t *)cache;

  ++self->destroy_calls;
}

static const cldb_cache_vtbl_t kCustomVtbl = {
    .insert = custom_insert,
    .lookup = custom_lookup,
    .release = custom_release,
    .value = custom_value,
    .erase = custom_erase,
    .new_id = custom_new_id,
    .prune = custom_prune,
    .set_capacity = custom_set_capacity,
    .total_charge = custom_total_charge,
    .destroy = custom_destroy,
};

CLDB_TEST(cache_custom_vtable) {
  custom_cache_t custom = {0};
  cldb_cache_handle_t *handle;
  cldb_slice_t key = cldb_slice_make("key", 3);

  custom.base.vtbl = &kCustomVtbl;
  handle = cldb_cache_insert(&custom.base, key, value_for_int(7), 3, NULL,
                             NULL);
  CLDB_ASSERT_TRUE(handle != NULL);
  CLDB_EXPECT_TRUE(handle == (cldb_cache_handle_t *)&custom.handle);
  CLDB_EXPECT_EQ_UINT(1, custom.insert_calls);

  CLDB_EXPECT_TRUE(cldb_cache_lookup(&custom.base, key) == handle);
  CLDB_EXPECT_EQ_UINT(1, custom.lookup_calls);
  CLDB_EXPECT_EQ_INT(7, int_for_value(cldb_cache_value(&custom.base, handle)));
  CLDB_EXPECT_EQ_UINT(1, custom.value_calls);

  cldb_cache_release(&custom.base, handle);
  cldb_cache_erase(&custom.base, key);
  CLDB_EXPECT_EQ_UINT(1, custom.release_calls);
  CLDB_EXPECT_EQ_UINT(1, custom.erase_calls);

  CLDB_EXPECT_EQ_UINT(1, cldb_cache_new_id(&custom.base));
  CLDB_EXPECT_EQ_UINT(2, cldb_cache_new_id(&custom.base));
  CLDB_EXPECT_EQ_UINT(2, custom.new_id_calls);

  cldb_cache_prune(&custom.base);
  cldb_cache_set_capacity(&custom.base, 99);
  CLDB_EXPECT_EQ_UINT(1, custom.prune_calls);
  CLDB_EXPECT_EQ_UINT(1, custom.set_capacity_calls);
  CLDB_EXPECT_EQ_UINT(99, custom.last_capacity);
  CLDB_EXPECT_EQ_UINT(123, cldb_cache_total_charge(&custom.base));
  CLDB_EXPECT_EQ_UINT(1, custom.total_charge_calls);

  cldb_cache_destroy(&custom.base);
  CLDB_EXPECT_EQ_UINT(1, custom.destroy_calls);
  cldb_cache_destroy(NULL);
}

enum {
  kStressThreads = 4,
  kStressIterations = 4000,
  kStressKeySpace = 512,
  kStressCacheCapacity = 256
};

typedef struct {
  cldb_cache_t *cache; /* borrowed */
  cldb_atomic_u64_t deleted; /* deleter 线程安全计数 */
} stress_state_t;

typedef struct {
  stress_state_t *state; /* borrowed */
  unsigned worker_id;
} stress_worker_t;

static void stress_delete(void *arg, cldb_slice_t key, void *value) {
  stress_state_t *state = arg;

  (void)key;
  (void)value;
  cldb_atomic_u64_fetch_add(&state->deleted, UINT64_C(1),
                            memory_order_relaxed);
}

static void *stress_worker(void *arg) {
  stress_worker_t *worker = arg;
  unsigned i;

  for (i = 0; i < (unsigned)kStressIterations; ++i) {
    uint32_t key_number =
        (uint32_t)((i * 17u + worker->worker_id * 131u) %
                   (unsigned)kStressKeySpace);
    char storage[4];
    cldb_slice_t key;

    cldb_encode_fixed32(storage, key_number);
    key = cldb_slice_make(storage, 4);
    switch (i % 4u) {
      case 0: {
        cldb_cache_handle_t *handle = cldb_cache_insert(
            worker->state->cache, key, (void *)(uintptr_t)(i + 1u), 1,
            stress_delete, worker->state);

        if (handle == NULL) {
          return (void *)(uintptr_t)1;
        }
        cldb_cache_release(worker->state->cache, handle);
        break;
      }
      case 1: {
        cldb_cache_handle_t *handle =
            cldb_cache_lookup(worker->state->cache, key);

        if (handle != NULL) {
          if (cldb_cache_value(worker->state->cache, handle) == NULL) {
            cldb_cache_release(worker->state->cache, handle);
            return (void *)(uintptr_t)1;
          }
          cldb_cache_release(worker->state->cache, handle);
        }
        break;
      }
      case 2:
        cldb_cache_erase(worker->state->cache, key);
        break;
      default:
        (void)cldb_cache_new_id(worker->state->cache);
        break;
    }
  }
  return NULL;
}

CLDB_TEST(cache_concurrent_stress) {
  const size_t capacity = (size_t)kStressCacheCapacity;
  cldb_cache_t *cache = cldb_new_lru_cache(capacity);
  stress_state_t state;
  cldb_thread_t threads[kStressThreads];
  stress_worker_t workers[kStressThreads];
  void *result = NULL;
  size_t i;

  CLDB_ASSERT_TRUE(cache != NULL);
  state.cache = cache;
  cldb_atomic_u64_init(&state.deleted, 0);

  for (i = 0; i < (size_t)kStressThreads; ++i) {
    workers[i].state = &state;
    workers[i].worker_id = (unsigned)i;
    cldb_thread_init(&threads[i]);
    CLDB_ASSERT_OK(cldb_thread_start(&threads[i], stress_worker, &workers[i]));
  }
  for (i = 0; i < (size_t)kStressThreads; ++i) {
    CLDB_ASSERT_OK(cldb_thread_join(&threads[i], &result));
    CLDB_EXPECT_EQ_PTR(NULL, result);
  }

  /* 压测后没有 handle 在外；缩容可把 LRU 清到容量以内。 */
  cldb_cache_set_capacity(cache, capacity);
  CLDB_EXPECT_TRUE(cldb_cache_total_charge(cache) <= capacity);
  cldb_cache_destroy(cache);
  CLDB_EXPECT_TRUE(cldb_atomic_u64_load(&state.deleted,
                                        memory_order_relaxed) > 0);
}

void cldb_register_cache_tests(void) {
  CLDB_RUN_TEST(cache_hit_and_miss);
  CLDB_RUN_TEST(cache_erase);
  CLDB_RUN_TEST(cache_entries_are_pinned);
  CLDB_RUN_TEST(cache_eviction_policy);
  CLDB_RUN_TEST(cache_use_exceeds_capacity);
  CLDB_RUN_TEST(cache_heavy_entries);
  CLDB_RUN_TEST(cache_new_id);
  CLDB_RUN_TEST(cache_new_id_concurrent);
  CLDB_RUN_TEST(cache_prune);
  CLDB_RUN_TEST(cache_zero_capacity);
  CLDB_RUN_TEST(cache_set_capacity);
  CLDB_RUN_TEST(cache_set_capacity_with_pinned);
  CLDB_RUN_TEST(cache_binary_and_empty_keys);
  CLDB_RUN_TEST(cache_custom_vtable);
  CLDB_RUN_TEST(cache_concurrent_stress);
}
