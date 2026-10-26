#include <stddef.h>
#include <stdint.h>

#include "harness.h"
#include "status_test_util.h"
#include "util/example.h"

CLDB_TEST(example_create_and_destroy) {
  cldb_example_t *example = NULL;

  CLDB_ASSERT_OK(cldb_example_create("alpha", &example));
  CLDB_ASSERT_TRUE(example != NULL);
  CLDB_EXPECT_EQ_STR("alpha", cldb_example_name(example));

  cldb_example_destroy(example);
  cldb_example_destroy(NULL);
}

CLDB_TEST(example_empty_name) {
  cldb_example_t *example = NULL;

  CLDB_ASSERT_OK(cldb_example_create("", &example));
  CLDB_EXPECT_EQ_STR("", cldb_example_name(example));
  cldb_example_destroy(example);
}

CLDB_TEST(example_invalid_arguments) {
  int anchor = 0;
  cldb_example_t *example = (cldb_example_t *)&anchor;
  cldb_example_t *valid = NULL;

  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_example_create(NULL, &example));
  CLDB_EXPECT_EQ_PTR(NULL, example);
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_example_create("x", NULL));
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_example_set_name(NULL, "x"));
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_example_add(NULL, 1, NULL));
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_example_get_value(NULL, NULL));
  CLDB_EXPECT_EQ_PTR(NULL, cldb_example_name(NULL));

  CLDB_ASSERT_OK(cldb_example_create("x", &valid));
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_example_set_name(valid, NULL));
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_example_get_value(valid, NULL));
  cldb_example_destroy(valid);
}

CLDB_TEST(example_set_name) {
  cldb_example_t *example = NULL;

  CLDB_ASSERT_OK(cldb_example_create("old", &example));
  CLDB_EXPECT_OK(cldb_example_set_name(example, "new"));
  CLDB_EXPECT_EQ_STR("new", cldb_example_name(example));
  cldb_example_destroy(example);
}

CLDB_TEST(example_add_and_get) {
  cldb_example_t *example = NULL;
  int64_t value = 0;

  CLDB_ASSERT_OK(cldb_example_create("n", &example));
  CLDB_EXPECT_OK(cldb_example_add(example, 7, &value));
  CLDB_EXPECT_EQ_INT(7, value);
  CLDB_EXPECT_OK(cldb_example_add(example, -10, NULL));
  CLDB_EXPECT_OK(cldb_example_get_value(example, &value));
  CLDB_EXPECT_EQ_INT(-3, value);
  cldb_example_destroy(example);
}

CLDB_TEST(example_add_overflow) {
  cldb_example_t *example = NULL;
  int64_t value = 0;

  CLDB_ASSERT_OK(cldb_example_create("n", &example));
  CLDB_EXPECT_OK(cldb_example_add(example, INT64_MAX, &value));
  CLDB_EXPECT_EQ_INT(INT64_MAX, value);
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_example_add(example, 1, &value));
  CLDB_EXPECT_OK(cldb_example_get_value(example, &value));
  CLDB_EXPECT_EQ_INT(INT64_MAX, value);
  cldb_example_destroy(example);

  example = NULL;
  CLDB_ASSERT_OK(cldb_example_create("n", &example));
  CLDB_EXPECT_OK(cldb_example_add(example, INT64_MIN, &value));
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_example_add(example, -1, &value));
  CLDB_EXPECT_OK(cldb_example_get_value(example, &value));
  CLDB_EXPECT_EQ_INT(INT64_MIN, value);
  cldb_example_destroy(example);
}

CLDB_TEST(example_error_message) {
  cldb_example_t *example = NULL;
  cldb_status_t status = cldb_example_create(NULL, &example);

  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT, status);
  CLDB_EXPECT_TRUE(cldb_status_message(&status)[0] != '\0');
  CLDB_EXPECT_EQ_PTR(NULL, example);
}

void cldb_register_example_tests(void) {
  CLDB_RUN_TEST(example_create_and_destroy);
  CLDB_RUN_TEST(example_empty_name);
  CLDB_RUN_TEST(example_invalid_arguments);
  CLDB_RUN_TEST(example_set_name);
  CLDB_RUN_TEST(example_add_and_get);
  CLDB_RUN_TEST(example_add_overflow);
  CLDB_RUN_TEST(example_error_message);
}
