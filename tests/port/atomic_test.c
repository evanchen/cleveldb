#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "harness.h"
#include "port/port_atomic.h"

/* 单线程验证 u32 的 init/load/store/fetch_add/CAS 与内存序参数。 */
CLDB_TEST(atomic_u32_single_thread) {
  cldb_atomic_u32_t value;
  uint32_t expected;

  cldb_atomic_u32_init(&value, 10);
  CLDB_ASSERT_EQ_UINT(10, cldb_atomic_u32_load(&value, memory_order_relaxed));

  cldb_atomic_u32_store(&value, 20, memory_order_release);
  CLDB_ASSERT_EQ_UINT(20, cldb_atomic_u32_load(&value, memory_order_acquire));

  CLDB_ASSERT_EQ_UINT(20, cldb_atomic_u32_fetch_add(&value, 5,
                                                    memory_order_relaxed));
  CLDB_ASSERT_EQ_UINT(25, cldb_atomic_u32_load(&value,
                                               memory_order_seq_cst));

  expected = 25;
  CLDB_ASSERT_TRUE(cldb_atomic_u32_compare_exchange(
      &value, &expected, 30, memory_order_acq_rel, memory_order_acquire));
  CLDB_ASSERT_EQ_UINT(25, expected);
  CLDB_ASSERT_EQ_UINT(30, cldb_atomic_u32_load(&value,
                                               memory_order_seq_cst));

  expected = 0;
  CLDB_ASSERT_FALSE(cldb_atomic_u32_compare_exchange(
      &value, &expected, 40, memory_order_acq_rel, memory_order_acquire));
  CLDB_ASSERT_EQ_UINT(30, expected);
  CLDB_ASSERT_EQ_UINT(30, cldb_atomic_u32_load(&value,
                                               memory_order_relaxed));
}

/* u64 覆盖超过 32 位的值以及 CAS 成功/失败。 */
CLDB_TEST(atomic_u64_single_thread) {
  cldb_atomic_u64_t value;
  uint64_t expected;

  cldb_atomic_u64_init(&value, UINT64_C(0x100000000));
  CLDB_ASSERT_EQ_UINT(UINT64_C(0x100000000),
                      cldb_atomic_u64_load(&value, memory_order_relaxed));
  CLDB_ASSERT_EQ_UINT(UINT64_C(0x100000000),
                      cldb_atomic_u64_fetch_add(&value, 7,
                                                memory_order_relaxed));
  CLDB_ASSERT_EQ_UINT(UINT64_C(0x100000007),
                      cldb_atomic_u64_load(&value, memory_order_acquire));

  expected = UINT64_C(0x100000007);
  CLDB_ASSERT_TRUE(cldb_atomic_u64_compare_exchange(
      &value, &expected, UINT64_C(0xffffffffffffffff),
      memory_order_acq_rel, memory_order_acquire));
  CLDB_ASSERT_EQ_UINT(UINT64_C(0x100000007), expected);
  CLDB_ASSERT_EQ_UINT(UINT64_C(0xffffffffffffffff),
                      cldb_atomic_u64_load(&value, memory_order_seq_cst));

  expected = 0;
  CLDB_ASSERT_FALSE(cldb_atomic_u64_compare_exchange(
      &value, &expected, 1, memory_order_acq_rel, memory_order_acquire));
  CLDB_ASSERT_EQ_UINT(UINT64_C(0xffffffffffffffff), expected);
}

/* 指针原子量覆盖 store/load/CAS 与 NULL。 */
CLDB_TEST(atomic_ptr_single_thread) {
  int first = 1;
  int second = 2;
  void *expected;
  cldb_atomic_ptr_t value;

  cldb_atomic_ptr_init(&value, &first);
  CLDB_ASSERT_EQ_PTR(&first,
                     cldb_atomic_ptr_load(&value, memory_order_relaxed));

  cldb_atomic_ptr_store(&value, &second, memory_order_release);
  CLDB_ASSERT_EQ_PTR(&second,
                     cldb_atomic_ptr_load(&value, memory_order_acquire));

  expected = &second;
  CLDB_ASSERT_TRUE(cldb_atomic_ptr_compare_exchange(
      &value, &expected, &first, memory_order_acq_rel, memory_order_acquire));
  CLDB_ASSERT_EQ_PTR(&second, expected);
  CLDB_ASSERT_EQ_PTR(&first,
                     cldb_atomic_ptr_load(&value, memory_order_seq_cst));

  expected = NULL;
  CLDB_ASSERT_FALSE(cldb_atomic_ptr_compare_exchange(
      &value, &expected, &second, memory_order_acq_rel,
      memory_order_acquire));
  CLDB_ASSERT_EQ_PTR(&first, expected);

  cldb_atomic_ptr_store(&value, NULL, memory_order_release);
  CLDB_ASSERT_EQ_PTR(NULL,
                     cldb_atomic_ptr_load(&value, memory_order_acquire));
}

