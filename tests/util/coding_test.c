#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "harness.h"
#include "util/buf.h"
#include "util/coding.h"

/* 校验缓冲内容；size 为 0 时允许 data 为 NULL。 */
static void expect_bytes(const cldb_buf_t *buf, const uint8_t *expected,
                         size_t size) {
  CLDB_ASSERT_EQ_UINT(size, buf->size);
  if (size == 0) {
    return;
  }
  CLDB_ASSERT_TRUE(buf->data != NULL);
  CLDB_ASSERT_EQ_INT(0, memcmp(buf->data, expected, size));
}

/* varint32 各入口的往返一致性。 */
static void check_varint32(uint32_t value) {
  char encoded[5];
  const char *end;
  size_t encoded_size;
  cldb_buf_t buf;
  cldb_slice_t input;
  uint32_t decoded;

  end = cldb_encode_varint32(encoded, value);
  encoded_size = (size_t)(end - encoded);
  CLDB_ASSERT_EQ_UINT(encoded_size, cldb_varint_length(value));
  CLDB_ASSERT_TRUE(encoded_size >= 1 && encoded_size <= 5);

  cldb_buf_init(&buf);
  CLDB_ASSERT_TRUE(cldb_put_varint32(&buf, value));
  CLDB_ASSERT_EQ_UINT(encoded_size, buf.size);
  CLDB_ASSERT_EQ_INT(0, memcmp(encoded, buf.data, encoded_size));

  decoded = 0;
  end = cldb_get_varint32_ptr(buf.data, buf.data + buf.size, &decoded);
  CLDB_ASSERT_TRUE(end != NULL);
  CLDB_ASSERT_EQ_UINT(value, decoded);
  CLDB_ASSERT_EQ_UINT(buf.size, (size_t)(end - buf.data));

  input = cldb_buf_slice(&buf);
  decoded = 0;
  CLDB_ASSERT_TRUE(cldb_get_varint32(&input, &decoded));
  CLDB_ASSERT_EQ_UINT(value, decoded);
  CLDB_ASSERT_EQ_UINT(0, input.size);

  cldb_buf_free(&buf);
}

/* 校验 varint32 编码字节和往返。 */
static void expect_varint32(uint32_t value, const uint8_t *expected,
                            size_t expected_size) {
  char encoded[5];
  char *end = cldb_encode_varint32(encoded, value);

  CLDB_ASSERT_EQ_UINT(expected_size, (size_t)(end - encoded));
  CLDB_ASSERT_EQ_INT(0, memcmp(encoded, expected, expected_size));
  check_varint32(value);
}

/* varint64 各入口的往返一致性。 */
static void check_varint64(uint64_t value) {
  char encoded[10];
  const char *end;
  size_t encoded_size;
  cldb_buf_t buf;
  cldb_slice_t input;
  uint64_t decoded;

  end = cldb_encode_varint64(encoded, value);
  encoded_size = (size_t)(end - encoded);
  CLDB_ASSERT_EQ_UINT(encoded_size, cldb_varint_length(value));
  CLDB_ASSERT_TRUE(encoded_size >= 1 && encoded_size <= 10);

  cldb_buf_init(&buf);
  CLDB_ASSERT_TRUE(cldb_put_varint64(&buf, value));
  CLDB_ASSERT_EQ_UINT(encoded_size, buf.size);
  CLDB_ASSERT_EQ_INT(0, memcmp(encoded, buf.data, encoded_size));

  decoded = 0;
  end = cldb_get_varint64_ptr(buf.data, buf.data + buf.size, &decoded);
  CLDB_ASSERT_TRUE(end != NULL);
  CLDB_ASSERT_EQ_UINT(value, decoded);
  CLDB_ASSERT_EQ_UINT(buf.size, (size_t)(end - buf.data));

  input = cldb_buf_slice(&buf);
  decoded = 0;
  CLDB_ASSERT_TRUE(cldb_get_varint64(&input, &decoded));
  CLDB_ASSERT_EQ_UINT(value, decoded);
  CLDB_ASSERT_EQ_UINT(0, input.size);

  cldb_buf_free(&buf);
}

/* 校验 varint64 编码字节和往返。 */
static void expect_varint64(uint64_t value, const uint8_t *expected,
                            size_t expected_size) {
  char encoded[10];
  char *end = cldb_encode_varint64(encoded, value);

  CLDB_ASSERT_EQ_UINT(expected_size, (size_t)(end - encoded));
  CLDB_ASSERT_EQ_INT(0, memcmp(encoded, expected, expected_size));
  check_varint64(value);
}

