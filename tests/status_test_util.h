#ifndef CLDB_TESTS_STATUS_TEST_UTIL_H_
#define CLDB_TESTS_STATUS_TEST_UTIL_H_

#include <stdio.h>

#include "cleveldb/status.h"
#include "harness.h"

/* 状态码可读名；仅用于失败诊断，与 ToString 前缀不同（不带冒号）。
 * 对应上游 gtest 打印枚举名的方式。 */
static inline const char *cldb_test_status_name(cldb_status_code_t code) {
  switch (code) {
    case CLDB_STATUS_OK:
      return "OK";
    case CLDB_STATUS_NOT_FOUND:
      return "NotFound";
    case CLDB_STATUS_CORRUPTION:
      return "Corruption";
    case CLDB_STATUS_NOT_SUPPORTED:
      return "NotSupported";
    case CLDB_STATUS_INVALID_ARGUMENT:
      return "InvalidArgument";
    case CLDB_STATUS_IO_ERROR:
      return "IOError";
  }
  return "Unknown";
}

/* 状态断言核心：actual 按值传入只求值一次；失败时打印期望码和实际
 * 完整状态文本，对应上游 util/testutil.h 的 ASSERT_LEVELDB_OK
 * 与 Status::ToString() 对照的用法。 */
static inline void cldb_test_check_status(const char *file, int line,
                                          const char *expr,
                                          cldb_status_code_t expected,
                                          cldb_status_t actual, bool fatal) {
  char expected_text[64];
  char actual_text[512];

  if (cldb_status_code(actual) == expected) {
    return;
  }
  (void)snprintf(expected_text, sizeof(expected_text), "%s (%d)",
                 cldb_test_status_name(expected), (int)expected);
  (void)cldb_status_to_string(actual, actual_text, sizeof(actual_text));
  cldb_test_check_str(file, line, expr, expected_text, actual_text, fatal);
}

/* EXPECT/ASSERT 版本；OK 版本等价于期望 CLDB_STATUS_OK。 */
#define CLDB_CHECK_STATUS_(expected, actual, fatal)                       \
  cldb_test_check_status(__FILE__, __LINE__, #actual, (expected), (actual), \
                         (fatal))

#define CLDB_EXPECT_STATUS(expected, actual) \
  CLDB_CHECK_STATUS_(expected, actual, false)
#define CLDB_ASSERT_STATUS(expected, actual) \
  CLDB_CHECK_STATUS_(expected, actual, true)
#define CLDB_EXPECT_OK(actual) CLDB_CHECK_STATUS_(CLDB_STATUS_OK, actual, false)
#define CLDB_ASSERT_OK(actual) CLDB_CHECK_STATUS_(CLDB_STATUS_OK, actual, true)

#endif /* CLDB_TESTS_STATUS_TEST_UTIL_H_ */
