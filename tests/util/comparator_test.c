#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "cleveldb/comparator.h"
#include "harness.h"

enum { kTestKeyCapacity = 64 };

/* 只比较符号；memcmp 的具体数值依实现而定。 */
static int sign_of(int value) { return (value > 0) - (value < 0); }

/* 同时验证 compare(a,b) 与反序，确保反对称性。 */
static void expect_compare(const cldb_comparator_t *comparator, const char *a,
                           size_t a_size, const char *b, size_t b_size,
                           int expected_sign) {
  cldb_slice_t left = cldb_slice_make(a, a_size);
  cldb_slice_t right = cldb_slice_make(b, b_size);

  CLDB_EXPECT_EQ_INT(expected_sign,
                     sign_of(cldb_comparator_compare(comparator, left, right)));
  CLDB_EXPECT_EQ_INT(-expected_sign,
                     sign_of(cldb_comparator_compare(comparator, right, left)));
}

/* separator 的验收同时检查预期值、不增长和 [start, limit) 性质。 */
static void expect_separator(const char *start, size_t start_size,
                             const char *limit, size_t limit_size,
                             const char *expected, size_t expected_size) {
  const cldb_comparator_t *comparator = cldb_bytewise_comparator();
  cldb_slice_t start_slice = cldb_slice_make(start, start_size);
  cldb_slice_t limit_slice = cldb_slice_make(limit, limit_size);
  char buffer[kTestKeyCapacity];
  size_t size = start_size;
  cldb_slice_t result;

  CLDB_ASSERT_TRUE(start_size <= sizeof(buffer));
  CLDB_ASSERT_TRUE(expected_size <= sizeof(buffer));
  CLDB_ASSERT_TRUE(cldb_comparator_compare(comparator, start_slice,
                                           limit_slice) < 0);
  memcpy(buffer, start, start_size);
  cldb_comparator_find_shortest_separator(comparator, buffer, &size,
                                          limit_slice);
  CLDB_EXPECT_TRUE(size <= start_size);
  CLDB_EXPECT_EQ_UINT(expected_size, size);
  if (size == expected_size) {
    CLDB_EXPECT_TRUE(size == 0 || memcmp(buffer, expected, size) == 0);
  }

  result = cldb_slice_make(buffer, size);
  CLDB_EXPECT_TRUE(cldb_comparator_compare(comparator, start_slice, result) <=
                   0);
  CLDB_EXPECT_TRUE(cldb_comparator_compare(comparator, result, limit_slice) <
                   0);
}

/* successor 的验收同时检查预期值、不增长和结果 >= 原 key。 */
static void expect_successor(const char *key, size_t key_size,
                             const char *expected, size_t expected_size) {
  const cldb_comparator_t *comparator = cldb_bytewise_comparator();
  cldb_slice_t original = cldb_slice_make(key, key_size);
  char buffer[kTestKeyCapacity];
  size_t size = key_size;
  cldb_slice_t result;

  CLDB_ASSERT_TRUE(key_size <= sizeof(buffer));
  CLDB_ASSERT_TRUE(expected_size <= sizeof(buffer));
  memcpy(buffer, key, key_size);
  cldb_comparator_find_short_successor(comparator, buffer, &size);
  CLDB_EXPECT_TRUE(size <= key_size);
  CLDB_EXPECT_EQ_UINT(expected_size, size);
  if (size == expected_size) {
    CLDB_EXPECT_TRUE(size == 0 || memcmp(buffer, expected, size) == 0);
  }

  result = cldb_slice_make(buffer, size);
  CLDB_EXPECT_TRUE(cldb_comparator_compare(comparator, original, result) <= 0);
}

static const char kByte00[] = {0x00};
static const char kByte01[] = {0x01};
static const char kByte02[] = {0x02};
static const char kByte03[] = {0x03};
static const char kByte7f[] = {(char)0x7f};
static const char kByte80[] = {(char)0x80};
static const char kByte81[] = {(char)0x81};
static const char kByteFe[] = {(char)0xfe};
static const char kByteFf[] = {(char)0xff};
static const char kFf00[] = {(char)0xff, 0x00};
static const char kFf01[] = {(char)0xff, 0x01};
static const char kFfA[] = {(char)0xff, 'a'};
static const char kFfB[] = {(char)0xff, 'b'};
static const char kFfC[] = {(char)0xff, 'c'};
static const char kFfFf[] = {(char)0xff, (char)0xff};
static const char kFfFf00[] = {(char)0xff, (char)0xff, 0x00};
static const char kFfFf01[] = {(char)0xff, (char)0xff, 0x01};
static const char kFfFe00[] = {(char)0xff, (char)0xfe, 0x00};
static const char kFeZz[] = {(char)0xfe, 'z', 'z'};
static const char kAbcFe[] = {'a', 'b', 'c', (char)0xfe};
static const char kAbcF[] = {'a', 'b', 'f'};
static const char kAbd[] = {'a', 'b', 'd'};

