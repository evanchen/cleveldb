#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "cleveldb/buf.h"
#include "cleveldb/comparator.h"
#include "cleveldb/filter_policy.h"
#include "cleveldb/slice.h"
#include "cleveldb/status.h"
#include "db/dbformat.h"
#include "harness.h"
#include "status_test_util.h"

/* 内部键、InternalKeyComparator、InternalFilterPolicy 与 LookupKey；
 * 对应上游 db/dbformat_test.cc 并补充排序和过滤器变换用例。 */

enum { kTestBufferSize = 256 };

static int sign_of(int value) { return (value > 0) - (value < 0); }

static void make_ikey(cldb_buf_t *key, cldb_slice_t user_key,
                      cldb_sequence_number_t sequence,
                      cldb_value_type_t type) {
  cldb_buf_init(key);
  CLDB_ASSERT_TRUE(
      cldb_append_internal_key(key, user_key, sequence, type));
}

static void expect_roundtrip(const char *data, size_t size,
                             cldb_sequence_number_t sequence,
                             cldb_value_type_t type) {
  cldb_buf_t encoded;
  cldb_slice_t internal;
  cldb_parsed_internal_key_t parsed;
  cldb_slice_t user = cldb_slice_make(data, size);

  make_ikey(&encoded, user, sequence, type);
  internal = cldb_buf_slice(&encoded);
  CLDB_ASSERT_TRUE(cldb_parse_internal_key(internal, &parsed));
  CLDB_EXPECT_TRUE(cldb_slice_equal(user, parsed.user_key));
  CLDB_EXPECT_EQ_UINT(sequence, parsed.sequence);
  CLDB_EXPECT_EQ_INT(type, parsed.type);
  CLDB_EXPECT_TRUE(
      cldb_slice_equal(user, cldb_extract_user_key(internal)));
  cldb_buf_free(&encoded);
}

CLDB_TEST(dbformat_constants) {
  CLDB_EXPECT_EQ_INT(7, CLDB_NUM_LEVELS);
  CLDB_EXPECT_EQ_UINT(UINT64_C(0x00ffffffffffffff),
                      CLDB_MAX_SEQUENCE_NUMBER);
  CLDB_EXPECT_EQ_INT(CLDB_VALUE_TYPE_DELETION, 0);
  CLDB_EXPECT_EQ_INT(CLDB_VALUE_TYPE_VALUE, 1);
  CLDB_EXPECT_EQ_INT(CLDB_VALUE_TYPE_FOR_SEEK, CLDB_VALUE_TYPE_VALUE);
  CLDB_EXPECT_EQ_UINT(8, CLDB_INTERNAL_KEY_TAG_SIZE);
}

CLDB_TEST(dbformat_internal_key_encode_decode) {
  static const char *kUserKeys[] = {
      "", "k", "hello", "longgggggggggggggggggggg",
  };
  static const cldb_sequence_number_t kSequences[] = {
      0,
      1,
      2,
      3,
      (UINT64_C(1) << 8) - 1,
      UINT64_C(1) << 8,
      (UINT64_C(1) << 8) + 1,
      (UINT64_C(1) << 16) - 1,
      UINT64_C(1) << 16,
      (UINT64_C(1) << 16) + 1,
      (UINT64_C(1) << 32) - 1,
      UINT64_C(1) << 32,
      (UINT64_C(1) << 32) + 1,
      CLDB_MAX_SEQUENCE_NUMBER,
  };
  static const char kExpectedFoo[] = {
      'f', 'o', 'o', 0x01, 0x64, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00,
  };
  static const char kExpectedMaxTag[] = {
      'k', 0x01, (char)0xff, (char)0xff, (char)0xff,
      (char)0xff, (char)0xff, (char)0xff, (char)0xff,
  };
  cldb_buf_t encoded;
  cldb_slice_t internal;
  size_t k;
  size_t s;

  for (k = 0; k < sizeof(kUserKeys) / sizeof(kUserKeys[0]); ++k) {
    for (s = 0; s < sizeof(kSequences) / sizeof(kSequences[0]); ++s) {
      expect_roundtrip(kUserKeys[k], strlen(kUserKeys[k]), kSequences[s],
                       CLDB_VALUE_TYPE_VALUE);
      expect_roundtrip("hello", 5, 1, CLDB_VALUE_TYPE_DELETION);
    }
  }

  make_ikey(&encoded, cldb_slice_make("foo", 3), 100,
            CLDB_VALUE_TYPE_VALUE);
  internal = cldb_buf_slice(&encoded);
  CLDB_EXPECT_EQ_UINT(sizeof(kExpectedFoo), internal.size);
  if (internal.size == sizeof(kExpectedFoo)) {
    CLDB_EXPECT_EQ_INT(
        0, memcmp(internal.data, kExpectedFoo, sizeof(kExpectedFoo)));
  }
  cldb_buf_free(&encoded);

  make_ikey(&encoded, cldb_slice_make("k", 1),
            CLDB_MAX_SEQUENCE_NUMBER, CLDB_VALUE_TYPE_VALUE);
  internal = cldb_buf_slice(&encoded);
  CLDB_EXPECT_EQ_UINT(sizeof(kExpectedMaxTag), internal.size);
  if (internal.size == sizeof(kExpectedMaxTag)) {
    CLDB_EXPECT_EQ_INT(
        0, memcmp(internal.data, kExpectedMaxTag, sizeof(kExpectedMaxTag)));
  }
  cldb_buf_free(&encoded);
}