CLDB_TEST(coding_fixed32_roundtrip) {
  cldb_buf_t buf;
  const char *p;
  uint32_t value;

  cldb_buf_init(&buf);
  for (value = 0; value < 100000; ++value) {
    CLDB_ASSERT_TRUE(cldb_put_fixed32(&buf, value));
  }
  CLDB_ASSERT_EQ_UINT(100000 * 4, buf.size);

  p = buf.data;
  for (value = 0; value < 100000; ++value) {
    CLDB_ASSERT_EQ_UINT(value, cldb_decode_fixed32(p));
    p += 4;
  }
  cldb_buf_free(&buf);
}

CLDB_TEST(coding_fixed64_roundtrip) {
  cldb_buf_t buf;
  const char *p;
  int power;

  cldb_buf_init(&buf);
  for (power = 0; power <= 63; ++power) {
    uint64_t value = UINT64_C(1) << power;

    CLDB_ASSERT_TRUE(cldb_put_fixed64(&buf, value - 1));
    CLDB_ASSERT_TRUE(cldb_put_fixed64(&buf, value));
    CLDB_ASSERT_TRUE(cldb_put_fixed64(&buf, value + 1));
  }
  CLDB_ASSERT_EQ_UINT(64 * 3 * 8, buf.size);

  p = buf.data;
  for (power = 0; power <= 63; ++power) {
    uint64_t value = UINT64_C(1) << power;

    CLDB_ASSERT_EQ_UINT(value - 1, cldb_decode_fixed64(p));
    p += 8;
    CLDB_ASSERT_EQ_UINT(value, cldb_decode_fixed64(p));
    p += 8;
    CLDB_ASSERT_EQ_UINT(value + 1, cldb_decode_fixed64(p));
    p += 8;
  }
  cldb_buf_free(&buf);
}

CLDB_TEST(coding_fixed_little_endian) {
  static const uint8_t kFixed32[] = {0x01, 0x02, 0x03, 0x04};
  static const uint8_t kFixed64[] = {0x01, 0x02, 0x03, 0x04,
                                     0x05, 0x06, 0x07, 0x08};
  char bytes[8];
  cldb_buf_t buf;

  cldb_encode_fixed32(bytes, UINT32_C(0x04030201));
  CLDB_ASSERT_EQ_INT(0, memcmp(bytes, kFixed32, sizeof(kFixed32)));
  CLDB_EXPECT_EQ_UINT(UINT32_C(0x04030201), cldb_decode_fixed32(bytes));

  cldb_encode_fixed64(bytes, UINT64_C(0x0807060504030201));
  CLDB_ASSERT_EQ_INT(0, memcmp(bytes, kFixed64, sizeof(kFixed64)));
  CLDB_EXPECT_EQ_UINT(UINT64_C(0x0807060504030201),
                      cldb_decode_fixed64(bytes));

  cldb_buf_init(&buf);
  CLDB_ASSERT_TRUE(cldb_put_fixed32(&buf, UINT32_C(0x04030201)));
  CLDB_ASSERT_TRUE(cldb_put_fixed64(&buf, UINT64_C(0x0807060504030201)));
  CLDB_ASSERT_EQ_UINT(sizeof(kFixed32) + sizeof(kFixed64), buf.size);
  CLDB_ASSERT_EQ_INT(0, memcmp(buf.data, kFixed32, sizeof(kFixed32)));
  CLDB_ASSERT_EQ_INT(
      0, memcmp(buf.data + sizeof(kFixed32), kFixed64, sizeof(kFixed64)));
  cldb_buf_free(&buf);
}

CLDB_TEST(coding_varint32_boundaries) {
  static const uint8_t kZero[] = {0x00};
  static const uint8_t k127[] = {0x7f};
  static const uint8_t k128[] = {0x80, 0x01};
  static const uint8_t k16383[] = {0xff, 0x7f};
  static const uint8_t k16384[] = {0x80, 0x80, 0x01};
  static const uint8_t k2_21[] = {0x80, 0x80, 0x80, 0x01};
  static const uint8_t k2_28[] = {0x80, 0x80, 0x80, 0x80, 0x01};
  static const uint8_t kMax[] = {0xff, 0xff, 0xff, 0xff, 0x0f};

  expect_varint32(0, kZero, sizeof(kZero));
  expect_varint32(127, k127, sizeof(k127));
  expect_varint32(128, k128, sizeof(k128));
  expect_varint32(16383, k16383, sizeof(k16383));
  expect_varint32(16384, k16384, sizeof(k16384));
  expect_varint32(UINT32_C(1) << 21, k2_21, sizeof(k2_21));
  expect_varint32(UINT32_C(1) << 28, k2_28, sizeof(k2_28));
  expect_varint32(UINT32_MAX, kMax, sizeof(kMax));
}

