#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "db/skiplist.h"
#include "harness.h"
#include "port/port_thread.h"
#include "util/arena.h"
#include "util/random.h"

/* 测试用比较器：key 指向一个 uint64_t，按数值比较。arg 未使用。 */
static int compare_u64(void *arg, const char *a, const char *b) {
  uint64_t x;
  uint64_t y;

  (void)arg;
  memcpy(&x, a, sizeof(x));
  memcpy(&y, b, sizeof(y));
  return (x < y) ? -1 : (x > y) ? 1 : 0;
}

static uint64_t decode_u64(const char *key) {
  uint64_t value;

  memcpy(&value, key, sizeof(value));
  return value;
}

/* 在 arena 中分配并写入一个数值 key；返回 NULL 表示分配失败。 */
static char *allocate_u64(cldb_arena_t *arena, uint64_t value) {
  uint64_t *key =
      (uint64_t *)cldb_arena_allocate_aligned(arena, sizeof(*key));

  if (key == NULL) {
    return NULL;
  }
  *key = value;
  return (char *)key;
}

static size_t lower_bound_u64(const uint64_t *keys, size_t count,
                              uint64_t key) {
  size_t lo = 0;
  size_t hi = count;

  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;

    if (keys[mid] < key) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo;
}

/* 把 key 插入已排序模型数组；容量不足返回 false。 */
static bool model_insert(uint64_t *keys, size_t *count, size_t capacity,
                         uint64_t key) {
  size_t pos;
  size_t i;

  if (*count >= capacity) {
    return false;
  }
  pos = lower_bound_u64(keys, *count, key);
  for (i = *count; i > pos; --i) {
    keys[i] = keys[i - 1];
  }
  keys[pos] = key;
  ++*count;
  return true;
}

CLDB_TEST(skiplist_empty) {
  cldb_arena_t arena;
  cldb_skiplist_t list;
  cldb_skiplist_iterator_t iter;
  uint64_t target = 100;

  cldb_arena_init(&arena);
  CLDB_EXPECT_FALSE(
      cldb_skiplist_init(NULL, &arena, compare_u64, NULL));
  CLDB_EXPECT_FALSE(cldb_skiplist_init(&list, NULL, compare_u64, NULL));
  CLDB_EXPECT_FALSE(cldb_skiplist_init(&list, &arena, NULL, NULL));
  CLDB_ASSERT_TRUE(
      cldb_skiplist_init(&list, &arena, compare_u64, NULL));
  CLDB_EXPECT_EQ_INT(1, cldb_skiplist_max_height(&list));
  CLDB_EXPECT_EQ_INT(0, cldb_skiplist_max_height(NULL));

  CLDB_EXPECT_FALSE(cldb_skiplist_contains(&list, (const char *)&target));
  CLDB_EXPECT_FALSE(cldb_skiplist_contains(NULL, (const char *)&target));
  CLDB_EXPECT_FALSE(cldb_skiplist_contains(&list, NULL));

  cldb_skiplist_iterator_init(&iter, &list);
  CLDB_EXPECT_FALSE(cldb_skiplist_iterator_valid(&iter));
  CLDB_EXPECT_EQ_PTR(NULL, cldb_skiplist_iterator_key(&iter));
  cldb_skiplist_iterator_seek_to_first(&iter);
  CLDB_EXPECT_FALSE(cldb_skiplist_iterator_valid(&iter));
  cldb_skiplist_iterator_seek_to_last(&iter);
  CLDB_EXPECT_FALSE(cldb_skiplist_iterator_valid(&iter));
  cldb_skiplist_iterator_seek(&iter, (const char *)&target);
  CLDB_EXPECT_FALSE(cldb_skiplist_iterator_valid(&iter));
  cldb_skiplist_iterator_next(&iter);
  CLDB_EXPECT_FALSE(cldb_skiplist_iterator_valid(&iter));
  cldb_skiplist_iterator_prev(&iter);
  CLDB_EXPECT_FALSE(cldb_skiplist_iterator_valid(&iter));

  /* NULL 对象/参数不应崩溃。 */
  cldb_skiplist_iterator_init(NULL, &list);
  cldb_skiplist_iterator_init(&iter, NULL);
  CLDB_EXPECT_FALSE(cldb_skiplist_iterator_valid(NULL));
  CLDB_EXPECT_EQ_PTR(NULL, cldb_skiplist_iterator_key(NULL));
  cldb_skiplist_iterator_next(NULL);
  cldb_skiplist_iterator_prev(NULL);
  cldb_skiplist_iterator_seek(NULL, (const char *)&target);
  cldb_skiplist_iterator_seek_to_first(NULL);
  cldb_skiplist_iterator_seek_to_last(NULL);
  cldb_skiplist_iterator_seek(&iter, NULL);
  CLDB_EXPECT_FALSE(cldb_skiplist_iterator_valid(&iter));
  cldb_skiplist_iterator_seek_to_first(&iter);
  CLDB_EXPECT_FALSE(cldb_skiplist_iterator_valid(&iter));
  cldb_skiplist_iterator_seek_to_last(&iter);
  CLDB_EXPECT_FALSE(cldb_skiplist_iterator_valid(&iter));

  CLDB_EXPECT_FALSE(cldb_skiplist_insert(&list, NULL));
  CLDB_EXPECT_FALSE(cldb_skiplist_insert(NULL, (const char *)&target));

  cldb_arena_destroy(&arena);
}