enum {
  kCounterThreads = 4,
  kCounterIterations = 200000,
};

/* 多线程 relaxed fetch_add：只要总量精确，就说明 RMW 没有丢失更新。 */
static void *counter_thread(void *arg) {
  cldb_atomic_u64_t *counter = arg;
  uint32_t i;

  for (i = 0; i < (uint32_t)kCounterIterations; ++i) {
    cldb_atomic_u64_fetch_add(counter, 1, memory_order_relaxed);
  }
  return NULL;
}

CLDB_TEST(atomic_counter_multithread) {
  cldb_atomic_u64_t counter;
  pthread_t threads[kCounterThreads];
  size_t i;

  cldb_atomic_u64_init(&counter, 0);
  for (i = 0; i < kCounterThreads; ++i) {
    CLDB_ASSERT_EQ_INT(
        0, pthread_create(&threads[i], NULL, counter_thread, &counter));
  }
  for (i = 0; i < kCounterThreads; ++i) {
    CLDB_ASSERT_EQ_INT(0, pthread_join(threads[i], NULL));
  }

  CLDB_ASSERT_EQ_UINT((uint64_t)kCounterThreads * kCounterIterations,
                      cldb_atomic_u64_load(&counter, memory_order_relaxed));
}

enum { kPayloadWords = 8 };

typedef struct {
  uint32_t values[kPayloadWords];
  uint32_t sum;
} atomic_payload_t;

typedef struct {
  cldb_atomic_u32_t start;
  cldb_atomic_ptr_t ready;
  atomic_payload_t payload;
  uint32_t observed_sum;
  bool valid;
} atomic_pubsub_t;

/* 发布端：先写普通字段，再 release store ready。 */
static void *publisher_thread(void *arg) {
  atomic_pubsub_t *state = arg;
  uint32_t i;

  while (cldb_atomic_u32_load(&state->start, memory_order_acquire) == 0) {
  }
  state->payload.sum = 0;
  for (i = 0; i < (uint32_t)kPayloadWords; ++i) {
    state->payload.values[i] = i * 7 + 3;
    state->payload.sum += state->payload.values[i];
  }
  cldb_atomic_ptr_store(&state->ready, &state->payload,
                        memory_order_release);
  return NULL;
}

/* 订阅端：acquire load ready 后读取 payload，验证发布顺序。 */
static void *subscriber_thread(void *arg) {
  atomic_pubsub_t *state = arg;
  atomic_payload_t *payload;
  uint32_t sum = 0;
  uint32_t i;

  while ((payload = cldb_atomic_ptr_load(&state->ready,
                                          memory_order_acquire)) == NULL) {
  }
  for (i = 0; i < (uint32_t)kPayloadWords; ++i) {
    if (payload->values[i] != i * 7 + 3) {
      state->valid = false;
      return NULL;
    }
    sum += payload->values[i];
  }
  state->observed_sum = sum;
  state->valid = (sum == payload->sum);
  return NULL;
}

CLDB_TEST(atomic_publish_subscribe) {
  atomic_pubsub_t state;
  pthread_t publisher;
  pthread_t subscriber;
  uint32_t expected_sum = 0;
  uint32_t i;

  state.observed_sum = 0;
  state.valid = true;
  cldb_atomic_u32_init(&state.start, 0);
  cldb_atomic_ptr_init(&state.ready, NULL);
  for (i = 0; i < (uint32_t)kPayloadWords; ++i) {
    expected_sum += i * 7 + 3;
  }

  CLDB_ASSERT_EQ_INT(
      0, pthread_create(&subscriber, NULL, subscriber_thread, &state));
  CLDB_ASSERT_EQ_INT(
      0, pthread_create(&publisher, NULL, publisher_thread, &state));
  cldb_atomic_u32_store(&state.start, 1, memory_order_release);

  CLDB_ASSERT_EQ_INT(0, pthread_join(publisher, NULL));
  CLDB_ASSERT_EQ_INT(0, pthread_join(subscriber, NULL));
  CLDB_ASSERT_TRUE(state.valid);
  CLDB_ASSERT_EQ_UINT(expected_sum, state.observed_sum);
}

void cldb_register_atomic_tests(void) {
  CLDB_RUN_TEST(atomic_u32_single_thread);
  CLDB_RUN_TEST(atomic_u64_single_thread);
  CLDB_RUN_TEST(atomic_ptr_single_thread);
  CLDB_RUN_TEST(atomic_counter_multithread);
  CLDB_RUN_TEST(atomic_publish_subscribe);
}