CLDB_TEST(coding_varint32_patterns) {
  uint32_t i;

  for (i = 0; i < 32 * 32; ++i) {
    uint32_t value = (i / 32) << (i % 32);

    check_varint32(value);
  }
}

CLDB_TEST(coding_varint64_boundaries) {
  static const uint8_t kZero[] = {0x00};
  static const uint8_t k100[] = {0x64};
  static const uint8_t k2_32[] = {0x80, 0x80, 0x80, 0x80, 0x10};
  static const uint8_t k2_63[] = {0x80, 0x80, 0x80, 0x80, 0x80,
                                  0x80, 0x80, 0x80, 0x80, 0x01};
  static const uint8_t kMaxMinusOne[] = {0xfe, 0xff, 0xff, 0xff, 0xff,
                                         0xff, 0xff, 0xff, 0xff, 0x01};
  static const uint8_t kMax[] = {0xff, 0xff, 0xff, 0xff, 0xff,
                                 0xff, 0xff, 0xff, 0xff, 0x01};

  expect_varint64(0, kZero, sizeof(kZero));
  expect_varint64(100, k100, sizeof(k100));
  expect_varint64(UINT64_C(1) << 32, k2_32, sizeof(k2_32));
  expect_varint64(UINT64_C(1) << 63, k2_63, sizeof(k2_63));
  expect_varint64(UINT64_MAX - 1, kMaxMinusOne, sizeof(kMaxMinusOne));
  expect_varint64(UINT64_MAX, kMax, sizeof(kMax));
}

CLDB_TEST(coding_varint64_powers) {
  uint32_t power;

  for (power = 0; power < 64; ++power) {
    uint64_t value = UINT64_C(1) << power;

    check_varint64(value);
    check_varint64(value - 1);
    check_varint64(value + 1);
  }
}

CLDB_TEST(coding_varint32_errors) {
  static const uint8_t kOverlong[] = {0x81, 0x82, 0x83, 0x84, 0x85, 0x11};
  static const uint8_t kContinuation[] = {0x80, 0x80, 0x80, 0x80, 0x80};
  uint32_t large_value = (UINT32_C(1) << 31) + 100;
  char encoded[5];
  char *end = cldb_encode_varint32(encoded, large_value);
  size_t length = (size_t)(end - encoded);
  size_t i;
  uint32_t decoded;

  CLDB_ASSERT_EQ_UINT(5, length);
  for (i = 0; i < length; ++i) {
    cldb_slice_t input = cldb_slice_make(encoded, i);

    decoded = UINT32_C(0xdeadbeef);
    CLDB_EXPECT_EQ_PTR(
        NULL, cldb_get_varint32_ptr(encoded, encoded + i, &decoded));
    CLDB_EXPECT_EQ_UINT(UINT32_C(0xdeadbeef), decoded);

    CLDB_EXPECT_FALSE(cldb_get_varint32(&input, &decoded));
    CLDB_EXPECT_EQ_UINT(i, input.size);
    CLDB_EXPECT_EQ_UINT(UINT32_C(0xdeadbeef), decoded);
  }

  decoded = 0;
  CLDB_ASSERT_TRUE(cldb_get_varint32_ptr(encoded, end, &decoded) != NULL);
  CLDB_EXPECT_EQ_UINT(large_value, decoded);

  decoded = UINT32_C(0xdeadbeef);
  CLDB_EXPECT_EQ_PTR(
      NULL, cldb_get_varint32_ptr((const char *)kOverlong,
                                  (const char *)kOverlong + sizeof(kOverlong),
                                  &decoded));
  CLDB_EXPECT_EQ_UINT(UINT32_C(0xdeadbeef), decoded);

  decoded = UINT32_C(0xdeadbeef);
  CLDB_EXPECT_EQ_PTR(
      NULL,
      cldb_get_varint32_ptr(
          (const char *)kContinuation,
          (const char *)kContinuation + sizeof(kContinuation), &decoded));
  CLDB_EXPECT_EQ_UINT(UINT32_C(0xdeadbeef), decoded);
}

