#ifndef CLDB_TESTS_HARNESS_H_
#define CLDB_TESTS_HARNESS_H_

#include <stdbool.h>
#include <stdint.h>

typedef void (*cldb_test_fn_t)(void);

/* 定义并注册测试；名字必须是合法 C 标识符。 */
#define CLDB_TEST(name) static void cldb_test_##name(void)
#define CLDB_RUN_TEST(name) cldb_test_register(#name, cldb_test_##name)

void cldb_test_register(const char *name, cldb_test_fn_t fn);
int cldb_test_run(int argc, char **argv);

/* 断言实现：EXPECT 继续，ASSERT 终止当前测试；仅限测试主线程。
 * ASSERT 走 longjmp，测试内已分配资源需要自行清理或改用 EXPECT。 */
void cldb_test_check_bool(const char *file, int line, const char *expr,
                          bool expected, bool actual, bool fatal);
void cldb_test_check_int(const char *file, int line, const char *expr,
                         int64_t expected, int64_t actual, bool fatal);
void cldb_test_check_uint(const char *file, int line, const char *expr,
                          uint64_t expected, uint64_t actual, bool fatal);
void cldb_test_check_ptr(const char *file, int line, const char *expr,
                         const void *expected, const void *actual, bool fatal);
void cldb_test_check_str(const char *file, int line, const char *expr,
                         const char *expected, const char *actual, bool fatal);

#define CLDB_CHECK_BOOL_(cond, expected, fatal)                           \
  cldb_test_check_bool(__FILE__, __LINE__, #cond, (expected), (cond), (fatal))

#define CLDB_EXPECT_TRUE(cond) CLDB_CHECK_BOOL_(cond, true, false)
#define CLDB_ASSERT_TRUE(cond) CLDB_CHECK_BOOL_(cond, true, true)
#define CLDB_EXPECT_FALSE(cond) CLDB_CHECK_BOOL_(cond, false, false)
#define CLDB_ASSERT_FALSE(cond) CLDB_CHECK_BOOL_(cond, false, true)

#define CLDB_CHECK_EQ_INT_(expected, actual, fatal)                       \
  cldb_test_check_int(__FILE__, __LINE__, #expected " == " #actual,       \
                      (int64_t)(expected), (int64_t)(actual), (fatal))

#define CLDB_EXPECT_EQ_INT(expected, actual) \
  CLDB_CHECK_EQ_INT_(expected, actual, false)
#define CLDB_ASSERT_EQ_INT(expected, actual) \
  CLDB_CHECK_EQ_INT_(expected, actual, true)

#define CLDB_CHECK_EQ_UINT_(expected, actual, fatal)                      \
  cldb_test_check_uint(__FILE__, __LINE__, #expected " == " #actual,      \
                       (uint64_t)(expected), (uint64_t)(actual), (fatal))

#define CLDB_EXPECT_EQ_UINT(expected, actual) \
  CLDB_CHECK_EQ_UINT_(expected, actual, false)
#define CLDB_ASSERT_EQ_UINT(expected, actual) \
  CLDB_CHECK_EQ_UINT_(expected, actual, true)

#define CLDB_CHECK_EQ_PTR_(expected, actual, fatal)                       \
  cldb_test_check_ptr(__FILE__, __LINE__, #expected " == " #actual,       \
                      (const void *)(expected), (const void *)(actual),   \
                      (fatal))

#define CLDB_EXPECT_EQ_PTR(expected, actual) \
  CLDB_CHECK_EQ_PTR_(expected, actual, false)
#define CLDB_ASSERT_EQ_PTR(expected, actual) \
  CLDB_CHECK_EQ_PTR_(expected, actual, true)

#define CLDB_CHECK_EQ_STR_(expected, actual, fatal)                       \
  cldb_test_check_str(__FILE__, __LINE__, #expected " == " #actual,       \
                      (const char *)(expected), (const char *)(actual),   \
                      (fatal))

#define CLDB_EXPECT_EQ_STR(expected, actual) \
  CLDB_CHECK_EQ_STR_(expected, actual, false)
#define CLDB_ASSERT_EQ_STR(expected, actual) \
  CLDB_CHECK_EQ_STR_(expected, actual, true)

/* OK/STATUS 断言依赖 cldb_status_t，见 tests/status_test_util.h。 */

#endif /* CLDB_TESTS_HARNESS_H_ */
