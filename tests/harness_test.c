#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "port/platform.h"

CLDB_TEST(harness_expect_pass) {
  static int anchor;
  int value = 0;

  CLDB_EXPECT_TRUE(true);
  CLDB_EXPECT_FALSE(false);
  CLDB_EXPECT_EQ_INT(-7, -7);
  CLDB_EXPECT_EQ_UINT(0, 0);
  CLDB_EXPECT_EQ_PTR(&anchor, &anchor);
  CLDB_EXPECT_EQ_PTR(NULL, NULL);
  CLDB_EXPECT_EQ_STR("abc", "abc");
  CLDB_EXPECT_EQ_STR(NULL, NULL);

  /* 实际值只求值一次。 */
  CLDB_EXPECT_EQ_INT(1, ++value);
  CLDB_EXPECT_EQ_INT(1, value);
}

CLDB_TEST(platform_name_is_linux) {
  CLDB_ASSERT_EQ_STR("linux", cldb_platform_name());
}

CLDB_TEST(harness_filter_target) { CLDB_EXPECT_TRUE(true); }

CLDB_TEST(harness_filter_other) { CLDB_EXPECT_TRUE(true); }

/* 默认通过；设置 CLDB_TEST_ENABLE_FAILURE=1 时故意失败。 */
CLDB_TEST(harness_failure_example) {
  const char *enabled = getenv("CLDB_TEST_ENABLE_FAILURE");

  if (enabled != NULL && strcmp(enabled, "1") == 0) {
    CLDB_EXPECT_EQ_INT(1, 2); /* EXPECT 后继续 */
    CLDB_ASSERT_EQ_INT(3, 4); /* ASSERT 终止当前测试 */
    CLDB_EXPECT_TRUE(false);  /* 不可达 */
  }
  CLDB_EXPECT_TRUE(true);
}

void cldb_register_harness_tests(void) {
  CLDB_RUN_TEST(harness_expect_pass);
  CLDB_RUN_TEST(platform_name_is_linux);
  CLDB_RUN_TEST(harness_filter_target);
  CLDB_RUN_TEST(harness_filter_other);
  CLDB_RUN_TEST(harness_failure_example);
}
