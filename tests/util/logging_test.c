#include <stdint.h>
#include <string.h>

#include "harness.h"
#include "util/logging.h"

/* 数字格式化、转义和十进制解析；对应上游 util/logging_test.cc。 */

static void expect_number(uint64_t number, const char *expected) {
  cldb_buf_t text;

  cldb_buf_init(&text);
  CLDB_EXPECT_TRUE(cldb_number_to_string(&text, number));
  CLDB_EXPECT_EQ_STR(expected, cldb_buf_cstr(&text));
  cldb_buf_free(&text);
}

static void expect_escape(cldb_slice_t input, const char *expected) {
  cldb_buf_t text;
  cldb_buf_t appended;

  cldb_buf_init(&text);
  CLDB_EXPECT_TRUE(cldb_escape_string(&text, input));
  CLDB_EXPECT_EQ_STR(expected, cldb_buf_cstr(&text));
  cldb_buf_free(&text);

  cldb_buf_init(&appended);
  CLDB_EXPECT_TRUE(cldb_buf_append_char(&appended, 'x'));
  CLDB_EXPECT_TRUE(cldb_append_escaped_string_to(&appended, input));
  CLDB_EXPECT_EQ_STR(expected, cldb_buf_cstr(&appended) + 1);
  cldb_buf_free(&appended);
}

static void expect_digits_roundtrip(uint64_t number, const void *padding,
                                    size_t padding_size) {
  cldb_buf_t decimal;
  cldb_buf_t input;
  cldb_slice_t original;
  cldb_slice_t output;
  uint64_t parsed = 0;

  cldb_buf_init(&decimal);
  cldb_buf_init(&input);
  CLDB_EXPECT_TRUE(cldb_number_to_string(&decimal, number));
  CLDB_EXPECT_TRUE(cldb_buf_append(&input, decimal.data, decimal.size));
  if (padding_size > 0) {
    CLDB_EXPECT_TRUE(cldb_buf_append(&input, padding, padding_size));
  }
  original = cldb_buf_slice(&input);
  output = original;
  CLDB_EXPECT_TRUE(cldb_consume_decimal_number(&output, &parsed));
  CLDB_EXPECT_EQ_UINT(number, parsed);
  if (padding_size > 0) {
    CLDB_EXPECT_EQ_UINT(decimal.size,
                        (size_t)(output.data - original.data));
  } else {
    /* remove_prefix 消耗到末尾时规范化为静态空切片。 */
    CLDB_EXPECT_EQ_STR("", output.data);
  }
  CLDB_EXPECT_EQ_UINT(padding_size, output.size);
  cldb_buf_free(&decimal);
  cldb_buf_free(&input);
}

static void expect_no_digits(const void *data, size_t size) {
  cldb_slice_t input = cldb_slice_make(data, size);
  cldb_slice_t output = input;
  uint64_t parsed = 123456789;

  CLDB_EXPECT_FALSE(cldb_consume_decimal_number(&output, &parsed));
  CLDB_EXPECT_EQ_UINT(123456789, parsed);
  CLDB_EXPECT_EQ_PTR(input.data, output.data);
  CLDB_EXPECT_EQ_UINT(input.size, output.size);
}

static void expect_overflow(const char *text) {
  cldb_slice_t input = cldb_slice_make(text, strlen(text));
  cldb_slice_t output = input;
  uint64_t parsed = 123456789;

  CLDB_EXPECT_FALSE(cldb_consume_decimal_number(&output, &parsed));
  CLDB_EXPECT_EQ_UINT(123456789, parsed);
  CLDB_EXPECT_EQ_PTR(input.data, output.data);
  CLDB_EXPECT_EQ_UINT(input.size, output.size);
}