CLDB_TEST(dbformat_internal_key_append_self) {
  cldb_buf_t buf;
  cldb_slice_t user;
  cldb_parsed_internal_key_t parsed;
  size_t i;

  cldb_buf_init(&buf);
  CLDB_ASSERT_TRUE(cldb_buf_assign(&buf, "foo", 3));
  user = cldb_buf_slice(&buf);
  CLDB_ASSERT_TRUE(
      cldb_append_internal_key(&buf, user, 7, CLDB_VALUE_TYPE_VALUE));
  CLDB_ASSERT_TRUE(
      cldb_parse_internal_key(cldb_buf_slice(&buf), &parsed));
  CLDB_EXPECT_EQ_UINT(6, parsed.user_key.size);
  CLDB_EXPECT_EQ_INT(0, memcmp(parsed.user_key.data, "foofoo", 6));
  CLDB_EXPECT_EQ_UINT(7, parsed.sequence);
  CLDB_EXPECT_EQ_INT(CLDB_VALUE_TYPE_VALUE, parsed.type);

  /* 源切片仍指向 buf，追加需要扩容时必须重新定位后再复制。 */
  cldb_buf_clear(&buf);
  CLDB_ASSERT_TRUE(cldb_buf_resize(&buf, 200));
  for (i = 0; i < 200; ++i) {
    buf.data[i] = (char)('a' + (int)(i % 26));
  }
  user = cldb_buf_slice(&buf);
  CLDB_ASSERT_TRUE(
      cldb_append_internal_key(&buf, user, 9, CLDB_VALUE_TYPE_DELETION));
  CLDB_ASSERT_TRUE(
      cldb_parse_internal_key(cldb_buf_slice(&buf), &parsed));
  CLDB_EXPECT_EQ_UINT(400, parsed.user_key.size);
  CLDB_EXPECT_EQ_INT(0, memcmp(parsed.user_key.data,
                               parsed.user_key.data + 200, 200));
  CLDB_EXPECT_EQ_INT('a', (unsigned char)parsed.user_key.data[0]);
  CLDB_EXPECT_EQ_INT('a', (unsigned char)parsed.user_key.data[200]);
  CLDB_EXPECT_EQ_UINT(9, parsed.sequence);
  CLDB_EXPECT_EQ_INT(CLDB_VALUE_TYPE_DELETION, parsed.type);
  cldb_buf_free(&buf);
}

CLDB_TEST(dbformat_internal_key_parse_errors) {
  cldb_buf_t valid;
  cldb_parsed_internal_key_t parsed =
      cldb_parsed_internal_key_make(cldb_slice_make("sentinel", 8), 123,
                                    CLDB_VALUE_TYPE_VALUE);
  char short_key[7] = {0};
  char bad_type_two[9] = {'x', 2, 0, 0, 0, 0, 0, 0, 0};
  char bad_type_ff[9] = {'x', (char)0xff, 0, 0, 0, 0, 0, 0, 0};

  CLDB_EXPECT_FALSE(cldb_parse_internal_key(
      cldb_slice_make(short_key, sizeof(short_key)), &parsed));
  CLDB_EXPECT_FALSE(cldb_parse_internal_key(
      cldb_slice_make(bad_type_two, sizeof(bad_type_two)), &parsed));
  CLDB_EXPECT_FALSE(cldb_parse_internal_key(
      cldb_slice_make(bad_type_ff, sizeof(bad_type_ff)), &parsed));
  CLDB_EXPECT_TRUE(cldb_slice_equal(parsed.user_key,
                                    cldb_slice_make("sentinel", 8)));
  CLDB_EXPECT_EQ_UINT(123, parsed.sequence);
  CLDB_EXPECT_EQ_INT(CLDB_VALUE_TYPE_VALUE, parsed.type);

  make_ikey(&valid, cldb_slice_make("foo", 3), 1,
            CLDB_VALUE_TYPE_VALUE);
  CLDB_EXPECT_FALSE(
      cldb_parse_internal_key(cldb_buf_slice(&valid), NULL));
  cldb_buf_free(&valid);
}

CLDB_TEST(dbformat_internal_key_append_errors) {
  cldb_buf_t buf;
  cldb_slice_t null_user = {NULL, 5};
  cldb_slice_t huge_user = {NULL, SIZE_MAX};

  cldb_buf_init(&buf);
  CLDB_ASSERT_TRUE(cldb_buf_assign(&buf, "keep", 4));
  CLDB_EXPECT_FALSE(cldb_append_internal_key(
      &buf, cldb_slice_make("x", 1), CLDB_MAX_SEQUENCE_NUMBER + 1,
      CLDB_VALUE_TYPE_VALUE));
  CLDB_EXPECT_EQ_UINT(4, buf.size);
  CLDB_EXPECT_EQ_INT(0, memcmp(buf.data, "keep", 4));

  CLDB_EXPECT_FALSE(cldb_append_internal_key(
      &buf, cldb_slice_make("x", 1), 1, (cldb_value_type_t)2));
  CLDB_EXPECT_EQ_UINT(4, buf.size);
  CLDB_EXPECT_EQ_INT(0, memcmp(buf.data, "keep", 4));

  CLDB_EXPECT_FALSE(
      cldb_append_internal_key(&buf, null_user, 1, CLDB_VALUE_TYPE_VALUE));
  CLDB_EXPECT_FALSE(cldb_append_internal_key(&buf, huge_user, 1,
                                             CLDB_VALUE_TYPE_VALUE));
  CLDB_EXPECT_FALSE(cldb_append_internal_key(
      NULL, cldb_slice_make("x", 1), 1, CLDB_VALUE_TYPE_VALUE));
  CLDB_EXPECT_EQ_UINT(4, buf.size);
  CLDB_EXPECT_EQ_INT(0, memcmp(buf.data, "keep", 4));
  cldb_buf_free(&buf);
}