CLDB_TEST(comparator_bytewise_name) {
  const cldb_comparator_t *comparator = cldb_bytewise_comparator();
  const char *expected = "leveldb.BytewiseComparator";
  const char *name;

  CLDB_ASSERT_TRUE(comparator != NULL);
  CLDB_ASSERT_TRUE(comparator->vtbl != NULL);
  CLDB_EXPECT_EQ_PTR(comparator, cldb_bytewise_comparator());
  CLDB_EXPECT_TRUE(comparator->vtbl->compare != NULL);
  CLDB_EXPECT_TRUE(comparator->vtbl->name != NULL);
  CLDB_EXPECT_TRUE(comparator->vtbl->find_shortest_separator != NULL);
  CLDB_EXPECT_TRUE(comparator->vtbl->find_short_successor != NULL);

  name = cldb_comparator_name(comparator);
  CLDB_ASSERT_TRUE(name != NULL);
  CLDB_EXPECT_EQ_PTR(name, cldb_comparator_name(comparator));
  CLDB_EXPECT_EQ_UINT(sizeof("leveldb.BytewiseComparator") - 1,
                      strlen(name));
  CLDB_EXPECT_EQ_INT(
      0, memcmp(name, expected, sizeof("leveldb.BytewiseComparator")));
}

CLDB_TEST(comparator_bytewise_compare) {
  const cldb_comparator_t *comparator = cldb_bytewise_comparator();
  static const char kA0B[] = {'a', '\0', 'b'};
  static const char kA0A[] = {'a', '\0', 'a'};
  static const char kA0[] = {'a', '\0'};

  expect_compare(comparator, "", 0, "", 0, 0);
  expect_compare(comparator, "", 0, "a", 1, -1);
  expect_compare(comparator, "a", 1, "", 0, 1);
  expect_compare(comparator, "a", 1, "a", 1, 0);
  expect_compare(comparator, "a", 1, "ab", 2, -1);
  expect_compare(comparator, "ab", 2, "a", 1, 1);
  expect_compare(comparator, "a", 1, "b", 1, -1);
  expect_compare(comparator, "abc", 3, "abd", 3, -1);
  expect_compare(comparator, kByte00, 1, kByte01, 1, -1);
  expect_compare(comparator, kByte7f, 1, kByte80, 1, -1);
  expect_compare(comparator, kByteFf, 1, kByte7f, 1, 1);
  expect_compare(comparator, kA0B, 3, kA0A, 3, 1);
  expect_compare(comparator, kA0, 2, "a", 1, 1);
}

CLDB_TEST(comparator_bytewise_separator) {
  size_t empty_size = 0;

  /* 空缓冲允许 start == NULL；实现不得解引用。 */
  cldb_comparator_find_shortest_separator(cldb_bytewise_comparator(), NULL,
                                          &empty_size,
                                          cldb_slice_make("a", 1));
  CLDB_EXPECT_EQ_UINT(0, empty_size);

  expect_separator("", 0, "a", 1, "", 0);
  expect_separator("foo", 3, "foobar", 6, "foo", 3);
  expect_separator("foo", 3, "fop", 3, "foo", 3);
  expect_separator("foo", 3, "foq", 3, "fop", 3);
  expect_separator("foozzz", 6, "fpr", 3, "foozzz", 6);
  expect_separator("foozzz", 6, "fqs", 3, "fp", 2);
  expect_separator("abcd", 4, "abcf", 4, "abce", 4);
  expect_separator("abczzz", 6, "abf", 3, "abd", 3);
  expect_separator("abc", 3, "abd", 3, "abc", 3);
  expect_separator("abc", 3, "ad", 2, "ac", 2);
  expect_separator("abz", 3, "ac", 2, "abz", 3);
  expect_separator("a", 1, "b", 1, "a", 1);
  expect_separator("a", 1, "c", 1, "b", 1);

  expect_separator(kByte00, 1, kByte02, 1, kByte01, 1);
  expect_separator(kByte00, 1, kByte01, 1, kByte00, 1);
  expect_separator(kByte7f, 1, kByte81, 1, kByte80, 1);
  expect_separator(kByte7f, 1, kByte80, 1, kByte7f, 1);
  expect_separator(kByteFe, 1, kByteFf, 1, kByteFe, 1);
  expect_separator(kFfA, 2, kFfC, 2, kFfB, 2);
  expect_separator(kAbcFe, 4, kAbcF, 3, kAbd, 3);
  expect_separator(kByte01, 1, kByte03, 1, kByte02, 1);
}

