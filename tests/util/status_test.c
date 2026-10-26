#include <stddef.h>
#include <string.h>

#include "harness.h"
#include "status_test_util.h"

static cldb_status_t counted_status(int *count, cldb_status_code_t code) {
  ++*count;
  return cldb_status_set(code, "counted", NULL);
}

CLDB_TEST(status_ok) {
  char text[16];
  cldb_status_t status = cldb_status_ok();

  CLDB_EXPECT_STATUS(CLDB_STATUS_OK, status);
  CLDB_EXPECT_TRUE(cldb_status_is_ok(status));
  CLDB_EXPECT_FALSE(cldb_status_is_not_found(status));
  CLDB_EXPECT_EQ_INT(0, cldb_status_code(status));
  CLDB_EXPECT_EQ_STR("", cldb_status_message(&status));
  CLDB_EXPECT_EQ_UINT(2, cldb_status_to_string(status, text, sizeof(text)));
  CLDB_EXPECT_EQ_STR("OK", text);
}

CLDB_TEST(status_codes) {
  cldb_status_t status;

  status = cldb_status_not_found("get", "missing");
  CLDB_EXPECT_STATUS(CLDB_STATUS_NOT_FOUND, status);
  CLDB_EXPECT_TRUE(cldb_status_is_not_found(status));
  CLDB_EXPECT_FALSE(cldb_status_is_ok(status));

  status = cldb_status_corruption("block", "checksum");
  CLDB_EXPECT_STATUS(CLDB_STATUS_CORRUPTION, status);
  CLDB_EXPECT_TRUE(cldb_status_is_corruption(status));

  status = cldb_status_not_supported("feature", NULL);
  CLDB_EXPECT_STATUS(CLDB_STATUS_NOT_SUPPORTED, status);
  CLDB_EXPECT_TRUE(cldb_status_is_not_supported(status));

  status = cldb_status_invalid_argument("out", "NULL");
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT, status);
  CLDB_EXPECT_TRUE(cldb_status_is_invalid_argument(status));

  status = cldb_status_io("read", "EIO");
  CLDB_EXPECT_STATUS(CLDB_STATUS_IO_ERROR, status);
  CLDB_EXPECT_TRUE(cldb_status_is_io_error(status));
}

CLDB_TEST(status_message_format) {
  cldb_status_t status;

  status = cldb_status_not_found("ctx", "detail");
  CLDB_EXPECT_EQ_STR("ctx: detail", cldb_status_message(&status));

  status = cldb_status_not_found("ctx", NULL);
  CLDB_EXPECT_EQ_STR("ctx", cldb_status_message(&status));

  status = cldb_status_not_found(NULL, "detail");
  CLDB_EXPECT_EQ_STR("detail", cldb_status_message(&status));

  status = cldb_status_not_found(NULL, NULL);
  CLDB_EXPECT_EQ_STR("", cldb_status_message(&status));

  status = cldb_status_not_found("", "");
  CLDB_EXPECT_EQ_STR("", cldb_status_message(&status));

  status = cldb_status_set(CLDB_STATUS_OK, "ignored", "ignored");
  CLDB_EXPECT_EQ_STR("", cldb_status_message(&status));
  CLDB_EXPECT_EQ_STR("", cldb_status_message(NULL));
}

CLDB_TEST(status_message_truncated) {
  char long_text[400];
  cldb_status_t status;
  size_t length;

  memset(long_text, 'x', sizeof(long_text));
  long_text[sizeof(long_text) - 1] = '\0';

  status = cldb_status_corruption(long_text, NULL);
  length = strlen(cldb_status_message(&status));
  CLDB_EXPECT_EQ_UINT(sizeof(status.message) - 1, length);
  CLDB_EXPECT_EQ_INT('\0', status.message[length]);
}