CLDB_TEST(skiplist_insert_and_lookup) {
  const uint32_t kN = UINT32_C(2000);
  const uint32_t kR = UINT32_C(5000);
  cldb_arena_t arena;
  cldb_skiplist_t list;
  cldb_random_t rnd;
  bool present[5000];
  uint64_t model[2000];
  size_t model_count = 0;
  uint32_t i;

  cldb_arena_init(&arena);
  CLDB_ASSERT_TRUE(cldb_skiplist_init(&list, &arena, compare_u64, NULL));
  cldb_random_init(&rnd, 1000);
  memset(present, 0, sizeof(present));

  for (i = 0; i < kN; ++i) {
    uint64_t key = (uint64_t)(cldb_random_next(&rnd) % kR);
    char *storage;

    if (present[(size_t)key]) {
      continue;
    }
    storage = allocate_u64(&arena, key);
    CLDB_ASSERT_TRUE(storage != NULL);
    CLDB_ASSERT_TRUE(cldb_skiplist_insert(&list, storage));
    present[(size_t)key] = true;
    CLDB_ASSERT_TRUE(model_insert(model, &model_count,
                                  sizeof(model) / sizeof(model[0]), key));
  }
  CLDB_ASSERT_TRUE(model_count > 0);

  for (i = 0; i < kR; ++i) {
    uint64_t key = (uint64_t)i;
    bool expected = present[(size_t)key];

    CLDB_EXPECT_TRUE(cldb_skiplist_contains(&list, (const char *)&key) ==
                     expected);
  }
  {
    uint64_t beyond = (uint64_t)kR;
    uint64_t huge = UINT64_MAX;

    CLDB_EXPECT_FALSE(cldb_skiplist_contains(&list, (const char *)&beyond));
    CLDB_EXPECT_FALSE(cldb_skiplist_contains(&list, (const char *)&huge));
  }

  {
    cldb_skiplist_iterator_t iter;
    uint64_t zero = 0;
    uint64_t beyond = (uint64_t)kR;

    cldb_skiplist_iterator_init(&iter, &list);
    CLDB_EXPECT_FALSE(cldb_skiplist_iterator_valid(&iter));
    CLDB_EXPECT_EQ_PTR(NULL, cldb_skiplist_iterator_key(&iter));

    cldb_skiplist_iterator_seek(&iter, (const char *)&zero);
    CLDB_ASSERT_TRUE(cldb_skiplist_iterator_valid(&iter));
    CLDB_EXPECT_EQ_UINT(model[0],
                        decode_u64(cldb_skiplist_iterator_key(&iter)));

    cldb_skiplist_iterator_seek_to_first(&iter);
    CLDB_ASSERT_TRUE(cldb_skiplist_iterator_valid(&iter));
    CLDB_EXPECT_EQ_UINT(model[0],
                        decode_u64(cldb_skiplist_iterator_key(&iter)));

    cldb_skiplist_iterator_seek_to_last(&iter);
    CLDB_ASSERT_TRUE(cldb_skiplist_iterator_valid(&iter));
    CLDB_EXPECT_EQ_UINT(model[model_count - 1],
                        decode_u64(cldb_skiplist_iterator_key(&iter)));

    cldb_skiplist_iterator_seek(&iter, (const char *)&beyond);
    CLDB_EXPECT_FALSE(cldb_skiplist_iterator_valid(&iter));
  }

  /* 顺序遍历全部节点。 */
  {
    cldb_skiplist_iterator_t iter;
    size_t index;

    cldb_skiplist_iterator_init(&iter, &list);
    cldb_skiplist_iterator_seek_to_first(&iter);
    for (index = 0; index < model_count; ++index) {
      CLDB_ASSERT_TRUE(cldb_skiplist_iterator_valid(&iter));
      CLDB_EXPECT_EQ_UINT(
          model[index], decode_u64(cldb_skiplist_iterator_key(&iter)));
      cldb_skiplist_iterator_next(&iter);
    }
    CLDB_EXPECT_FALSE(cldb_skiplist_iterator_valid(&iter));
    CLDB_EXPECT_EQ_PTR(NULL, cldb_skiplist_iterator_key(&iter));
    cldb_skiplist_iterator_next(&iter);
    CLDB_EXPECT_FALSE(cldb_skiplist_iterator_valid(&iter));
  }

  /* 每个 target 的 lower_bound 结果及其后两个节点应与模型一致。 */
  for (i = 0; i < kR; ++i) {
    cldb_skiplist_iterator_t iter;
    uint64_t target = (uint64_t)i;
    size_t index = lower_bound_u64(model, model_count, target);
    int j;

    cldb_skiplist_iterator_init(&iter, &list);
    cldb_skiplist_iterator_seek(&iter, (const char *)&target);
    for (j = 0; j < 3; ++j) {
      if (index == model_count) {
        CLDB_EXPECT_FALSE(cldb_skiplist_iterator_valid(&iter));
        break;
      }
      CLDB_ASSERT_TRUE(cldb_skiplist_iterator_valid(&iter));
      CLDB_EXPECT_EQ_UINT(
          model[index], decode_u64(cldb_skiplist_iterator_key(&iter)));
      ++index;
      cldb_skiplist_iterator_next(&iter);
    }
  }

  /* 反向遍历全部节点，并检查首节点 Prev 变为无效。 */
  {
    cldb_skiplist_iterator_t iter;
    size_t index = model_count;

    cldb_skiplist_iterator_init(&iter, &list);
    cldb_skiplist_iterator_seek_to_last(&iter);
    while (index > 0) {
      --index;
      CLDB_ASSERT_TRUE(cldb_skiplist_iterator_valid(&iter));
      CLDB_EXPECT_EQ_UINT(
          model[index], decode_u64(cldb_skiplist_iterator_key(&iter)));
      cldb_skiplist_iterator_prev(&iter);
    }
    CLDB_EXPECT_FALSE(cldb_skiplist_iterator_valid(&iter));
    cldb_skiplist_iterator_prev(&iter);
    CLDB_EXPECT_FALSE(cldb_skiplist_iterator_valid(&iter));

    cldb_skiplist_iterator_seek_to_first(&iter);
    CLDB_ASSERT_TRUE(cldb_skiplist_iterator_valid(&iter));
    cldb_skiplist_iterator_prev(&iter);
    CLDB_EXPECT_FALSE(cldb_skiplist_iterator_valid(&iter));
  }

  cldb_arena_destroy(&arena);
}