CLDB_TEST(coding_varint64_errors) {
  static const uint8_t kOverlong[] = {0x81, 0x82, 0x83, 0x84, 0x85,
                                      0x81, 0x82, 0x83, 0x84, 0x85,
                                      0x11};
  static const uint8_t kContinuation[] = {0x80, 0x80, 0x80, 0x80, 0x80,
                                          0x80, 0x80, 0x80, 0x80, 0x80};
  uint64_t large_value = (UINT64_C(1) << 63) + 100;
  char encoded[10];
  char *end = cldb_encode_varint64(encoded, large_value);
  size_t length = (size_t)(end - encoded);
  size_t i;
  uint64_t decoded;

  CLDB_ASSERT_EQ_UINT(10, length);
  for (i = 0; i < length; ++i) {
    cldb_slice_t input = cldb_slice_make(encoded, i);

    decoded = UINT64_C(0xdeadbeefdeadbeef);
    CLDB_EXPECT_EQ_PTR(
        NULL, cldb_get_varint64_ptr(encoded, encoded + i, &decoded));
    CLDB_EXPECT_EQ_UINT(UINT64_C(0xdeadbeefdeadbeef), decoded);

    CLDB_EXPECT_FALSE(cldb_get_varint64(&input, &decoded));
    CLDB_EXPECT_EQ_UINT(i, input.size);
    CLDB_EXPECT_EQ_UINT(UINT64_C(0xdeadbeefdeadbeef), decoded);
  }

  decoded = 0;
  CLDB_ASSERT_TRUE(cldb_get_varint64_ptr(encoded, end, &decoded) != NULL);
  CLDB_EXPECT_EQ_UINT(large_value, decoded);

  decoded = UINT64_C(0xdeadbeefdeadbeef);
  CLDB_EXPECT_EQ_PTR(
      NULL, cldb_get_varint64_ptr((const char *)kOverlong,
                                  (const char *)kOverlong + sizeof(kOverlong),
                                  &decoded));
  CLDB_EXPECT_EQ_UINT(UINT64_C(0xdeadbeefdeadbeef), decoded);

  decoded = UINT64_C(0xdeadbeefdeadbeef);
  CLDB_EXPECT_EQ_PTR(
      NULL,
      cldb_get_varint64_ptr(
          (const char *)kContinuation,
          (const char *)kContinuation + sizeof(kContinuation), &decoded));
  CLDB_EXPECT_EQ_UINT(UINT64_C(0xdeadbeefdeadbeef), decoded);
}

CLDB_TEST(coding_varint_noncanonical) {
  static const uint8_t kZero2[] = {0x80, 0x00};
  static const uint8_t kZero5[] = {0x80, 0x80, 0x80, 0x80, 0x00};
  static const uint8_t kZero10[] = {0x80, 0x80, 0x80, 0x80, 0x80,
                                    0x80, 0x80, 0x80, 0x80, 0x00};
  static const uint8_t kMax10[] = {0xff, 0xff, 0xff, 0xff, 0xff,
                                   0xff, 0xff, 0xff, 0xff, 0x7f};
  const char *end;
  uint32_t value32;
  uint64_t value64;

  value32 = 1;
  end = cldb_get_varint32_ptr((const char *)kZero2,
                              (const char *)kZero2 + sizeof(kZero2), &value32);
  CLDB_ASSERT_TRUE(end != NULL);
  CLDB_EXPECT_EQ_UINT(0, value32);
  CLDB_EXPECT_EQ_UINT(sizeof(kZero2), (size_t)(end - (const char *)kZero2));

  value32 = 1;
  end = cldb_get_varint32_ptr((const char *)kZero5,
                              (const char *)kZero5 + sizeof(kZero5), &value32);
  CLDB_ASSERT_TRUE(end != NULL);
  CLDB_EXPECT_EQ_UINT(0, value32);
  CLDB_EXPECT_EQ_UINT(sizeof(kZero5), (size_t)(end - (const char *)kZero5));

  value64 = 1;
  end = cldb_get_varint64_ptr((const char *)kZero10,
                              (const char *)kZero10 + sizeof(kZero10),
                              &value64);
  CLDB_ASSERT_TRUE(end != NULL);
  CLDB_EXPECT_EQ_UINT(0, value64);

  value64 = 0;
  end = cldb_get_varint64_ptr((const char *)kMax10,
                              (const char *)kMax10 + sizeof(kMax10), &value64);
  CLDB_ASSERT_TRUE(end != NULL);
  CLDB_EXPECT_EQ_UINT(UINT64_MAX, value64);
}

