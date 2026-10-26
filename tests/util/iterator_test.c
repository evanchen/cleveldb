#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "cleveldb/iterator.h"
#include "harness.h"
#include "status_test_util.h"

enum { kCleanupCapacity = 8 };

typedef struct {
  int ids[kCleanupCapacity];
  size_t count;
} cleanup_log_t;

typedef struct {
  int id;
} cleanup_item_t;

/* 记录回调参数；arg1 是日志，arg2 是带编号的条目。 */
static void record_cleanup(void *arg1, void *arg2) {
  cleanup_log_t *log = arg1;
  const cleanup_item_t *item = arg2;

  CLDB_ASSERT_TRUE(log->count < kCleanupCapacity);
  log->ids[log->count++] = item->id;
}

/* 按注册顺序登记 count 个回调，销毁后核对调用次数和实际顺序。 */
static void expect_cleanup_order(const int *registered, const int *expected,
                                 size_t count) {
  cldb_iterator_t *iterator = cldb_new_empty_iterator();
  cleanup_log_t log = {0};
  cleanup_item_t items[kCleanupCapacity];
  size_t i;

  CLDB_ASSERT_TRUE(iterator != NULL);
  CLDB_ASSERT_TRUE(count <= kCleanupCapacity);
  for (i = 0; i < count; ++i) {
    items[i].id = registered[i];
    CLDB_ASSERT_OK(cldb_iter_register_cleanup(iterator, record_cleanup, &log,
                                              &items[i]));
  }

  CLDB_EXPECT_EQ_UINT(0, log.count);
  cldb_iter_destroy(iterator);
  CLDB_EXPECT_EQ_UINT(count, log.count);
  for (i = 0; i < count && i < log.count; ++i) {
    CLDB_EXPECT_EQ_INT(expected[i], log.ids[i]);
  }
}

CLDB_TEST(iterator_empty_lifecycle) {
  cldb_iterator_t *iterator = cldb_new_empty_iterator();

  CLDB_ASSERT_TRUE(iterator != NULL);
  CLDB_EXPECT_FALSE(cldb_iter_valid(iterator));
  CLDB_EXPECT_OK(cldb_iter_status(iterator));

  cldb_iter_seek_to_first(iterator);
  CLDB_EXPECT_FALSE(cldb_iter_valid(iterator));
  CLDB_EXPECT_OK(cldb_iter_status(iterator));

  cldb_iter_seek_to_last(iterator);
  CLDB_EXPECT_FALSE(cldb_iter_valid(iterator));
  CLDB_EXPECT_OK(cldb_iter_status(iterator));

  cldb_iter_seek(iterator, cldb_slice_make("target", 6));
  CLDB_EXPECT_FALSE(cldb_iter_valid(iterator));
  CLDB_EXPECT_OK(cldb_iter_status(iterator));

  cldb_iter_destroy(iterator);
}

CLDB_TEST(iterator_error_status) {
  cldb_status_t error = cldb_status_corruption("iterator", "corrupt block");
  cldb_iterator_t *iterator = cldb_new_error_iterator(error);
  cldb_status_t status;

  CLDB_ASSERT_TRUE(iterator != NULL);
  CLDB_EXPECT_FALSE(cldb_iter_valid(iterator));
  status = cldb_iter_status(iterator);
  CLDB_EXPECT_STATUS(CLDB_STATUS_CORRUPTION, status);
  CLDB_EXPECT_TRUE(strstr(cldb_status_message(&status), "corrupt block") !=
                   NULL);

  /* seek 对错误迭代器为空操作，状态保持不变。 */
  cldb_iter_seek_to_first(iterator);
  cldb_iter_seek_to_last(iterator);
  cldb_iter_seek(iterator, cldb_slice_make("target", 6));
  CLDB_EXPECT_FALSE(cldb_iter_valid(iterator));
  status = cldb_iter_status(iterator);
  CLDB_EXPECT_STATUS(CLDB_STATUS_CORRUPTION, status);
  CLDB_EXPECT_TRUE(strstr(cldb_status_message(&status), "corrupt block") !=
                   NULL);

  cldb_iter_destroy(iterator);
}

CLDB_TEST(iterator_cleanup_order) {
  static const int kAb[] = {1, 2};
  static const int kBa[] = {2, 1};
  static const int kAbc[] = {1, 2, 3};
  static const int kExpectedAbc[] = {1, 3, 2};

  /* 两个回调保持注册顺序；三个回调复刻上游头节点后插的链表：
   * A、B、C 登记后实际执行 A、C、B。 */
  expect_cleanup_order(kAb, kAb, 2);
  expect_cleanup_order(kBa, kBa, 2);
  expect_cleanup_order(kAbc, kExpectedAbc, 3);
}

typedef struct {
  int calls;
} free_counter_t;

static void free_counted(void *arg1, void *arg2) {
  free_counter_t *counter = arg2;

  free(arg1);
  ++counter->calls;
}

CLDB_TEST(iterator_cleanup_releases_resource) {
  cldb_iterator_t *iterator = cldb_new_empty_iterator();
  free_counter_t counter = {0};
  int *block = malloc(64);

  CLDB_ASSERT_TRUE(iterator != NULL);
  CLDB_ASSERT_TRUE(block != NULL);
  CLDB_ASSERT_OK(cldb_iter_register_cleanup(iterator, free_counted, block,
                                            &counter));

  CLDB_EXPECT_EQ_INT(0, counter.calls);
  cldb_iter_destroy(iterator);
  CLDB_EXPECT_EQ_INT(1, counter.calls);
}

CLDB_TEST(iterator_cleanup_invalid_arguments) {
  cldb_iterator_t uninitialized = {0};
  cldb_iterator_t *iterator = cldb_new_empty_iterator();
  cleanup_log_t log = {0};

  CLDB_EXPECT_STATUS(
      CLDB_STATUS_INVALID_ARGUMENT,
      cldb_iter_register_cleanup(NULL, record_cleanup, &log, NULL));

  CLDB_ASSERT_TRUE(iterator != NULL);
  CLDB_EXPECT_STATUS(
      CLDB_STATUS_INVALID_ARGUMENT,
      cldb_iter_register_cleanup(iterator, NULL, &log, NULL));
  CLDB_EXPECT_STATUS(
      CLDB_STATUS_INVALID_ARGUMENT,
      cldb_iter_register_cleanup(&uninitialized, record_cleanup, &log, NULL));

  cldb_iter_run_cleanup(NULL);
  cldb_iter_run_cleanup(iterator);
  cldb_iter_destroy(iterator);
  cldb_iter_destroy(NULL);
}

void cldb_register_iterator_tests(void) {
  CLDB_RUN_TEST(iterator_empty_lifecycle);
  CLDB_RUN_TEST(iterator_error_status);
  CLDB_RUN_TEST(iterator_cleanup_order);
  CLDB_RUN_TEST(iterator_cleanup_releases_resource);
  CLDB_RUN_TEST(iterator_cleanup_invalid_arguments);
}
