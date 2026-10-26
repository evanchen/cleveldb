#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "harness.h"
#include "port/port_mutex.h"
#include "port/port_thread.h"
#include "status_test_util.h"

/* 线程内不能使用 harness 的 ASSERT/EXPECT（longjmp 仅限测试主线程），
 * 失败统一返回非 NULL，由 join 结果或共享状态暴露。 */
static void *thread_failed(void) {
  return (void *)(uintptr_t)1;
}

static bool lock_mutex(cldb_mutex_t *mutex) {
  return cldb_status_is_ok(cldb_mutex_lock(mutex));
}

static bool unlock_mutex(cldb_mutex_t *mutex) {
  return cldb_status_is_ok(cldb_mutex_unlock(mutex));
}

CLDB_TEST(mutex_lifecycle) {
  cldb_mutex_t mutex = {0};

  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT, cldb_mutex_lock(&mutex));
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT, cldb_mutex_unlock(&mutex));
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT, cldb_mutex_destroy(&mutex));
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT, cldb_mutex_init(NULL));
  CLDB_EXPECT_OK(cldb_mutex_destroy(NULL));

  CLDB_ASSERT_OK(cldb_mutex_init(&mutex));
  CLDB_EXPECT_OK(cldb_mutex_lock(&mutex));
  CLDB_EXPECT_OK(cldb_mutex_unlock(&mutex));
  CLDB_ASSERT_OK(cldb_mutex_destroy(&mutex));

  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT, cldb_mutex_lock(&mutex));
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT, cldb_mutex_unlock(&mutex));
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT, cldb_mutex_destroy(&mutex));

  /* destroy 后允许重新 init。 */
  CLDB_ASSERT_OK(cldb_mutex_init(&mutex));
  CLDB_ASSERT_OK(cldb_mutex_destroy(&mutex));
}

CLDB_TEST(condvar_lifecycle) {
  cldb_mutex_t mutex = {0};
  cldb_condvar_t condvar = {0};

  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_condvar_init(NULL, &mutex));
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_condvar_init(&condvar, NULL));
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_condvar_init(&condvar, &mutex));
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_condvar_wait(&condvar));
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_condvar_signal(&condvar));
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_condvar_broadcast(&condvar));
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_condvar_destroy(&condvar));
  CLDB_EXPECT_OK(cldb_condvar_destroy(NULL));

  CLDB_ASSERT_OK(cldb_mutex_init(&mutex));
  CLDB_ASSERT_OK(cldb_condvar_init(&condvar, &mutex));
  CLDB_ASSERT_OK(cldb_mutex_lock(&mutex));
  CLDB_EXPECT_OK(cldb_condvar_signal(&condvar));
  CLDB_EXPECT_OK(cldb_condvar_broadcast(&condvar));
  CLDB_ASSERT_OK(cldb_mutex_unlock(&mutex));
  CLDB_ASSERT_OK(cldb_condvar_destroy(&condvar));
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_condvar_signal(&condvar));
  CLDB_ASSERT_OK(cldb_mutex_destroy(&mutex));
}

enum {
  kMutexThreads = 4,
  kMutexIterations = 50000,
};

typedef struct {
  cldb_mutex_t mutex;
  uint64_t counter; /* 受 mutex 保护 */
} mutex_counter_t;

/* 每次读-改-写都在锁内，验证互斥量没有丢失更新。 */
static void *counter_thread(void *arg) {
  mutex_counter_t *state = arg;
  uint32_t i;

  for (i = 0; i < (uint32_t)kMutexIterations; ++i) {
    if (!lock_mutex(&state->mutex)) {
      return thread_failed();
    }
    ++state->counter;
    if (!unlock_mutex(&state->mutex)) {
      return thread_failed();
    }
  }
  return NULL;
}

CLDB_TEST(mutex_mutual_exclusion) {
  mutex_counter_t state;
  cldb_thread_t threads[kMutexThreads];
  size_t i;
  void *result;

  state.counter = 0;
  CLDB_ASSERT_OK(cldb_mutex_init(&state.mutex));

  for (i = 0; i < (size_t)kMutexThreads; ++i) {
    cldb_thread_init(&threads[i]);
    CLDB_ASSERT_OK(cldb_thread_start(&threads[i], counter_thread, &state));
  }
  for (i = 0; i < (size_t)kMutexThreads; ++i) {
    result = thread_failed();
    CLDB_ASSERT_OK(cldb_thread_join(&threads[i], &result));
    CLDB_EXPECT_EQ_PTR(NULL, result);
  }
  CLDB_EXPECT_EQ_UINT((uint64_t)kMutexThreads * (uint64_t)kMutexIterations,
                      state.counter);
  CLDB_ASSERT_OK(cldb_mutex_destroy(&state.mutex));
}