CLDB_TEST(skiplist_duplicate_key) {
  cldb_arena_t arena;
  cldb_skiplist_t list;
  cldb_skiplist_iterator_t iter;
  uint64_t values[] = {3, 1, 2};
  uint64_t order[] = {1, 2, 3};
  uint64_t duplicate = 2;
  char *first;
  size_t i;
  size_t count = 0;

  cldb_arena_init(&arena);
  CLDB_ASSERT_TRUE(cldb_skiplist_init(&list, &arena, compare_u64, NULL));
  for (i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
    char *storage = allocate_u64(&arena, values[i]);

    CLDB_ASSERT_TRUE(storage != NULL);
    CLDB_ASSERT_TRUE(cldb_skiplist_insert(&list, storage));
  }

  /* 用一个不同地址、相同数值的 key 再插入，必须拒绝且不改变链表。 */
  first = allocate_u64(&arena, duplicate);
  CLDB_ASSERT_TRUE(first != NULL);
  CLDB_EXPECT_FALSE(cldb_skiplist_insert(&list, first));
  CLDB_EXPECT_TRUE(cldb_skiplist_contains(&list, (const char *)&duplicate));

  cldb_skiplist_iterator_init(&iter, &list);
  cldb_skiplist_iterator_seek_to_first(&iter);
  while (cldb_skiplist_iterator_valid(&iter)) {
    CLDB_ASSERT_TRUE(count < sizeof(order) / sizeof(order[0]));
    CLDB_EXPECT_EQ_UINT(order[count],
                        decode_u64(cldb_skiplist_iterator_key(&iter)));
    ++count;
    cldb_skiplist_iterator_next(&iter);
  }
  CLDB_EXPECT_EQ_UINT(sizeof(order) / sizeof(order[0]), count);

  cldb_arena_destroy(&arena);
}