CLDB_TEST(status_to_string_prefix) {
  char text[64];
  cldb_status_t status;

  status = cldb_status_not_found("get", "missing");
  CLDB_EXPECT_EQ_UINT(
      strlen("NotFound: get: missing"),
      cldb_status_to_string(status, text, sizeof(text)));
  CLDB_EXPECT_EQ_STR("NotFound: get: missing", text);

  status = cldb_status_corruption("block", NULL);
  CLDB_EXPECT_EQ_UINT(
      strlen("Corruption: block"),
      cldb_status_to_string(status, text, sizeof(text)));
  CLDB_EXPECT_EQ_STR("Corruption: block", text);

  status = cldb_status_not_supported("feature", NULL);
  CLDB_EXPECT_EQ_UINT(
      strlen("Not implemented: feature"),
      cldb_status_to_string(status, text, sizeof(text)));
  CLDB_EXPECT_EQ_STR("Not implemented: feature", text);

  status = cldb_status_invalid_argument("arg", NULL);
  CLDB_EXPECT_EQ_UINT(
      strlen("Invalid argument: arg"),
      cldb_status_to_string(status, text, sizeof(text)));
  CLDB_EXPECT_EQ_STR("Invalid argument: arg", text);

  status = cldb_status_io("read", NULL);
  CLDB_EXPECT_EQ_UINT(
      strlen("IO error: read"),
      cldb_status_to_string(status, text, sizeof(text)));
  CLDB_EXPECT_EQ_STR("IO error: read", text);

  status.code = (cldb_status_code_t)99;
  status.message[0] = '?';
  status.message[1] = '\0';
  CLDB_EXPECT_EQ_UINT(
      strlen("Unknown code(99): ?"),
      cldb_status_to_string(status, text, sizeof(text)));
  CLDB_EXPECT_EQ_STR("Unknown code(99): ?", text);

  status = cldb_status_ok();
  status.message[0] = 'x';
  status.message[1] = '\0';
  CLDB_EXPECT_EQ_UINT(2, cldb_status_to_string(status, text, sizeof(text)));
  CLDB_EXPECT_EQ_STR("OK", text);
}

CLDB_TEST(status_to_string_truncated) {
  char full[64];
  char small[8];
  char one[1];
  cldb_status_t status = cldb_status_not_found("context", "detail");
  size_t needed = cldb_status_to_string(status, full, sizeof(full));

  CLDB_EXPECT_EQ_UINT(needed,
                      cldb_status_to_string(status, small, sizeof(small)));
  CLDB_EXPECT_EQ_UINT(sizeof(small) - 1, strlen(small));
  CLDB_EXPECT_EQ_INT(0, strncmp(full, small, sizeof(small) - 1));
  CLDB_EXPECT_EQ_INT('\0', small[sizeof(small) - 1]);

  CLDB_EXPECT_EQ_UINT(needed,
                      cldb_status_to_string(status, one, sizeof(one)));
  CLDB_EXPECT_EQ_INT('\0', one[0]);

  CLDB_EXPECT_EQ_UINT(needed, cldb_status_to_string(status, NULL, 0));
}

CLDB_TEST(status_copy_independent) {
  cldb_status_t original = cldb_status_not_found("a", "b");
  cldb_status_t copy = original;

  original.message[0] = 'x';
  CLDB_EXPECT_EQ_STR("a: b", cldb_status_message(&copy));
  CLDB_EXPECT_EQ_STR("x: b", cldb_status_message(&original));
}

CLDB_TEST(status_assertions_evaluate_once) {
  int count = 0;

  CLDB_EXPECT_OK(counted_status(&count, CLDB_STATUS_OK));
  CLDB_EXPECT_EQ_INT(1, count);
  CLDB_EXPECT_STATUS(CLDB_STATUS_NOT_FOUND,
                     counted_status(&count, CLDB_STATUS_NOT_FOUND));
  CLDB_EXPECT_EQ_INT(2, count);
  CLDB_ASSERT_OK(cldb_status_ok());
  CLDB_EXPECT_EQ_INT(2, count);
}

void cldb_register_status_tests(void) {
  CLDB_RUN_TEST(status_ok);
  CLDB_RUN_TEST(status_codes);
  CLDB_RUN_TEST(status_message_format);
  CLDB_RUN_TEST(status_message_truncated);
  CLDB_RUN_TEST(status_to_string_prefix);
  CLDB_RUN_TEST(status_to_string_truncated);
  CLDB_RUN_TEST(status_copy_independent);
  CLDB_RUN_TEST(status_assertions_evaluate_once);
}