CLDB_TEST(coding_length_prefixed) {
  static const char kBinary[] = {'a', '\0', 'b'};
  char big[200];
  uint8_t expected[215];
  size_t expected_size = 0;
  cldb_buf_t buf;
  cldb_slice_t input;
  cldb_slice_t value;

  expected[expected_size++] = 0x00;
  expected[expected_size++] = 0x03;
  expected[expected_size++] = 'f';
  expected[expected_size++] = 'o';
  expected[expected_size++] = 'o';
  expected[expected_size++] = 0x03;
  expected[expected_size++] = 'b';
  expected[expected_size++] = 'a';
  expected[expected_size++] = 'r';
  expected[expected_size++] = 0x03;
  expected[expected_size++] = 'a';
  expected[expected_size++] = 0x00;
  expected[expected_size++] = 'b';
  expected[expected_size++] = 0xc8;
  expected[expected_size++] = 0x01;
  memset(big, 'x', sizeof(big));
  memcpy(expected + expected_size, big, sizeof(big));
  expected_size += sizeof(big);

  cldb_buf_init(&buf);
  CLDB_ASSERT_TRUE(cldb_put_length_prefixed_slice(&buf, cldb_slice_empty()));
  CLDB_ASSERT_TRUE(
      cldb_put_length_prefixed_slice(&buf, cldb_slice_make("foo", 3)));
  CLDB_ASSERT_TRUE(
      cldb_put_length_prefixed_slice(&buf, cldb_slice_make("bar", 3)));
  CLDB_ASSERT_TRUE(cldb_put_length_prefixed_slice(
      &buf, cldb_slice_make(kBinary, sizeof(kBinary))));
  CLDB_ASSERT_TRUE(
      cldb_put_length_prefixed_slice(&buf, cldb_slice_make(big, sizeof(big))));
  expect_bytes(&buf, expected, expected_size);

  input = cldb_buf_slice(&buf);
  CLDB_ASSERT_TRUE(cldb_get_length_prefixed_slice(&input, &value));
  CLDB_ASSERT_TRUE(cldb_slice_is_empty(value));

  CLDB_ASSERT_TRUE(cldb_get_length_prefixed_slice(&input, &value));
  CLDB_ASSERT_EQ_UINT(3, value.size);
  CLDB_ASSERT_EQ_INT(0, memcmp(value.data, "foo", 3));

  CLDB_ASSERT_TRUE(cldb_get_length_prefixed_slice(&input, &value));
  CLDB_ASSERT_EQ_UINT(3, value.size);
  CLDB_ASSERT_EQ_INT(0, memcmp(value.data, "bar", 3));

  CLDB_ASSERT_TRUE(cldb_get_length_prefixed_slice(&input, &value));
  CLDB_ASSERT_EQ_UINT(sizeof(kBinary), value.size);
  CLDB_ASSERT_EQ_INT(0, memcmp(value.data, kBinary, sizeof(kBinary)));

  CLDB_ASSERT_TRUE(cldb_get_length_prefixed_slice(&input, &value));
  CLDB_ASSERT_EQ_UINT(sizeof(big), value.size);
  CLDB_ASSERT_EQ_INT(0, memcmp(value.data, big, sizeof(big)));
  CLDB_EXPECT_EQ_UINT(0, input.size);

  cldb_buf_free(&buf);
}