CLDB_TEST(skiplist_height_distribution) {
  const uint32_t kSamples = UINT32_C(65536);
  cldb_random_t rnd;
  uint32_t counts[CLDB_SKIPLIST_MAX_HEIGHT] = {0};
  uint32_t at_least[4] = {0, 0, 0, 0};
  uint32_t max_seen = 0;
  uint32_t i;

  cldb_random_init(&rnd, 12345);
  CLDB_EXPECT_EQ_INT(1, cldb_skiplist_random_height(NULL));
  for (i = 0; i < kSamples; ++i) {
    int height = cldb_skiplist_random_height(&rnd);
    int level;

    CLDB_ASSERT_TRUE(height >= 1);
    CLDB_ASSERT_TRUE(height <= CLDB_SKIPLIST_MAX_HEIGHT);
    counts[height - 1] += 1;
    if ((uint32_t)height > max_seen) {
      max_seen = (uint32_t)height;
    }
    for (level = 0; level < 4; ++level) {
      if (height >= level + 2) {
        at_least[level] += 1;
      }
    }
  }

  CLDB_EXPECT_EQ_UINT(kSamples, counts[0] + at_least[0]);
  /* 每升一层概率约 1/4；用宽区间避免对随机序列过度约束。 */
  CLDB_EXPECT_TRUE(counts[0] >= kSamples * 7 / 10);
  CLDB_EXPECT_TRUE(counts[0] <= kSamples * 8 / 10);
  CLDB_EXPECT_TRUE(at_least[0] >= kSamples / 5);
  CLDB_EXPECT_TRUE(at_least[0] <= kSamples * 3 / 10);
  CLDB_EXPECT_TRUE(at_least[1] >= kSamples / 20);
  CLDB_EXPECT_TRUE(at_least[1] <= kSamples / 10);
  CLDB_EXPECT_TRUE(at_least[2] >= kSamples / 100);
  CLDB_EXPECT_TRUE(at_least[2] <= kSamples / 40);
  CLDB_EXPECT_TRUE(max_seen >= 5);
  CLDB_EXPECT_TRUE(max_seen <= CLDB_SKIPLIST_MAX_HEIGHT);
}