enum {
  kQueueCapacity = 8,
  kProducerItems = 20000,
};

typedef struct {
  cldb_mutex_t mutex;
  cldb_condvar_t not_empty; /* 与 mutex 绑定；有新元素时唤醒消费者 */
  cldb_condvar_t not_full;  /* 与 mutex 绑定；有空位时唤醒生产者 */
  uint32_t items[kQueueCapacity];
  size_t head;
  size_t tail;
  size_t count;
  bool done;
  uint64_t consumed_sum;
  size_t consumed_count;
} producer_consumer_t;

/* 生产 0..N-1；队列满时在 not_full 上按谓词循环等待。 */
static void *producer_thread(void *arg) {
  producer_consumer_t *state = arg;
  uint32_t i;

  for (i = 0; i < (uint32_t)kProducerItems; ++i) {
    if (!lock_mutex(&state->mutex)) {
      return thread_failed();
    }
    while (state->count == (size_t)kQueueCapacity) {
      if (!cldb_status_is_ok(cldb_condvar_wait(&state->not_full))) {
        (void)cldb_mutex_unlock(&state->mutex);
        return thread_failed();
      }
    }
    state->items[state->tail] = i;
    state->tail = (state->tail + 1) % (size_t)kQueueCapacity;
    ++state->count;
    if (!cldb_status_is_ok(cldb_condvar_signal(&state->not_empty))) {
      (void)cldb_mutex_unlock(&state->mutex);
      return thread_failed();
    }
    if (!unlock_mutex(&state->mutex)) {
      return thread_failed();
    }
  }

  if (!lock_mutex(&state->mutex)) {
    return thread_failed();
  }
  state->done = true;
  if (!cldb_status_is_ok(cldb_condvar_broadcast(&state->not_empty))) {
    (void)cldb_mutex_unlock(&state->mutex);
    return thread_failed();
  }
  if (!unlock_mutex(&state->mutex)) {
    return thread_failed();
  }
  return NULL;
}

/* 消费到 done 且队列为空；等待必须放在谓词循环中。 */
static void *consumer_thread(void *arg) {
  producer_consumer_t *state = arg;

  for (;;) {
    uint32_t value;

    if (!lock_mutex(&state->mutex)) {
      return thread_failed();
    }
    while (state->count == 0 && !state->done) {
      if (!cldb_status_is_ok(cldb_condvar_wait(&state->not_empty))) {
        (void)cldb_mutex_unlock(&state->mutex);
        return thread_failed();
      }
    }
    if (state->count == 0 && state->done) {
      if (!unlock_mutex(&state->mutex)) {
        return thread_failed();
      }
      break;
    }

    value = state->items[state->head];
    state->head = (state->head + 1) % (size_t)kQueueCapacity;
    --state->count;
    state->consumed_sum += value;
    ++state->consumed_count;
    if (!cldb_status_is_ok(cldb_condvar_signal(&state->not_full))) {
      (void)cldb_mutex_unlock(&state->mutex);
      return thread_failed();
    }
    if (!unlock_mutex(&state->mutex)) {
      return thread_failed();
    }
  }
  return NULL;
}