CLDB_TEST(coding_length_prefixed_errors) {
  static const char kTruncatedPrefix[] = {(char)0x80};
  static const char kTruncatedPayload[] = {0x03, 'f', 'o'};
  static const char kHugeLength[] = {(char)0xff, (char)0xff, (char)0xff,
                                     (char)0xff, 0x0f, 'x'};
  cldb_slice_t unchanged = cldb_slice_make("unchanged", 9);
  cldb_slice_t input;
  cldb_slice_t result = unchanged;

  input = cldb_slice_make(kTruncatedPrefix, sizeof(kTruncatedPrefix));
  CLDB_EXPECT_FALSE(cldb_get_length_prefixed_slice(&input, &result));
  CLDB_EXPECT_TRUE(cldb_slice_equal(result, unchanged));
  CLDB_EXPECT_TRUE(
      cldb_slice_equal(input,
                       cldb_slice_make(kTruncatedPrefix,
                                       sizeof(kTruncatedPrefix))));

  input = cldb_slice_make(kTruncatedPayload, sizeof(kTruncatedPayload));
  CLDB_EXPECT_FALSE(cldb_get_length_prefixed_slice(&input, &result));
  CLDB_EXPECT_TRUE(cldb_slice_equal(result, unchanged));
  CLDB_EXPECT_TRUE(cldb_slice_equal(
      input, cldb_slice_make(kTruncatedPayload + 1,
                             sizeof(kTruncatedPayload) - 1)));

  input = cldb_slice_make(kHugeLength, sizeof(kHugeLength));
  CLDB_EXPECT_FALSE(cldb_get_length_prefixed_slice(&input, &result));
  CLDB_EXPECT_TRUE(cldb_slice_equal(result, unchanged));
  CLDB_EXPECT_TRUE(cldb_slice_equal(
      input, cldb_slice_make(kHugeLength + 5, sizeof(kHugeLength) - 5)));
}

CLDB_TEST(coding_invalid_arguments) {
  cldb_slice_t empty = cldb_slice_empty();
  cldb_slice_t null_slice = {NULL, 0};
  cldb_slice_t null_data = {NULL, 1};
  cldb_slice_t output = cldb_slice_empty();
  cldb_buf_t buf;
  uint32_t value32 = 0;
  uint64_t value64 = 0;
#if SIZE_MAX > UINT32_MAX
  cldb_slice_t huge;
#endif

  cldb_buf_init(&buf);
  CLDB_EXPECT_FALSE(cldb_put_fixed32(NULL, 1));
  CLDB_EXPECT_FALSE(cldb_put_fixed64(NULL, 1));
  CLDB_EXPECT_FALSE(cldb_put_varint32(NULL, 1));
  CLDB_EXPECT_FALSE(cldb_put_varint64(NULL, 1));
  CLDB_EXPECT_FALSE(cldb_put_length_prefixed_slice(NULL, empty));
  CLDB_EXPECT_FALSE(cldb_put_length_prefixed_slice(&buf, null_data));
  CLDB_EXPECT_EQ_UINT(0, buf.size);

  CLDB_EXPECT_FALSE(cldb_get_varint32(NULL, &value32));
  CLDB_EXPECT_FALSE(cldb_get_varint64(NULL, &value64));
  CLDB_EXPECT_FALSE(cldb_get_varint32(&empty, NULL));
  CLDB_EXPECT_FALSE(cldb_get_varint64(&empty, NULL));
  CLDB_EXPECT_FALSE(cldb_get_varint32(&null_slice, &value32));
  CLDB_EXPECT_FALSE(cldb_get_varint64(&null_slice, &value64));
  CLDB_EXPECT_EQ_PTR(NULL, cldb_get_varint32_ptr(NULL, NULL, &value32));
  CLDB_EXPECT_EQ_PTR(NULL, cldb_get_varint64_ptr(NULL, NULL, &value64));
  CLDB_EXPECT_FALSE(cldb_get_length_prefixed_slice(NULL, &output));
  CLDB_EXPECT_FALSE(cldb_get_length_prefixed_slice(&empty, NULL));

#if SIZE_MAX > UINT32_MAX
  huge = cldb_slice_make("x", (size_t)UINT32_MAX + 1);
  CLDB_EXPECT_FALSE(cldb_put_length_prefixed_slice(&buf, huge));
  CLDB_EXPECT_EQ_UINT(0, buf.size);
#endif

  cldb_buf_free(&buf);
}

void cldb_register_coding_tests(void) {
  CLDB_RUN_TEST(coding_fixed32_roundtrip);
  CLDB_RUN_TEST(coding_fixed64_roundtrip);
  CLDB_RUN_TEST(coding_fixed_little_endian);
  CLDB_RUN_TEST(coding_varint32_boundaries);
  CLDB_RUN_TEST(coding_varint32_patterns);
  CLDB_RUN_TEST(coding_varint64_boundaries);
  CLDB_RUN_TEST(coding_varint64_powers);
  CLDB_RUN_TEST(coding_varint32_errors);
  CLDB_RUN_TEST(coding_varint64_errors);
  CLDB_RUN_TEST(coding_varint_noncanonical);
  CLDB_RUN_TEST(coding_length_prefixed);
  CLDB_RUN_TEST(coding_length_prefixed_errors);
  CLDB_RUN_TEST(coding_invalid_arguments);
}