CLDB_TEST(skiplist_max_height_bound) {
  const uint32_t kN = UINT32_C(10000);
  cldb_arena_t arena;
  cldb_skiplist_t list;
  uint32_t i;

  cldb_arena_init(&arena);
  CLDB_ASSERT_TRUE(cldb_skiplist_init(&list, &arena, compare_u64, NULL));
  for (i = 0; i < kN; ++i) {
    char *storage = allocate_u64(&arena, (uint64_t)i);

    CLDB_ASSERT_TRUE(storage != NULL);
    CLDB_ASSERT_TRUE(cldb_skiplist_insert(&list, storage));
    CLDB_EXPECT_TRUE(cldb_skiplist_max_height(&list) >= 1);
    CLDB_EXPECT_TRUE(cldb_skiplist_max_height(&list) <=
                     CLDB_SKIPLIST_MAX_HEIGHT);
  }
  /* 节点足够多时必然有高于 1 层的节点，且 max height 不越界。 */
  CLDB_EXPECT_TRUE(cldb_skiplist_max_height(&list) > 1);
  CLDB_EXPECT_TRUE(cldb_skiplist_max_height(&list) <=
                   CLDB_SKIPLIST_MAX_HEIGHT);
  {
    uint64_t zero = 0;

    CLDB_EXPECT_TRUE(cldb_skiplist_contains(&list, (const char *)&zero));
  }
  cldb_arena_destroy(&arena);
}

/* 并发测试使用的初始 key 为 4*i，写者插入 4*i+1，全部唯一且范围固定。 */
typedef struct {
  cldb_skiplist_t *list;
  cldb_arena_t *arena;
  const uint32_t *order; /* 0..initial_count-1 的一个排列 */
  uint32_t initial_count;
  cldb_atomic_u32_t *quit; /* release 写入 1 时读者退出 */
  uint32_t error;          /* 线程内不使用 harness，用错误码报告 */
} skiplist_writer_state_t;

typedef struct {
  const cldb_skiplist_t *list;
  uint32_t initial_count;
  cldb_atomic_u32_t *quit;
  uint32_t error;
} skiplist_reader_state_t;

enum {
  SKIPLIST_CONCURRENT_ERROR_KEY = 1,
  SKIPLIST_CONCURRENT_ERROR_ORDER = 2,
  SKIPLIST_CONCURRENT_ERROR_MISSING_INITIAL = 3,
  SKIPLIST_CONCURRENT_ERROR_ALLOC = 4,
  SKIPLIST_CONCURRENT_ERROR_INSERT = 5
};

static bool concurrent_key_valid(uint64_t key, uint32_t initial_count) {
  if (key >= (uint64_t)initial_count * UINT64_C(4)) {
    return false;
  }
  return (key & UINT64_C(3)) == 0 || (key & UINT64_C(3)) == 1;
}

static bool concurrent_scan_forward(skiplist_reader_state_t *state) {
  cldb_skiplist_iterator_t iter;
  uint64_t previous = 0;
  bool has_previous = false;
  uint32_t initial_seen = 0;

  cldb_skiplist_iterator_init(&iter, state->list);
  cldb_skiplist_iterator_seek_to_first(&iter);
  while (cldb_skiplist_iterator_valid(&iter)) {
    const char *raw = cldb_skiplist_iterator_key(&iter);
    uint64_t key;

    if (raw == NULL) {
      state->error = SKIPLIST_CONCURRENT_ERROR_KEY;
      return false;
    }
    key = decode_u64(raw);
    if (!concurrent_key_valid(key, state->initial_count)) {
      state->error = SKIPLIST_CONCURRENT_ERROR_KEY;
      return false;
    }
    if (has_previous && key <= previous) {
      state->error = SKIPLIST_CONCURRENT_ERROR_ORDER;
      return false;
    }
    if ((key & UINT64_C(3)) == 0) {
      ++initial_seen;
    }
    previous = key;
    has_previous = true;
    cldb_skiplist_iterator_next(&iter);
  }
  if (initial_seen != state->initial_count) {
    state->error = SKIPLIST_CONCURRENT_ERROR_MISSING_INITIAL;
    return false;
  }
  return true;
}