CLDB_TEST(condvar_producer_consumer) {
  producer_consumer_t state;
  cldb_thread_t producer;
  cldb_thread_t consumer;
  uint64_t expected_sum;
  void *result;

  state.head = 0;
  state.tail = 0;
  state.count = 0;
  state.done = false;
  state.consumed_sum = 0;
  state.consumed_count = 0;
  CLDB_ASSERT_OK(cldb_mutex_init(&state.mutex));
  CLDB_ASSERT_OK(cldb_condvar_init(&state.not_empty, &state.mutex));
  CLDB_ASSERT_OK(cldb_condvar_init(&state.not_full, &state.mutex));
  cldb_thread_init(&producer);
  cldb_thread_init(&consumer);

  CLDB_ASSERT_OK(cldb_thread_start(&producer, producer_thread, &state));
  CLDB_ASSERT_OK(cldb_thread_start(&consumer, consumer_thread, &state));

  result = thread_failed();
  CLDB_ASSERT_OK(cldb_thread_join(&producer, &result));
  CLDB_EXPECT_EQ_PTR(NULL, result);
  result = thread_failed();
  CLDB_ASSERT_OK(cldb_thread_join(&consumer, &result));
  CLDB_EXPECT_EQ_PTR(NULL, result);

  expected_sum = ((uint64_t)kProducerItems - 1) * (uint64_t)kProducerItems / 2;
  CLDB_EXPECT_EQ_UINT((uint64_t)kProducerItems, state.consumed_count);
  CLDB_EXPECT_EQ_UINT(expected_sum, state.consumed_sum);

  CLDB_ASSERT_OK(cldb_condvar_destroy(&state.not_empty));
  CLDB_ASSERT_OK(cldb_condvar_destroy(&state.not_full));
  CLDB_ASSERT_OK(cldb_mutex_destroy(&state.mutex));
}

enum { kBroadcastWaiters = 8 };

typedef struct {
  cldb_mutex_t mutex;
  cldb_condvar_t ready; /* 等待者报告已就绪 */
  cldb_condvar_t go_cv; /* 主线程广播开始 */
  int ready_count;
  int woke_count;
  bool go;
} broadcast_state_t;

static void *broadcast_waiter(void *arg) {
  broadcast_state_t *state = arg;

  if (!lock_mutex(&state->mutex)) {
    return thread_failed();
  }
  ++state->ready_count;
  if (!cldb_status_is_ok(cldb_condvar_signal(&state->ready))) {
    (void)cldb_mutex_unlock(&state->mutex);
    return thread_failed();
  }
  while (!state->go) {
    if (!cldb_status_is_ok(cldb_condvar_wait(&state->go_cv))) {
      (void)cldb_mutex_unlock(&state->mutex);
      return thread_failed();
    }
  }
  ++state->woke_count;
  if (!unlock_mutex(&state->mutex)) {
    return thread_failed();
  }
  return NULL;
}

CLDB_TEST(condvar_broadcast) {
  broadcast_state_t state;
  cldb_thread_t waiters[kBroadcastWaiters];
  size_t i;
  void *result;

  state.ready_count = 0;
  state.woke_count = 0;
  state.go = false;
  CLDB_ASSERT_OK(cldb_mutex_init(&state.mutex));
  CLDB_ASSERT_OK(cldb_condvar_init(&state.ready, &state.mutex));
  CLDB_ASSERT_OK(cldb_condvar_init(&state.go_cv, &state.mutex));

  for (i = 0; i < (size_t)kBroadcastWaiters; ++i) {
    cldb_thread_init(&waiters[i]);
    CLDB_ASSERT_OK(cldb_thread_start(&waiters[i], broadcast_waiter, &state));
  }

  CLDB_ASSERT_OK(cldb_mutex_lock(&state.mutex));
  while (state.ready_count < kBroadcastWaiters) {
    CLDB_ASSERT_OK(cldb_condvar_wait(&state.ready));
  }
  state.go = true;
  CLDB_ASSERT_OK(cldb_condvar_broadcast(&state.go_cv));
  CLDB_ASSERT_OK(cldb_mutex_unlock(&state.mutex));

  for (i = 0; i < (size_t)kBroadcastWaiters; ++i) {
    result = thread_failed();
    CLDB_ASSERT_OK(cldb_thread_join(&waiters[i], &result));
    CLDB_EXPECT_EQ_PTR(NULL, result);
  }
  CLDB_EXPECT_EQ_INT(kBroadcastWaiters, state.ready_count);
  CLDB_EXPECT_EQ_INT(kBroadcastWaiters, state.woke_count);

  CLDB_ASSERT_OK(cldb_condvar_destroy(&state.ready));
  CLDB_ASSERT_OK(cldb_condvar_destroy(&state.go_cv));
  CLDB_ASSERT_OK(cldb_mutex_destroy(&state.mutex));
}

void cldb_register_mutex_tests(void) {
  CLDB_RUN_TEST(mutex_lifecycle);
  CLDB_RUN_TEST(condvar_lifecycle);
  CLDB_RUN_TEST(mutex_mutual_exclusion);
  CLDB_RUN_TEST(condvar_producer_consumer);
  CLDB_RUN_TEST(condvar_broadcast);
}