CLDB_TEST(dbformat_extract_user_key) {
  cldb_buf_t empty_user;
  cldb_buf_t foo;

  make_ikey(&empty_user, cldb_slice_empty(), 1, CLDB_VALUE_TYPE_VALUE);
  CLDB_EXPECT_TRUE(cldb_slice_is_empty(
      cldb_extract_user_key(cldb_buf_slice(&empty_user))));
  cldb_buf_free(&empty_user);

  make_ikey(&foo, cldb_slice_make("foo", 3), 1, CLDB_VALUE_TYPE_VALUE);
  CLDB_EXPECT_TRUE(cldb_slice_equal(cldb_extract_user_key(
                                        cldb_buf_slice(&foo)),
                                    cldb_slice_make("foo", 3)));
  cldb_buf_free(&foo);

#ifdef NDEBUG
  /* release 版对非法长度不崩溃，返回空切片。 */
  {
    char short_key[7] = {0};

    CLDB_EXPECT_TRUE(cldb_slice_is_empty(cldb_extract_user_key(
        cldb_slice_make(short_key, sizeof(short_key)))));
    CLDB_EXPECT_TRUE(
        cldb_slice_is_empty(cldb_extract_user_key(cldb_slice_empty())));
  }
#endif
}

static void expect_ikey_compare(
    const cldb_internal_key_comparator_t *comparator, const char *a,
    size_t a_size, cldb_sequence_number_t a_seq, cldb_value_type_t a_type,
    const char *b, size_t b_size, cldb_sequence_number_t b_seq,
    cldb_value_type_t b_type, int expected_sign) {
  cldb_buf_t a_key;
  cldb_buf_t b_key;
  int forward;
  int backward;

  make_ikey(&a_key, cldb_slice_make(a, a_size), a_seq, a_type);
  make_ikey(&b_key, cldb_slice_make(b, b_size), b_seq, b_type);
  forward = cldb_comparator_compare(&comparator->base,
                                    cldb_buf_slice(&a_key),
                                    cldb_buf_slice(&b_key));
  backward = cldb_comparator_compare(&comparator->base,
                                     cldb_buf_slice(&b_key),
                                     cldb_buf_slice(&a_key));
  CLDB_EXPECT_EQ_INT(expected_sign, sign_of(forward));
  CLDB_EXPECT_EQ_INT(-expected_sign, sign_of(backward));
  cldb_buf_free(&a_key);
  cldb_buf_free(&b_key);
}

CLDB_TEST(dbformat_internal_key_comparator) {
  cldb_internal_key_comparator_t comparator;

  cldb_internal_key_comparator_init(&comparator,
                                    cldb_bytewise_comparator());
  CLDB_EXPECT_EQ_STR("leveldb.InternalKeyComparator",
                     cldb_comparator_name(&comparator.base));
  CLDB_EXPECT_EQ_PTR(
      cldb_bytewise_comparator(),
      cldb_internal_key_comparator_user_comparator(&comparator));

  expect_ikey_compare(&comparator, "a", 1, 1, CLDB_VALUE_TYPE_VALUE,
                      "b", 1, 1, CLDB_VALUE_TYPE_VALUE, -1);
  expect_ikey_compare(&comparator, "", 0, 1, CLDB_VALUE_TYPE_VALUE,
                      "a", 1, 1, CLDB_VALUE_TYPE_VALUE, -1);
  expect_ikey_compare(&comparator, "\xff", 1, 1, CLDB_VALUE_TYPE_VALUE,
                      "\x00", 1, 1, CLDB_VALUE_TYPE_VALUE, 1);
  /* 同一 user key：sequence 大在前。 */
  expect_ikey_compare(&comparator, "a", 1, 2, CLDB_VALUE_TYPE_VALUE,
                      "a", 1, 1, CLDB_VALUE_TYPE_VALUE, -1);
  /* sequence 相同：ValueType 大在前。 */
  expect_ikey_compare(&comparator, "a", 1, 7, CLDB_VALUE_TYPE_VALUE,
                      "a", 1, 7, CLDB_VALUE_TYPE_DELETION, -1);
  expect_ikey_compare(&comparator, "a", 1, 7, CLDB_VALUE_TYPE_DELETION,
                      "a", 1, 7, CLDB_VALUE_TYPE_DELETION, 0);
}

typedef struct reverse_comparator {
  cldb_comparator_t base; /* 必须为首成员 */
} reverse_comparator_t;

static int reverse_compare(const cldb_comparator_t *comparator,
                           cldb_slice_t a, cldb_slice_t b) {
  (void)comparator;
  return cldb_slice_compare(b, a);
}

static const char *reverse_name(const cldb_comparator_t *comparator) {
  (void)comparator;
  return "test.ReverseComparator";
}

static void reverse_no_separator(const cldb_comparator_t *comparator,
                                 char *start, size_t *start_size,
                                 cldb_slice_t limit) {
  (void)comparator;
  (void)start;
  (void)start_size;
  (void)limit;
}

static void reverse_no_successor(const cldb_comparator_t *comparator,
                                 char *key, size_t *key_size) {
  (void)comparator;
  (void)key;
  (void)key_size;
}

static const cldb_comparator_vtbl_t kReverseVtbl = {
    .compare = reverse_compare,
    .name = reverse_name,
    .find_shortest_separator = reverse_no_separator,
    .find_short_successor = reverse_no_successor,
};