static bool concurrent_scan_backward(skiplist_reader_state_t *state) {
  cldb_skiplist_iterator_t iter;
  uint64_t previous = UINT64_MAX;
  uint32_t initial_seen = 0;

  cldb_skiplist_iterator_init(&iter, state->list);
  cldb_skiplist_iterator_seek_to_last(&iter);
  while (cldb_skiplist_iterator_valid(&iter)) {
    const char *raw = cldb_skiplist_iterator_key(&iter);
    uint64_t key;

    if (raw == NULL) {
      state->error = SKIPLIST_CONCURRENT_ERROR_KEY;
      return false;
    }
    key = decode_u64(raw);
    if (!concurrent_key_valid(key, state->initial_count)) {
      state->error = SKIPLIST_CONCURRENT_ERROR_KEY;
      return false;
    }
    if (key >= previous) {
      state->error = SKIPLIST_CONCURRENT_ERROR_ORDER;
      return false;
    }
    if ((key & UINT64_C(3)) == 0) {
      ++initial_seen;
    }
    previous = key;
    cldb_skiplist_iterator_prev(&iter);
  }
  if (initial_seen != state->initial_count) {
    state->error = SKIPLIST_CONCURRENT_ERROR_MISSING_INITIAL;
    return false;
  }
  return true;
}

static void *concurrent_reader(void *arg) {
  skiplist_reader_state_t *state = arg;

  do {
    if (!concurrent_scan_forward(state) ||
        !concurrent_scan_backward(state)) {
      break;
    }
  } while (cldb_atomic_u32_load(state->quit, memory_order_acquire) == 0);
  return NULL;
}

static void *concurrent_writer(void *arg) {
  skiplist_writer_state_t *state = arg;
  uint32_t i;

  for (i = 0; i < state->initial_count; ++i) {
    uint64_t key = ((uint64_t)state->order[i] << 2) | UINT64_C(1);
    char *storage = allocate_u64(state->arena, key);

    if (storage == NULL) {
      state->error = SKIPLIST_CONCURRENT_ERROR_ALLOC;
      break;
    }
    if (!cldb_skiplist_insert(state->list, storage)) {
      state->error = SKIPLIST_CONCURRENT_ERROR_INSERT;
      break;
    }
  }
  cldb_atomic_u32_store(state->quit, 1, memory_order_release);
  return NULL;
}

static bool verify_concurrent_final(const cldb_skiplist_t *list,
                                    uint32_t initial_count) {
  cldb_skiplist_iterator_t iter;
  uint32_t seen = 0;
  uint32_t expected_count = initial_count * 2;

  cldb_skiplist_iterator_init(&iter, list);
  cldb_skiplist_iterator_seek_to_first(&iter);
  while (cldb_skiplist_iterator_valid(&iter)) {
    uint64_t expected =
        ((uint64_t)(seen / 2) << 2) | (uint64_t)(seen % 2);

    if (decode_u64(cldb_skiplist_iterator_key(&iter)) != expected) {
      return false;
    }
    ++seen;
    cldb_skiplist_iterator_next(&iter);
  }
  return seen == expected_count;
}

