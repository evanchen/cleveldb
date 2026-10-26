#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "harness.h"
#include "port/port_mutex.h"
#include "port/port_thread.h"
#include "status_test_util.h"

/* 线程内不能使用 harness 的 ASSERT/EXPECT（longjmp 仅限测试主线程）。 */
static void *thread_failed(void) {
  return (void *)(uintptr_t)1;
}

static void *identity_thread(void *arg) {
  return arg;
}

CLDB_TEST(thread_start_join) {
  cldb_thread_t thread;
  void *result = NULL;
  int value = 42;

  cldb_thread_init(NULL);
  cldb_thread_init(&thread);
  CLDB_ASSERT_OK(cldb_thread_start(&thread, identity_thread, &value));
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_thread_start(&thread, identity_thread, NULL));
  CLDB_ASSERT_OK(cldb_thread_join(&thread, &result));
  CLDB_EXPECT_EQ_PTR(&value, result);
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_thread_join(&thread, NULL));
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_thread_detach(&thread));

  /* join 后对象可复用；join 的 result 允许为 NULL。 */
  CLDB_ASSERT_OK(cldb_thread_start(&thread, identity_thread, NULL));
  CLDB_ASSERT_OK(cldb_thread_join(&thread, NULL));
}

CLDB_TEST(thread_invalid_arguments) {
  cldb_thread_t thread;
  void *result = NULL;

  cldb_thread_init(&thread);
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_thread_start(NULL, identity_thread, NULL));
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_thread_start(&thread, NULL, NULL));
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_thread_join(NULL, &result));
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_thread_join(&thread, &result));
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT, cldb_thread_detach(NULL));
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT, cldb_thread_detach(&thread));
}

typedef struct {
  cldb_mutex_t mutex;
  cldb_condvar_t done_cv; /* 与 mutex 绑定；线程完成时通知主线程 */
  bool done;
  bool worker_ok;
} detached_state_t;

/* 分离后无法 join 取返回值，用 done/worker_ok 在锁内报告结果。 */
static void *detached_worker(void *arg) {
  detached_state_t *state = arg;
  cldb_status_t status = cldb_mutex_lock(&state->mutex);

  if (!cldb_status_is_ok(status)) {
    return thread_failed();
  }
  state->done = true;
  state->worker_ok = cldb_status_is_ok(cldb_condvar_signal(&state->done_cv));
  status = cldb_mutex_unlock(&state->mutex);
  if (!state->worker_ok || !cldb_status_is_ok(status)) {
    return thread_failed();
  }
  return NULL;
}

CLDB_TEST(thread_detach_lifecycle) {
  detached_state_t state;
  cldb_thread_t thread;

  state.done = false;
  state.worker_ok = false;
  CLDB_ASSERT_OK(cldb_mutex_init(&state.mutex));
  CLDB_ASSERT_OK(cldb_condvar_init(&state.done_cv, &state.mutex));
  cldb_thread_init(&thread);

  CLDB_ASSERT_OK(cldb_thread_start(&thread, detached_worker, &state));
  CLDB_ASSERT_OK(cldb_thread_detach(&thread));
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_thread_detach(&thread));
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_thread_join(&thread, NULL));

  /* 等待分离线程完成并报告结果。 */
  CLDB_ASSERT_OK(cldb_mutex_lock(&state.mutex));
  while (!state.done) {
    CLDB_ASSERT_OK(cldb_condvar_wait(&state.done_cv));
  }
  CLDB_EXPECT_TRUE(state.worker_ok);
  CLDB_ASSERT_OK(cldb_mutex_unlock(&state.mutex));

  CLDB_ASSERT_OK(cldb_condvar_destroy(&state.done_cv));
  CLDB_ASSERT_OK(cldb_mutex_destroy(&state.mutex));
}

/* 单调时钟经过的纳秒数；测试只跨很短时间，不会溢出。 */
static uint64_t elapsed_nanos(struct timespec start, struct timespec end) {
  uint64_t seconds = (uint64_t)(end.tv_sec - start.tv_sec);
  int64_t nanoseconds = (int64_t)end.tv_nsec - (int64_t)start.tv_nsec;

  if (nanoseconds < 0) {
    --seconds;
    nanoseconds += INT64_C(1000000000);
  }
  return seconds * UINT64_C(1000000000) + (uint64_t)nanoseconds;
}

CLDB_TEST(thread_sleep) {
  struct timespec start;
  struct timespec end;

  CLDB_EXPECT_OK(cldb_sleep_for_micros(0));
  CLDB_EXPECT_OK(cldb_sleep_for_millis(0));

  CLDB_ASSERT_EQ_INT(0, clock_gettime(CLOCK_MONOTONIC, &start));
  CLDB_ASSERT_OK(cldb_sleep_for_millis(5));
  CLDB_ASSERT_EQ_INT(0, clock_gettime(CLOCK_MONOTONIC, &end));
  /* 容忍时钟/调度粒度，只验证没有提前返回。 */
  CLDB_EXPECT_TRUE(elapsed_nanos(start, end) >= UINT64_C(4000000));

  CLDB_ASSERT_EQ_INT(0, clock_gettime(CLOCK_MONOTONIC, &start));
  CLDB_ASSERT_OK(cldb_sleep_for_micros(2000));
  CLDB_ASSERT_EQ_INT(0, clock_gettime(CLOCK_MONOTONIC, &end));
  CLDB_EXPECT_TRUE(elapsed_nanos(start, end) >= UINT64_C(1000000));
}

void cldb_register_thread_tests(void) {
  CLDB_RUN_TEST(thread_start_join);
  CLDB_RUN_TEST(thread_invalid_arguments);
  CLDB_RUN_TEST(thread_detach_lifecycle);
  CLDB_RUN_TEST(thread_sleep);
}