CLDB_TEST(dbformat_internal_key_sort_matrix) {
  static const cldb_sequence_number_t kExpectedSequences[6] = {
      CLDB_MAX_SEQUENCE_NUMBER, CLDB_MAX_SEQUENCE_NUMBER, 3, 3, 1, 1,
  };
  static const cldb_value_type_t kExpectedTypes[6] = {
      CLDB_VALUE_TYPE_VALUE, CLDB_VALUE_TYPE_DELETION, CLDB_VALUE_TYPE_VALUE,
      CLDB_VALUE_TYPE_DELETION, CLDB_VALUE_TYPE_VALUE,
      CLDB_VALUE_TYPE_DELETION,
  };
  static const cldb_sequence_number_t kInputSequences[6] = {
      1, CLDB_MAX_SEQUENCE_NUMBER, 3, 1, CLDB_MAX_SEQUENCE_NUMBER, 3,
  };
  static const cldb_value_type_t kInputTypes[6] = {
      CLDB_VALUE_TYPE_DELETION, CLDB_VALUE_TYPE_VALUE, CLDB_VALUE_TYPE_DELETION,
      CLDB_VALUE_TYPE_VALUE, CLDB_VALUE_TYPE_DELETION, CLDB_VALUE_TYPE_VALUE,
  };
  cldb_internal_key_comparator_t comparator;
  cldb_buf_t keys[6];
  size_t i;

  cldb_internal_key_comparator_init(&comparator,
                                    cldb_bytewise_comparator());
  for (i = 0; i < 6; ++i) {
    make_ikey(&keys[i], cldb_slice_make("same-user", 9),
              kInputSequences[i], kInputTypes[i]);
  }

  /* 插入排序；结果应为 tag 数值降序。 */
  for (i = 1; i < 6; ++i) {
    size_t j = i;

    while (j > 0 &&
           cldb_comparator_compare(&comparator.base,
                                   cldb_buf_slice(&keys[j]),
                                   cldb_buf_slice(&keys[j - 1])) < 0) {
      cldb_buf_t tmp = keys[j];

      keys[j] = keys[j - 1];
      keys[j - 1] = tmp;
      --j;
    }
  }
  for (i = 0; i < 6; ++i) {
    cldb_parsed_internal_key_t parsed;

    CLDB_ASSERT_TRUE(
        cldb_parse_internal_key(cldb_buf_slice(&keys[i]), &parsed));
    CLDB_EXPECT_TRUE(cldb_slice_equal(parsed.user_key,
                                      cldb_slice_make("same-user", 9)));
    CLDB_EXPECT_EQ_UINT(kExpectedSequences[i], parsed.sequence);
    CLDB_EXPECT_EQ_INT(kExpectedTypes[i], parsed.type);
  }

  for (i = 0; i < 6; ++i) {
    cldb_buf_free(&keys[i]);
  }
}

CLDB_TEST(dbformat_internal_key_comparator_custom) {
  reverse_comparator_t reverse;
  cldb_internal_key_comparator_t comparator;

  reverse.base.vtbl = &kReverseVtbl;
  cldb_internal_key_comparator_init(&comparator, &reverse.base);
  CLDB_EXPECT_EQ_PTR(
      &reverse.base,
      cldb_internal_key_comparator_user_comparator(&comparator));
  CLDB_EXPECT_EQ_STR("leveldb.InternalKeyComparator",
                     cldb_comparator_name(&comparator.base));

  /* 用户比较器决定 user key 顺序；同一 user key 仍按 tag 降序。 */
  expect_ikey_compare(&comparator, "a", 1, 1, CLDB_VALUE_TYPE_VALUE,
                      "b", 1, 1, CLDB_VALUE_TYPE_VALUE, 1);
  expect_ikey_compare(&comparator, "a", 1, 2, CLDB_VALUE_TYPE_VALUE,
                      "a", 1, 1, CLDB_VALUE_TYPE_VALUE, -1);
}

static void expect_shortest_separator(
    const cldb_internal_key_comparator_t *comparator, const char *start,
    size_t start_size, cldb_sequence_number_t start_seq,
    cldb_value_type_t start_type, const char *limit, size_t limit_size,
    cldb_sequence_number_t limit_seq, cldb_value_type_t limit_type,
    const char *expected, size_t expected_size,
    cldb_sequence_number_t expected_seq, cldb_value_type_t expected_type) {
  char buffer[kTestBufferSize];
  cldb_buf_t start_key;
  cldb_buf_t limit_key;
  cldb_buf_t expected_key;
  cldb_slice_t result;
  size_t size;
  bool changed;

  make_ikey(&start_key, cldb_slice_make(start, start_size), start_seq,
            start_type);
  make_ikey(&limit_key, cldb_slice_make(limit, limit_size), limit_seq,
            limit_type);
  if (expected == NULL) {
    cldb_buf_init(&expected_key);
    CLDB_ASSERT_TRUE(cldb_buf_assign(&expected_key, start_key.data,
                                     start_key.size));
  } else {
    make_ikey(&expected_key, cldb_slice_make(expected, expected_size),
              expected_seq, expected_type);
  }
  CLDB_ASSERT_TRUE(start_key.size <= sizeof(buffer));
  memcpy(buffer, start_key.data, start_key.size);
  size = start_key.size;
  cldb_comparator_find_shortest_separator(
      &comparator->base, buffer, &size, cldb_buf_slice(&limit_key));

  CLDB_EXPECT_TRUE(size <= start_key.size);
  CLDB_EXPECT_EQ_UINT(expected_key.size, size);
  if (size == expected_key.size && size > 0) {
    CLDB_EXPECT_EQ_INT(0, memcmp(buffer, expected_key.data, size));
  }
  result = cldb_slice_make(buffer, size);
  changed = !cldb_slice_equal(result, cldb_buf_slice(&start_key));
  if (changed) {
    CLDB_EXPECT_TRUE(cldb_comparator_compare(&comparator->base,
                                             cldb_buf_slice(&start_key),
                                             result) < 0);
  }
  if (cldb_comparator_compare(&comparator->base,
                              cldb_buf_slice(&start_key),
                              cldb_buf_slice(&limit_key)) < 0) {
    CLDB_EXPECT_TRUE(cldb_comparator_compare(&comparator->base, result,
                                             cldb_buf_slice(&limit_key)) <
                     0);
  }
  cldb_buf_free(&expected_key);
  cldb_buf_free(&limit_key);
  cldb_buf_free(&start_key);
}