/* 跑一轮单写多读；任一阶段失败返回 false，且总会停止线程并释放资源。 */
static bool run_concurrent_round(uint32_t seed) {
  const uint32_t initial_count = UINT32_C(1000);
  const uint32_t reader_count = UINT32_C(2);
  cldb_arena_t arena;
  cldb_skiplist_t list;
  cldb_random_t rnd;
  uint32_t *order = NULL;
  cldb_atomic_u32_t quit;
  skiplist_writer_state_t writer_state;
  skiplist_reader_state_t reader_states[2];
  cldb_thread_t writer_thread;
  cldb_thread_t reader_threads[2];
  bool writer_started = false;
  bool reader_started[2] = {false, false};
  bool ok = true;
  uint32_t i;

  cldb_arena_init(&arena);
  if (!cldb_skiplist_init(&list, &arena, compare_u64, NULL)) {
    cldb_arena_destroy(&arena);
    return false;
  }

  for (i = 0; i < initial_count; ++i) {
    char *storage = allocate_u64(&arena, (uint64_t)i << 2);

    if (storage == NULL || !cldb_skiplist_insert(&list, storage)) {
      cldb_arena_destroy(&arena);
      return false;
    }
  }

  order = malloc((size_t)initial_count * sizeof(*order));
  if (order == NULL) {
    cldb_arena_destroy(&arena);
    return false;
  }
  for (i = 0; i < initial_count; ++i) {
    order[i] = i;
  }
  cldb_random_init(&rnd, seed);
  for (i = initial_count; i > 1; --i) {
    uint32_t j = cldb_random_uniform(&rnd, i);
    uint32_t tmp = order[i - 1];

    order[i - 1] = order[(size_t)j];
    order[(size_t)j] = tmp;
  }

  cldb_atomic_u32_init(&quit, 0);
  writer_state.list = &list;
  writer_state.arena = &arena;
  writer_state.order = order;
  writer_state.initial_count = initial_count;
  writer_state.quit = &quit;
  writer_state.error = 0;

  cldb_thread_init(&writer_thread);
  for (i = 0; i < reader_count; ++i) {
    reader_states[i].list = &list;
    reader_states[i].initial_count = initial_count;
    reader_states[i].quit = &quit;
    reader_states[i].error = 0;
    cldb_thread_init(&reader_threads[i]);
    if (!cldb_status_is_ok(cldb_thread_start(&reader_threads[i],
                                             concurrent_reader,
                                             &reader_states[i]))) {
      ok = false;
      break;
    }
    reader_started[i] = true;
  }

  if (ok &&
      !cldb_status_is_ok(cldb_thread_start(&writer_thread, concurrent_writer,
                                           &writer_state))) {
    ok = false;
  }
  if (ok) {
    writer_started = true;
  }

  if (writer_started) {
    if (!cldb_status_is_ok(cldb_thread_join(&writer_thread, NULL))) {
      ok = false;
    }
  } else {
    cldb_atomic_u32_store(&quit, 1, memory_order_release);
  }
  /* 写者正常结束时会释放 quit；这里再写一次，覆盖写者启动失败等路径。 */
  cldb_atomic_u32_store(&quit, 1, memory_order_release);

  for (i = 0; i < reader_count; ++i) {
    if (reader_started[i]) {
      if (!cldb_status_is_ok(cldb_thread_join(&reader_threads[i], NULL))) {
        ok = false;
      }
    }
  }

  if (writer_state.error != 0) {
    ok = false;
  }
  for (i = 0; i < reader_count; ++i) {
    if (reader_states[i].error != 0) {
      ok = false;
    }
  }
  if (ok && !verify_concurrent_final(&list, initial_count)) {
    ok = false;
  }

  free(order);
  cldb_arena_destroy(&arena);
  return ok;
}

CLDB_TEST(skiplist_concurrent_single_writer_multi_reader) {
  uint32_t run;

  for (run = 0; run < 5; ++run) {
    CLDB_ASSERT_TRUE(run_concurrent_round(UINT32_C(1000) + run * 100));
  }
}

void cldb_register_skiplist_tests(void) {
  CLDB_RUN_TEST(skiplist_empty);
  CLDB_RUN_TEST(skiplist_insert_and_lookup);
  CLDB_RUN_TEST(skiplist_duplicate_key);
  CLDB_RUN_TEST(skiplist_height_distribution);
  CLDB_RUN_TEST(skiplist_max_height_bound);
  CLDB_RUN_TEST(skiplist_concurrent_single_writer_multi_reader);
}