CLDB_TEST(comparator_bytewise_successor) {
  size_t empty_size = 0;

  /* 空 key 同样允许 key == NULL。 */
  cldb_comparator_find_short_successor(cldb_bytewise_comparator(), NULL,
                                       &empty_size);
  CLDB_EXPECT_EQ_UINT(0, empty_size);

  expect_successor("", 0, "", 0);
  expect_successor("a", 1, "b", 1);
  expect_successor("foo", 3, "g", 1);
  expect_successor("abc", 3, "b", 1);
  expect_successor(kByte00, 1, kByte01, 1);
  expect_successor(kByte7f, 1, kByte80, 1);
  expect_successor(kByteFf, 1, kByteFf, 1);
  expect_successor(kFfFf, 2, kFfFf, 2);
  expect_successor(kFf00, 2, kFf01, 2);
  expect_successor(kFfFf00, 3, kFfFf01, 3);
  expect_successor(kFfFe00, 3, kFfFf, 2);
  expect_successor(kFfA, 2, kFfB, 2);
  expect_successor(kFeZz, 3, kByteFf, 1);
}

typedef struct {
  cldb_comparator_t base; /* 必须为首成员，回调可转回外层类型 */
  bool reverse;
} test_comparator_t;

static int test_compare(const cldb_comparator_t *comparator, cldb_slice_t a,
                        cldb_slice_t b) {
  const test_comparator_t *self = (const test_comparator_t *)comparator;

  return self->reverse ? cldb_slice_compare(b, a) : cldb_slice_compare(a, b);
}

static const char *test_name(const cldb_comparator_t *comparator) {
  (void)comparator;
  return "test.ReverseComparator";
}

static void test_find_shortest_separator(const cldb_comparator_t *comparator,
                                         char *start, size_t *start_size,
                                         cldb_slice_t limit) {
  (void)comparator;
  (void)start;
  (void)start_size;
  (void)limit;
}

static void test_find_short_successor(const cldb_comparator_t *comparator,
                                      char *key, size_t *key_size) {
  (void)comparator;
  (void)key;
  (void)key_size;
}

static const cldb_comparator_vtbl_t kTestVtbl = {
    .compare = test_compare,
    .name = test_name,
    .find_shortest_separator = test_find_shortest_separator,
    .find_short_successor = test_find_short_successor,
};

CLDB_TEST(comparator_custom_vtable) {
  test_comparator_t custom;
  /* 反序比较器下 "abc" < "abb"，满足 separator 的前置条件。 */
  cldb_slice_t limit = cldb_slice_make("abb", 3);
  char start[] = "abc";
  char key[] = "xyz";
  size_t start_size = 3;
  size_t key_size = 3;

  custom.base.vtbl = &kTestVtbl;
  custom.reverse = true;

  expect_compare(&custom.base, "a", 1, "b", 1, 1);
  CLDB_EXPECT_EQ_STR("test.ReverseComparator",
                     cldb_comparator_name(&custom.base));

  /* 简单自定义实现允许 separator/successor 原样返回，vtable 转发仍生效。 */
  cldb_comparator_find_shortest_separator(&custom.base, start, &start_size,
                                          limit);
  CLDB_EXPECT_EQ_UINT(3, start_size);
  CLDB_EXPECT_TRUE(start_size == 3 && memcmp(start, "abc", 3) == 0);
  cldb_comparator_find_short_successor(&custom.base, key, &key_size);
  CLDB_EXPECT_EQ_UINT(3, key_size);
  CLDB_EXPECT_TRUE(key_size == 3 && memcmp(key, "xyz", 3) == 0);
}

void cldb_register_comparator_tests(void) {
  CLDB_RUN_TEST(comparator_bytewise_name);
  CLDB_RUN_TEST(comparator_bytewise_compare);
  CLDB_RUN_TEST(comparator_bytewise_separator);
  CLDB_RUN_TEST(comparator_bytewise_successor);
  CLDB_RUN_TEST(comparator_custom_vtable);
}