CLDB_TEST(dbformat_internal_key_shortest_separator) {
  cldb_internal_key_comparator_t comparator;

  cldb_internal_key_comparator_init(&comparator,
                                    cldb_bytewise_comparator());
  expect_shortest_separator(
      &comparator, "foo", 3, 100, CLDB_VALUE_TYPE_VALUE, "foo", 3, 99,
      CLDB_VALUE_TYPE_VALUE, NULL, 0, 0, CLDB_VALUE_TYPE_VALUE);
  expect_shortest_separator(
      &comparator, "foo", 3, 100, CLDB_VALUE_TYPE_VALUE, "foo", 3, 101,
      CLDB_VALUE_TYPE_VALUE, NULL, 0, 0, CLDB_VALUE_TYPE_VALUE);
  expect_shortest_separator(
      &comparator, "foo", 3, 100, CLDB_VALUE_TYPE_VALUE, "foo", 3, 100,
      CLDB_VALUE_TYPE_VALUE, NULL, 0, 0, CLDB_VALUE_TYPE_VALUE);
  expect_shortest_separator(
      &comparator, "foo", 3, 100, CLDB_VALUE_TYPE_VALUE, "foo", 3, 100,
      CLDB_VALUE_TYPE_DELETION, NULL, 0, 0, CLDB_VALUE_TYPE_VALUE);
  expect_shortest_separator(
      &comparator, "foo", 3, 100, CLDB_VALUE_TYPE_VALUE, "bar", 3, 99,
      CLDB_VALUE_TYPE_VALUE, NULL, 0, 0, CLDB_VALUE_TYPE_VALUE);
  expect_shortest_separator(
      &comparator, "foo", 3, 100, CLDB_VALUE_TYPE_VALUE, "hello", 5, 200,
      CLDB_VALUE_TYPE_VALUE, "g", 1, CLDB_MAX_SEQUENCE_NUMBER,
      CLDB_VALUE_TYPE_FOR_SEEK);
  expect_shortest_separator(
      &comparator, "foo", 3, 100, CLDB_VALUE_TYPE_VALUE, "foobar", 6, 200,
      CLDB_VALUE_TYPE_VALUE, NULL, 0, 0, CLDB_VALUE_TYPE_VALUE);
  expect_shortest_separator(
      &comparator, "foobar", 6, 100, CLDB_VALUE_TYPE_VALUE, "foo", 3, 200,
      CLDB_VALUE_TYPE_VALUE, NULL, 0, 0, CLDB_VALUE_TYPE_VALUE);
  expect_shortest_separator(
      &comparator, "abczzz", 6, 100, CLDB_VALUE_TYPE_VALUE, "abf", 3, 200,
      CLDB_VALUE_TYPE_VALUE, "abd", 3, CLDB_MAX_SEQUENCE_NUMBER,
      CLDB_VALUE_TYPE_FOR_SEEK);
}

static void expect_short_successor(
    const cldb_internal_key_comparator_t *comparator, const char *key,
    size_t key_size, cldb_sequence_number_t sequence,
    cldb_value_type_t type, const char *expected, size_t expected_size,
    cldb_sequence_number_t expected_seq, cldb_value_type_t expected_type) {
  char buffer[kTestBufferSize];
  cldb_buf_t original;
  cldb_buf_t expected_key;
  cldb_slice_t result;
  size_t size;

  make_ikey(&original, cldb_slice_make(key, key_size), sequence, type);
  if (expected == NULL) {
    cldb_buf_init(&expected_key);
    CLDB_ASSERT_TRUE(cldb_buf_assign(&expected_key, original.data,
                                     original.size));
  } else {
    make_ikey(&expected_key, cldb_slice_make(expected, expected_size),
              expected_seq, expected_type);
  }
  CLDB_ASSERT_TRUE(original.size <= sizeof(buffer));
  memcpy(buffer, original.data, original.size);
  size = original.size;
  cldb_comparator_find_short_successor(&comparator->base, buffer, &size);

  CLDB_EXPECT_TRUE(size <= original.size);
  CLDB_EXPECT_EQ_UINT(expected_key.size, size);
  if (size == expected_key.size && size > 0) {
    CLDB_EXPECT_EQ_INT(0, memcmp(buffer, expected_key.data, size));
  }
  result = cldb_slice_make(buffer, size);
  CLDB_EXPECT_TRUE(cldb_comparator_compare(
                       &comparator->base, cldb_buf_slice(&original),
                       result) <= 0);
  cldb_buf_free(&expected_key);
  cldb_buf_free(&original);
}

CLDB_TEST(dbformat_internal_key_short_successor) {
  cldb_internal_key_comparator_t comparator;

  cldb_internal_key_comparator_init(&comparator,
                                    cldb_bytewise_comparator());
  expect_short_successor(
      &comparator, "foo", 3, 100, CLDB_VALUE_TYPE_VALUE, "g", 1,
      CLDB_MAX_SEQUENCE_NUMBER, CLDB_VALUE_TYPE_FOR_SEEK);
  expect_short_successor(
      &comparator, "abc", 3, 100, CLDB_VALUE_TYPE_VALUE, "b", 1,
      CLDB_MAX_SEQUENCE_NUMBER, CLDB_VALUE_TYPE_FOR_SEEK);
  expect_short_successor(
      &comparator, "\xff\xff", 2, 100, CLDB_VALUE_TYPE_VALUE, NULL, 0, 0,
      CLDB_VALUE_TYPE_VALUE);
  expect_short_successor(
      &comparator, "", 0, 100, CLDB_VALUE_TYPE_VALUE, NULL, 0, 0,
      CLDB_VALUE_TYPE_VALUE);
}