CLDB_TEST(logging_number_to_string) {
  expect_number(0, "0");
  expect_number(1, "1");
  expect_number(9, "9");
  expect_number(10, "10");
  expect_number(11, "11");
  expect_number(19, "19");
  expect_number(99, "99");
  expect_number(100, "100");
  expect_number(109, "109");
  expect_number(190, "190");
  expect_number(123, "123");
  expect_number(12345678, "12345678");
  expect_number(UINT64_C(18446744073709551000),
                "18446744073709551000");
  expect_number(UINT64_C(18446744073709551600),
                "18446744073709551600");
  expect_number(UINT64_C(18446744073709551610),
                "18446744073709551610");
  expect_number(UINT64_MAX, "18446744073709551615");
}

CLDB_TEST(logging_append_number) {
  cldb_buf_t text;

  cldb_buf_init(&text);
  CLDB_EXPECT_TRUE(cldb_buf_append(&text, "old", 3));
  CLDB_EXPECT_TRUE(cldb_append_number_to(&text, 123));
  CLDB_EXPECT_EQ_STR("old123", cldb_buf_cstr(&text));
  CLDB_EXPECT_TRUE(cldb_number_to_string(&text, 7));
  CLDB_EXPECT_EQ_STR("7", cldb_buf_cstr(&text));
  CLDB_EXPECT_TRUE(cldb_number_to_string(&text, UINT64_MAX));
  CLDB_EXPECT_EQ_STR("18446744073709551615", cldb_buf_cstr(&text));
  CLDB_EXPECT_FALSE(cldb_append_number_to(NULL, 1));
  CLDB_EXPECT_FALSE(cldb_number_to_string(NULL, 1));
  cldb_buf_free(&text);
}

CLDB_TEST(logging_escape_string) {
  static const char kBinary[] = {'f', 'o', 'o', '\0', 'b', 'a', 'r',
                                 (char)0xff};
  static const char kControls[] = {'\0', '\1', '\t', '\n', '\x1f',
                                   '\x7f', (char)0x80, (char)0xff};
  static const char kPrintable[] =
      " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ"
      "[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~";
  cldb_buf_t text;
  cldb_slice_t invalid;
  cldb_slice_t empty = cldb_slice_make(NULL, 0);

  expect_escape(empty, "");
  expect_escape(cldb_slice_make("foo", 3), "foo");
  expect_escape(cldb_slice_make(kPrintable, sizeof(kPrintable) - 1),
                kPrintable);
  expect_escape(cldb_slice_make("\0", 1), "\\x00");
  expect_escape(cldb_slice_make("\t", 1), "\\x09");
  expect_escape(cldb_slice_make("\n", 1), "\\x0a");
  expect_escape(cldb_slice_make("\x1f", 1), "\\x1f");
  expect_escape(cldb_slice_make("\x7f", 1), "\\x7f");
  expect_escape(cldb_slice_make("\x80", 1), "\\x80");
  expect_escape(cldb_slice_make("\xff", 1), "\\xff");
  expect_escape(cldb_slice_make(kControls, sizeof(kControls)),
                "\\x00\\x01\\x09\\x0a\\x1f\\x7f\\x80\\xff");
  expect_escape(cldb_slice_make(kBinary, sizeof(kBinary)),
                "foo\\x00bar\\xff");

  cldb_buf_init(&text);
  CLDB_EXPECT_TRUE(cldb_buf_append(&text, "old", 3));
  CLDB_EXPECT_TRUE(cldb_append_escaped_string_to(&text, empty));
  CLDB_EXPECT_EQ_STR("old", cldb_buf_cstr(&text));

  invalid.data = NULL;
  invalid.size = 1;
  CLDB_EXPECT_FALSE(cldb_append_escaped_string_to(&text, invalid));
  CLDB_EXPECT_EQ_STR("old", cldb_buf_cstr(&text));
  CLDB_EXPECT_FALSE(cldb_escape_string(&text, invalid));
  CLDB_EXPECT_EQ_STR("old", cldb_buf_cstr(&text));
  CLDB_EXPECT_FALSE(cldb_append_escaped_string_to(NULL, empty));
  CLDB_EXPECT_FALSE(cldb_escape_string(NULL, empty));
  cldb_buf_free(&text);
}

