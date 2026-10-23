#include <stddef.h>

#include "cleveldb/slice.h"
#include "harness.h"

CLDB_TEST(slice_empty) {
  cldb_slice_t empty = cldb_slice_empty();

  CLDB_EXPECT_TRUE(cldb_slice_is_empty(empty));
  CLDB_EXPECT_EQ_UINT(0, cldb_slice_size(empty));
  CLDB_EXPECT_TRUE(cldb_slice_data(empty) != NULL);
  CLDB_EXPECT_EQ_INT(0, cldb_slice_compare(empty, cldb_slice_empty()));
  CLDB_EXPECT_TRUE(cldb_slice_equal(empty, cldb_slice_empty()));
  CLDB_EXPECT_TRUE(cldb_slice_starts_with(empty, cldb_slice_empty()));
}

CLDB_TEST(slice_binary) {
  static const char kBinary[] = {'a', '\0', 'b', (char)0x80};
  cldb_slice_t slice = cldb_slice_make(kBinary, sizeof(kBinary));

  CLDB_EXPECT_EQ_UINT(sizeof(kBinary), cldb_slice_size(slice));
  CLDB_EXPECT_EQ_INT('a', (unsigned char)slice.data[0]);
  CLDB_EXPECT_EQ_INT(0, (unsigned char)slice.data[1]);
  CLDB_EXPECT_EQ_INT('b', (unsigned char)slice.data[2]);
  CLDB_EXPECT_EQ_INT(0x80, (unsigned char)slice.data[3]);
  CLDB_EXPECT_TRUE(cldb_slice_equal(
      slice, cldb_slice_make(kBinary, sizeof(kBinary))));
  CLDB_EXPECT_FALSE(cldb_slice_equal(slice, cldb_slice_make(kBinary, 3)));
}

CLDB_TEST(slice_compare) {
  cldb_slice_t a = cldb_slice_make("abc", 3);
  cldb_slice_t b = cldb_slice_make("abd", 3);
  cldb_slice_t prefix = cldb_slice_make("ab", 2);
  cldb_slice_t empty = cldb_slice_empty();

  CLDB_EXPECT_TRUE(cldb_slice_compare(a, a) == 0);
  CLDB_EXPECT_TRUE(cldb_slice_compare(a, b) < 0);
  CLDB_EXPECT_TRUE(cldb_slice_compare(b, a) > 0);
  CLDB_EXPECT_TRUE(cldb_slice_compare(prefix, a) < 0);
  CLDB_EXPECT_TRUE(cldb_slice_compare(a, prefix) > 0);
  CLDB_EXPECT_TRUE(cldb_slice_compare(empty, a) < 0);
  CLDB_EXPECT_TRUE(cldb_slice_compare(a, empty) > 0);
}

CLDB_TEST(slice_starts_with) {
  static const char kNulPrefixed[] = {'a', 'b', 'c', '\0', 'd'};
  cldb_slice_t slice = cldb_slice_make("abcdef", 6);
  cldb_slice_t binary = cldb_slice_make(kNulPrefixed, sizeof(kNulPrefixed));

  CLDB_EXPECT_TRUE(cldb_slice_starts_with(slice, cldb_slice_make("abc", 3)));
  CLDB_EXPECT_TRUE(cldb_slice_starts_with(slice, cldb_slice_make("", 0)));
  CLDB_EXPECT_TRUE(cldb_slice_starts_with(slice, slice));
  CLDB_EXPECT_FALSE(
      cldb_slice_starts_with(slice, cldb_slice_make("abcdefg", 7)));
  CLDB_EXPECT_FALSE(
      cldb_slice_starts_with(cldb_slice_make("abc", 3), slice));
  CLDB_EXPECT_TRUE(cldb_slice_starts_with(
      binary, cldb_slice_make(kNulPrefixed, 4)));
  CLDB_EXPECT_FALSE(cldb_slice_starts_with(
      slice, cldb_slice_make("abc\0", 4)));
}

CLDB_TEST(slice_remove_prefix) {
  const char *base = "abcdef";
  cldb_slice_t slice = cldb_slice_make(base, 6);

  cldb_slice_remove_prefix(&slice, 0);
  CLDB_EXPECT_EQ_UINT(6, slice.size);
  CLDB_EXPECT_EQ_PTR(base, slice.data);

  cldb_slice_remove_prefix(&slice, 2);
  CLDB_EXPECT_EQ_UINT(4, slice.size);
  CLDB_EXPECT_EQ_PTR(base + 2, slice.data);
  CLDB_EXPECT_TRUE(cldb_slice_equal(slice, cldb_slice_make("cdef", 4)));

  cldb_slice_remove_prefix(&slice, 4);
  CLDB_EXPECT_TRUE(cldb_slice_is_empty(slice));
  CLDB_EXPECT_TRUE(slice.data != NULL);

  cldb_slice_remove_prefix(&slice, 1);
  CLDB_EXPECT_TRUE(cldb_slice_is_empty(slice));
  cldb_slice_remove_prefix(NULL, 1);
}

CLDB_TEST(slice_null_data) {
  cldb_slice_t empty = cldb_slice_make(NULL, 0);
  cldb_slice_t invalid = cldb_slice_make(NULL, 5);
  cldb_slice_t zero_length = cldb_slice_make("abc", 0);

  CLDB_EXPECT_TRUE(cldb_slice_is_empty(empty));
  CLDB_EXPECT_TRUE(cldb_slice_is_empty(invalid));
  CLDB_EXPECT_EQ_UINT(0, zero_length.size);
  CLDB_EXPECT_TRUE(zero_length.data != NULL);
}

void cldb_register_slice_tests(void) {
  CLDB_RUN_TEST(slice_empty);
  CLDB_RUN_TEST(slice_binary);
  CLDB_RUN_TEST(slice_compare);
  CLDB_RUN_TEST(slice_starts_with);
  CLDB_RUN_TEST(slice_remove_prefix);
  CLDB_RUN_TEST(slice_null_data);
}