static char *test_sink_append(void *arg, const void *data, size_t size) {
  cldb_buf_t *buf = (cldb_buf_t *)arg;
  size_t old_size = buf->size;

  if (size > SIZE_MAX - old_size) {
    return NULL;
  }
  if (data == NULL) {
    if (!cldb_buf_resize(buf, old_size + size)) {
      return NULL;
    }
  } else if (!cldb_buf_append(buf, data, size)) {
    return NULL;
  }
  return buf->data + old_size;
}

typedef struct recording_policy {
  cldb_filter_policy_t base; /* 必须为首成员 */
  size_t create_calls;
  size_t match_calls;
  cldb_slice_t keys[8];
  size_t key_count;
  cldb_slice_t match_key;
  const cldb_filter_policy_t *last_policy;
  bool fail_create;
} recording_policy_t;

/* 回调没有上下文参数，测试用全局指针取记录器。 */
static recording_policy_t *g_recording;

static const char *recording_name(const cldb_filter_policy_t *policy) {
  (void)policy;
  return "test.RecordingFilter";
}

static cldb_status_t recording_create(const cldb_filter_policy_t *policy,
                                      const cldb_slice_t *keys, size_t n,
                                      cldb_filter_sink_t *dst) {
  recording_policy_t *self = g_recording;
  static const char kMarker[] = {'R'};
  size_t i;

  if (self == NULL) {
    return cldb_status_io("recording.create_filter", "test not initialized");
  }
  self->last_policy = policy;
  ++self->create_calls;
  self->key_count = n < 8 ? n : 8;
  for (i = 0; i < self->key_count; ++i) {
    self->keys[i] = keys[i];
  }
  if (self->fail_create) {
    return cldb_status_corruption("recording.create_filter", "injected");
  }
  if (dst->append(dst->arg, kMarker, sizeof(kMarker)) == NULL) {
    return cldb_status_io("recording.create_filter", "append failed");
  }
  return cldb_status_ok();
}

static bool recording_key_may_match(const cldb_filter_policy_t *policy,
                                    cldb_slice_t key,
                                    cldb_slice_t filter) {
  recording_policy_t *self = g_recording;

  (void)filter;
  if (self == NULL) {
    return false;
  }
  self->last_policy = policy;
  ++self->match_calls;
  self->match_key = key;
  return cldb_slice_equal(key, cldb_slice_make("hit", 3));
}

static void recording_destroy(cldb_filter_policy_t *policy) {
  (void)policy;
}

static const cldb_filter_policy_vtbl_t kRecordingVtbl = {
    .name = recording_name,
    .create_filter = recording_create,
    .key_may_match = recording_key_may_match,
    .destroy = recording_destroy,
};