CLDB_TEST(logging_consume_decimal_roundtrip) {
  static const char kZeros[3] = {'\0', '\0', '\0'};
  uint64_t i;

  expect_digits_roundtrip(0, NULL, 0);
  expect_digits_roundtrip(1, NULL, 0);
  expect_digits_roundtrip(9, NULL, 0);
  expect_digits_roundtrip(10, NULL, 0);
  expect_digits_roundtrip(11, NULL, 0);
  expect_digits_roundtrip(19, NULL, 0);
  expect_digits_roundtrip(99, NULL, 0);
  expect_digits_roundtrip(100, NULL, 0);
  expect_digits_roundtrip(109, NULL, 0);
  expect_digits_roundtrip(190, NULL, 0);
  expect_digits_roundtrip(123, NULL, 0);
  expect_digits_roundtrip(UINT64_MAX, NULL, 0);

  expect_digits_roundtrip(0, " ", 1);
  expect_digits_roundtrip(1, "abc", 3);
  expect_digits_roundtrip(9, "x", 1);
  expect_digits_roundtrip(10, "_", 1);
  expect_digits_roundtrip(11, kZeros, sizeof(kZeros));
  expect_digits_roundtrip(19, "abc", 3);
  expect_digits_roundtrip(99, "padding", 7);
  expect_digits_roundtrip(100, " ", 1);

  for (i = 0; i < 100; ++i) {
    expect_digits_roundtrip(UINT64_MAX - i, "pad", 3);
  }
}

CLDB_TEST(logging_consume_decimal_overflow) {
  expect_overflow("18446744073709551616");
  expect_overflow("18446744073709551617");
  expect_overflow("18446744073709551618");
  expect_overflow("18446744073709551619");
  expect_overflow("18446744073709551620");
  expect_overflow("18446744073709551621");
  expect_overflow("18446744073709551622");
  expect_overflow("18446744073709551623");
  expect_overflow("18446744073709551624");
  expect_overflow("18446744073709551625");
  expect_overflow("18446744073709551626");
  expect_overflow("18446744073709551700");
  expect_overflow("99999999999999999999");
}

CLDB_TEST(logging_consume_decimal_no_digits) {
  static const char kNul[] = {'\0', '1', '2', '3'};
  static const char kDel[] = {'\x7f', '1', '2', '3'};
  static const char kHigh[] = {(char)0xff, '1', '2', '3'};
  cldb_slice_t input = cldb_slice_empty();
  uint64_t parsed = 0;

  expect_no_digits("", 0);
  expect_no_digits(" ", 1);
  expect_no_digits("a", 1);
  expect_no_digits(" 123", 4);
  expect_no_digits("a123", 4);
  expect_no_digits(kNul, sizeof(kNul));
  expect_no_digits(kDel, sizeof(kDel));
  expect_no_digits(kHigh, sizeof(kHigh));

  parsed = 42;
  CLDB_EXPECT_FALSE(cldb_consume_decimal_number(NULL, &parsed));
  CLDB_EXPECT_EQ_UINT(42, parsed);
  CLDB_EXPECT_FALSE(cldb_consume_decimal_number(&input, NULL));
  CLDB_EXPECT_FALSE(cldb_consume_decimal_number(&input, &parsed));
  CLDB_EXPECT_EQ_UINT(42, parsed);

  input = cldb_slice_make("123", 3);
  CLDB_EXPECT_FALSE(cldb_consume_decimal_number(NULL, &parsed));
  CLDB_EXPECT_EQ_UINT(3, input.size);
  CLDB_EXPECT_FALSE(cldb_consume_decimal_number(&input, NULL));
  CLDB_EXPECT_EQ_UINT(3, input.size);
  CLDB_EXPECT_EQ_INT(0, memcmp(input.data, "123", 3));
}

void cldb_register_logging_tests(void) {
  CLDB_RUN_TEST(logging_number_to_string);
  CLDB_RUN_TEST(logging_append_number);
  CLDB_RUN_TEST(logging_escape_string);
  CLDB_RUN_TEST(logging_consume_decimal_roundtrip);
  CLDB_RUN_TEST(logging_consume_decimal_overflow);
  CLDB_RUN_TEST(logging_consume_decimal_no_digits);
}