CLDB_TEST(dbformat_internal_filter_policy) {
  enum { kHugeUserKeySize = 65536 };
  recording_policy_t recording = {0};
  cldb_internal_filter_policy_t internal;
  cldb_buf_t first;
  cldb_buf_t empty;
  cldb_buf_t long_key;
  cldb_buf_t huge_user;
  cldb_buf_t huge_key;
  cldb_buf_t hit;
  cldb_buf_t dst;
  cldb_filter_sink_t sink;
  cldb_slice_t keys[4];
  char short_key[7] = {0};
  cldb_slice_t short_slice;
  size_t index;

  g_recording = &recording;
  recording.base.vtbl = &kRecordingVtbl;
  cldb_internal_filter_policy_init(&internal, &recording.base);
  make_ikey(&first, cldb_slice_make("foo", 3), 1,
            CLDB_VALUE_TYPE_VALUE);
  make_ikey(&empty, cldb_slice_empty(), 1, CLDB_VALUE_TYPE_VALUE);
  make_ikey(&long_key, cldb_slice_make("long-user-key", 13), 1,
            CLDB_VALUE_TYPE_DELETION);
  cldb_buf_init(&huge_user);
  CLDB_ASSERT_TRUE(cldb_buf_resize(&huge_user, kHugeUserKeySize));
  for (index = 0; index < kHugeUserKeySize; ++index) {
    huge_user.data[index] = (char)('a' + (int)(index % 26));
  }
  make_ikey(&huge_key, cldb_buf_slice(&huge_user), 1,
            CLDB_VALUE_TYPE_VALUE);
  keys[0] = cldb_buf_slice(&first);
  keys[1] = cldb_buf_slice(&empty);
  keys[2] = cldb_buf_slice(&long_key);
  keys[3] = cldb_buf_slice(&huge_key);

  cldb_buf_init(&dst);
  sink.arg = &dst;
  sink.append = test_sink_append;

  CLDB_EXPECT_EQ_STR("test.RecordingFilter",
                     cldb_filter_policy_name(&internal.base));
  CLDB_EXPECT_OK(
      cldb_filter_policy_create_filter(&internal.base, keys, 4, &sink));
  CLDB_EXPECT_EQ_UINT(1, recording.create_calls);
  CLDB_EXPECT_EQ_UINT(4, recording.key_count);
  CLDB_EXPECT_TRUE(cldb_slice_equal(recording.keys[0],
                                    cldb_slice_make("foo", 3)));
  CLDB_EXPECT_TRUE(cldb_slice_is_empty(recording.keys[1]));
  CLDB_EXPECT_TRUE(cldb_slice_equal(recording.keys[2],
                                    cldb_slice_make("long-user-key", 13)));
  CLDB_EXPECT_EQ_UINT(kHugeUserKeySize, recording.keys[3].size);
  if (recording.keys[3].size == kHugeUserKeySize) {
    CLDB_EXPECT_EQ_INT(0, memcmp(recording.keys[3].data, huge_user.data,
                                 kHugeUserKeySize));
  }
  CLDB_EXPECT_EQ_UINT(1, dst.size);
  CLDB_EXPECT_EQ_INT('R', (unsigned char)dst.data[0]);
  CLDB_EXPECT_EQ_PTR(&recording.base, recording.last_policy);

  /* 包装器不得修改调用者传入的内部键。 */
  {
    cldb_parsed_internal_key_t parsed;

    CLDB_ASSERT_TRUE(
        cldb_parse_internal_key(cldb_buf_slice(&first), &parsed));
    CLDB_EXPECT_TRUE(cldb_slice_equal(parsed.user_key,
                                      cldb_slice_make("foo", 3)));
  }

  make_ikey(&hit, cldb_slice_make("hit", 3), 1, CLDB_VALUE_TYPE_VALUE);
  CLDB_EXPECT_TRUE(cldb_filter_policy_key_may_match(
      &internal.base, cldb_buf_slice(&hit), cldb_buf_slice(&dst)));
  CLDB_EXPECT_EQ_UINT(1, recording.match_calls);
  CLDB_EXPECT_TRUE(cldb_slice_equal(recording.match_key,
                                    cldb_slice_make("hit", 3)));
  CLDB_EXPECT_EQ_PTR(&recording.base, recording.last_policy);

  short_slice = cldb_slice_make(short_key, sizeof(short_key));
  CLDB_EXPECT_FALSE(cldb_filter_policy_key_may_match(
      &internal.base, short_slice, cldb_buf_slice(&dst)));
  CLDB_EXPECT_EQ_UINT(1, recording.match_calls);

  {
    cldb_slice_t null_internal = {NULL, CLDB_INTERNAL_KEY_TAG_SIZE};

    CLDB_EXPECT_FALSE(cldb_filter_policy_key_may_match(
        &internal.base, null_internal, cldb_buf_slice(&dst)));
    CLDB_EXPECT_EQ_UINT(1, recording.match_calls);
    CLDB_EXPECT_STATUS(
        CLDB_STATUS_INVALID_ARGUMENT,
        cldb_filter_policy_create_filter(&internal.base, &null_internal, 1,
                                         &sink));
    CLDB_EXPECT_EQ_UINT(1, recording.create_calls);
  }

  CLDB_EXPECT_STATUS(
      CLDB_STATUS_INVALID_ARGUMENT,
      cldb_filter_policy_create_filter(&internal.base, &short_slice, 1,
                                       &sink));
  CLDB_EXPECT_EQ_UINT(1, recording.create_calls);
  CLDB_EXPECT_STATUS(
      CLDB_STATUS_INVALID_ARGUMENT,
      cldb_filter_policy_create_filter(&internal.base, keys, 1, NULL));
  CLDB_EXPECT_STATUS(
      CLDB_STATUS_INVALID_ARGUMENT,
      cldb_filter_policy_create_filter(&internal.base, keys, SIZE_MAX, &sink));
  CLDB_EXPECT_EQ_UINT(1, recording.create_calls);

  CLDB_EXPECT_OK(
      cldb_filter_policy_create_filter(&internal.base, NULL, 0, &sink));
  CLDB_EXPECT_EQ_UINT(2, recording.create_calls);
  CLDB_EXPECT_EQ_UINT(0, recording.key_count);
  CLDB_EXPECT_EQ_UINT(2, dst.size);

  recording.fail_create = true;
  CLDB_EXPECT_STATUS(
      CLDB_STATUS_CORRUPTION,
      cldb_filter_policy_create_filter(&internal.base, keys, 3, &sink));
  CLDB_EXPECT_EQ_UINT(3, recording.create_calls);

  /* 包装器不拥有用户策略；destroy 不触发用户策略的 destroy。 */
  cldb_filter_policy_destroy(&internal.base);
  g_recording = NULL;

  cldb_buf_free(&dst);
  cldb_buf_free(&hit);
  cldb_buf_free(&huge_key);
  cldb_buf_free(&huge_user);
  cldb_buf_free(&long_key);
  cldb_buf_free(&empty);
  cldb_buf_free(&first);
}

CLDB_TEST(dbformat_lookup_key) {
  static const char kExpectedFoo[] = {
      0x0b, 'f', 'o', 'o', 0x01, 0x64, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00,
  };
  static const char kExpectedEmpty[] = {
      0x08, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  };
  cldb_lookup_key_t lookup;
  cldb_buf_t long_user;
  cldb_slice_t memtable;
  cldb_slice_t internal;
  cldb_slice_t user;
  size_t size;

  CLDB_ASSERT_TRUE(
      cldb_lookup_key_init(&lookup, cldb_slice_make("foo", 3), 100));
  memtable = cldb_lookup_key_memtable_key(&lookup);
  internal = cldb_lookup_key_internal_key(&lookup);
  user = cldb_lookup_key_user_key(&lookup);
  CLDB_EXPECT_EQ_UINT(sizeof(kExpectedFoo), memtable.size);
  if (memtable.size == sizeof(kExpectedFoo)) {
    CLDB_EXPECT_EQ_INT(0, memcmp(memtable.data, kExpectedFoo,
                                 sizeof(kExpectedFoo)));
  }
  CLDB_EXPECT_EQ_UINT(sizeof(kExpectedFoo) - 1, internal.size);
  CLDB_EXPECT_TRUE(cldb_slice_equal(user, cldb_slice_make("foo", 3)));
  CLDB_EXPECT_EQ_INT(
      0, memcmp(internal.data, kExpectedFoo + 1, internal.size));
  cldb_lookup_key_destroy(&lookup);

  CLDB_ASSERT_TRUE(
      cldb_lookup_key_init(&lookup, cldb_slice_empty(), 0));
  memtable = cldb_lookup_key_memtable_key(&lookup);
  internal = cldb_lookup_key_internal_key(&lookup);
  user = cldb_lookup_key_user_key(&lookup);
  CLDB_EXPECT_EQ_UINT(sizeof(kExpectedEmpty), memtable.size);
  if (memtable.size == sizeof(kExpectedEmpty)) {
    CLDB_EXPECT_EQ_INT(0, memcmp(memtable.data, kExpectedEmpty,
                                 sizeof(kExpectedEmpty)));
  }
  CLDB_EXPECT_EQ_UINT(sizeof(kExpectedEmpty) - 1, internal.size);
  CLDB_EXPECT_EQ_INT(
      0, memcmp(internal.data, kExpectedEmpty + 1, internal.size));
  CLDB_EXPECT_TRUE(cldb_slice_is_empty(user));
  cldb_lookup_key_destroy(&lookup);

  CLDB_ASSERT_TRUE(cldb_lookup_key_init(&lookup, cldb_slice_make("k", 1),
                                        CLDB_MAX_SEQUENCE_NUMBER));
  internal = cldb_lookup_key_internal_key(&lookup);
  CLDB_EXPECT_EQ_UINT(9, internal.size);
  CLDB_EXPECT_EQ_INT('k', (unsigned char)internal.data[0]);
  CLDB_EXPECT_EQ_INT(0x01, (unsigned char)internal.data[1]);
  for (size = 2; size < 9; ++size) {
    CLDB_EXPECT_EQ_INT(0xff, (unsigned char)internal.data[size]);
  }
  cldb_lookup_key_destroy(&lookup);

  cldb_buf_init(&long_user);
  for (size = 0; size <= 188; ++size) {
    CLDB_EXPECT_TRUE(cldb_buf_resize(&long_user, size + 1));
    long_user.data[size] = (char)('a' + (int)(size % 26));
  }

  CLDB_ASSERT_TRUE(
      cldb_lookup_key_init(&lookup, cldb_buf_slice(&long_user), 7));
  /* 用户键 187 字节时总编码仍在内联阈值内。 */
  cldb_lookup_key_destroy(&lookup);
  CLDB_EXPECT_TRUE(cldb_buf_resize(&long_user, 188));
  CLDB_ASSERT_TRUE(
      cldb_lookup_key_init(&lookup, cldb_buf_slice(&long_user), 7));
  user = cldb_lookup_key_user_key(&lookup);
  CLDB_EXPECT_EQ_UINT(188, user.size);
  CLDB_EXPECT_EQ_INT(0, memcmp(user.data, long_user.data, 188));
  internal = cldb_lookup_key_internal_key(&lookup);
  CLDB_EXPECT_EQ_UINT(196, internal.size);
  CLDB_EXPECT_EQ_UINT(198,
                      cldb_lookup_key_memtable_key(&lookup).size);
  cldb_lookup_key_destroy(&lookup);
  cldb_buf_free(&long_user);

  CLDB_EXPECT_FALSE(cldb_lookup_key_init(
      &lookup, cldb_slice_make("k", 1), CLDB_MAX_SEQUENCE_NUMBER + 1));
  CLDB_EXPECT_TRUE(
      cldb_slice_is_empty(cldb_lookup_key_memtable_key(&lookup)));
  CLDB_EXPECT_TRUE(
      cldb_slice_is_empty(cldb_lookup_key_internal_key(&lookup)));
  CLDB_EXPECT_TRUE(cldb_slice_is_empty(cldb_lookup_key_user_key(&lookup)));
  cldb_lookup_key_destroy(&lookup);

  {
    cldb_slice_t null_user = {NULL, 5};
    cldb_slice_t huge_user = {NULL, SIZE_MAX};

    CLDB_EXPECT_FALSE(cldb_lookup_key_init(&lookup, null_user, 1));
    CLDB_EXPECT_FALSE(cldb_lookup_key_init(&lookup, huge_user, 1));
    CLDB_EXPECT_FALSE(cldb_lookup_key_init(NULL, cldb_slice_empty(), 1));
    cldb_lookup_key_destroy(NULL);
  }
}

void cldb_register_dbformat_tests(void) {
  CLDB_RUN_TEST(dbformat_constants);
  CLDB_RUN_TEST(dbformat_internal_key_encode_decode);
  CLDB_RUN_TEST(dbformat_internal_key_append_self);
  CLDB_RUN_TEST(dbformat_internal_key_parse_errors);
  CLDB_RUN_TEST(dbformat_internal_key_append_errors);
  CLDB_RUN_TEST(dbformat_extract_user_key);
  CLDB_RUN_TEST(dbformat_internal_key_comparator);
  CLDB_RUN_TEST(dbformat_internal_key_sort_matrix);
  CLDB_RUN_TEST(dbformat_internal_key_comparator_custom);
  CLDB_RUN_TEST(dbformat_internal_key_shortest_separator);
  CLDB_RUN_TEST(dbformat_internal_key_short_successor);
  CLDB_RUN_TEST(dbformat_internal_filter_policy);
  CLDB_RUN_TEST(dbformat_lookup_key);
}
