#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "cleveldb/buf.h"
#include "harness.h"

static void expect_content(const cldb_buf_t *buf, const void *data,
                           size_t size) {
  CLDB_EXPECT_EQ_UINT(size, buf->size);
  if (size > 0) {
    CLDB_EXPECT_TRUE(buf->data != NULL);
  }
  if (buf->data != NULL && buf->size < buf->capacity) {
    CLDB_EXPECT_EQ_INT(0, (unsigned char)buf->data[buf->size]);
    if (buf->size == size && size > 0) {
      CLDB_EXPECT_EQ_INT(0, memcmp(buf->data, data, size));
    }
  }
}

CLDB_TEST(buf_init_free) {
  cldb_buf_t buf;

  cldb_buf_init(&buf);
  CLDB_EXPECT_EQ_PTR(NULL, buf.data);
  CLDB_EXPECT_EQ_UINT(0, buf.size);
  CLDB_EXPECT_EQ_UINT(0, buf.capacity);
  CLDB_EXPECT_EQ_STR("", cldb_buf_cstr(&buf));
  CLDB_EXPECT_TRUE(cldb_slice_is_empty(cldb_buf_slice(&buf)));

  cldb_buf_free(&buf);
  CLDB_EXPECT_EQ_PTR(NULL, buf.data);
  CLDB_EXPECT_EQ_UINT(0, buf.size);

  cldb_buf_init(NULL);
  cldb_buf_free(NULL);
  cldb_buf_clear(NULL);
  CLDB_EXPECT_EQ_STR("", cldb_buf_cstr(NULL));
  CLDB_EXPECT_TRUE(cldb_slice_is_empty(cldb_buf_slice(NULL)));
}

CLDB_TEST(buf_append_binary) {
  static const char kBinary[] = {'x', '\0', 'y'};
  cldb_buf_t buf;

  cldb_buf_init(&buf);
  CLDB_EXPECT_TRUE(cldb_buf_append(&buf, kBinary, sizeof(kBinary)));
  expect_content(&buf, kBinary, sizeof(kBinary));
  CLDB_EXPECT_EQ_STR("x", cldb_buf_cstr(&buf));
  CLDB_EXPECT_TRUE(buf.capacity >= buf.size + 1);
  CLDB_EXPECT_EQ_UINT(sizeof(kBinary), cldb_buf_slice(&buf).size);
  cldb_buf_free(&buf);
}

CLDB_TEST(buf_append_char) {
  cldb_buf_t buf;

  cldb_buf_init(&buf);
  CLDB_EXPECT_TRUE(cldb_buf_append_char(&buf, 'a'));
  CLDB_EXPECT_TRUE(cldb_buf_append_char(&buf, 'b'));
  CLDB_EXPECT_TRUE(cldb_buf_append_char(&buf, '\0'));
  CLDB_EXPECT_EQ_UINT(3, buf.size);
  CLDB_EXPECT_EQ_STR("ab", cldb_buf_cstr(&buf));
  CLDB_EXPECT_EQ_INT(0, (unsigned char)buf.data[2]);
  CLDB_EXPECT_EQ_INT(0, (unsigned char)buf.data[3]);
  cldb_buf_free(&buf);
}

CLDB_TEST(buf_append_slice) {
  cldb_buf_t buf;
  cldb_slice_t slice = cldb_slice_make("XYZ", 3);

  cldb_buf_init(&buf);
  CLDB_EXPECT_TRUE(cldb_buf_append_slice(&buf, slice));
  expect_content(&buf, "XYZ", 3);
  CLDB_EXPECT_TRUE(cldb_buf_append_slice(&buf, cldb_slice_empty()));
  expect_content(&buf, "XYZ", 3);
  cldb_buf_free(&buf);
}

CLDB_TEST(buf_reserve_resize) {
  static const char kZeros[4] = {0, 0, 0, 0};
  cldb_buf_t buf;
  size_t capacity;

  cldb_buf_init(&buf);
  CLDB_EXPECT_TRUE(cldb_buf_reserve(&buf, 8));
  CLDB_EXPECT_TRUE(buf.capacity >= 8);
  CLDB_EXPECT_EQ_UINT(0, buf.size);
  CLDB_EXPECT_TRUE(buf.data != NULL);
  CLDB_EXPECT_EQ_INT(0, (unsigned char)buf.data[0]);

  CLDB_EXPECT_TRUE(cldb_buf_resize(&buf, 4));
  expect_content(&buf, kZeros, sizeof(kZeros));

  capacity = buf.capacity;
  CLDB_EXPECT_TRUE(cldb_buf_resize(&buf, 2));
  CLDB_EXPECT_EQ_UINT(2, buf.size);
  CLDB_EXPECT_EQ_INT(0, (unsigned char)buf.data[2]);
  CLDB_EXPECT_TRUE(cldb_buf_resize(&buf, 0));
  CLDB_EXPECT_EQ_UINT(0, buf.size);
  CLDB_EXPECT_EQ_INT(0, (unsigned char)buf.data[0]);
  CLDB_EXPECT_EQ_UINT(capacity, buf.capacity);

  CLDB_EXPECT_FALSE(cldb_buf_resize(&buf, SIZE_MAX));
  CLDB_EXPECT_EQ_UINT(0, buf.size);
  cldb_buf_free(&buf);
}

CLDB_TEST(buf_growth_keeps_nul) {
  char fill[15];
  cldb_buf_t buf;

  memset(fill, 'q', sizeof(fill));
  cldb_buf_init(&buf);
  CLDB_EXPECT_TRUE(cldb_buf_append(&buf, fill, sizeof(fill)));
  CLDB_EXPECT_EQ_UINT(15, buf.size);
  CLDB_EXPECT_TRUE(buf.capacity >= 16);
  CLDB_EXPECT_EQ_INT(0, (unsigned char)buf.data[15]);

  CLDB_EXPECT_TRUE(cldb_buf_append_char(&buf, 'z'));
  CLDB_EXPECT_EQ_UINT(16, buf.size);
  CLDB_EXPECT_TRUE(buf.capacity >= 17);
  CLDB_EXPECT_EQ_INT('z', (unsigned char)buf.data[15]);
  CLDB_EXPECT_EQ_INT(0, (unsigned char)buf.data[16]);
  cldb_buf_free(&buf);
}

CLDB_TEST(buf_assign) {
  static const char kBinary[] = {'a', '\0', 'b'};
  cldb_buf_t buf;

  cldb_buf_init(&buf);
  CLDB_EXPECT_TRUE(cldb_buf_append(&buf, "old", 3));
  CLDB_EXPECT_TRUE(cldb_buf_assign(&buf, "new", 3));
  expect_content(&buf, "new", 3);
  CLDB_EXPECT_TRUE(cldb_buf_assign(&buf, kBinary, sizeof(kBinary)));
  expect_content(&buf, kBinary, sizeof(kBinary));
  CLDB_EXPECT_TRUE(cldb_buf_assign(&buf, NULL, 0));
  CLDB_EXPECT_EQ_UINT(0, buf.size);
  CLDB_EXPECT_EQ_STR("", cldb_buf_cstr(&buf));
  CLDB_EXPECT_FALSE(cldb_buf_assign(&buf, NULL, 1));
  cldb_buf_free(&buf);
}

CLDB_TEST(buf_self_append) {
  char fill[15];
  char expected[30];
  cldb_buf_t buf;
  cldb_slice_t own;

  cldb_buf_init(&buf);
  CLDB_EXPECT_TRUE(cldb_buf_append(&buf, "abc", 3));
  own = cldb_buf_slice(&buf);
  CLDB_EXPECT_TRUE(cldb_buf_append_slice(&buf, own));
  expect_content(&buf, "abcabc", 6);

  CLDB_EXPECT_TRUE(cldb_buf_assign(&buf, cldb_buf_cstr(&buf) + 1, 3));
  expect_content(&buf, "bca", 3);

  memset(fill, 'r', sizeof(fill));
  CLDB_EXPECT_TRUE(cldb_buf_assign(&buf, fill, sizeof(fill)));
  own = cldb_buf_slice(&buf);
  CLDB_EXPECT_TRUE(cldb_buf_append_slice(&buf, own));
  memset(expected, 'r', sizeof(expected));
  expect_content(&buf, expected, sizeof(expected));

  own = cldb_slice_make(cldb_buf_cstr(&buf) + 1, 3);
  CLDB_EXPECT_TRUE(cldb_buf_append_slice(&buf, own));
  CLDB_EXPECT_EQ_UINT(33, buf.size);
  CLDB_EXPECT_EQ_INT('r', (unsigned char)buf.data[30]);
  CLDB_EXPECT_EQ_INT('r', (unsigned char)buf.data[31]);
  CLDB_EXPECT_EQ_INT('r', (unsigned char)buf.data[32]);
  cldb_buf_free(&buf);
}

CLDB_TEST(buf_self_append_realloc_offset) {
  static const char kBase[] = "0123456789abcde";
  cldb_buf_t buf;
  cldb_slice_t own;

  cldb_buf_init(&buf);
  CLDB_EXPECT_TRUE(cldb_buf_append(&buf, kBase, sizeof(kBase) - 1));
  CLDB_EXPECT_EQ_UINT(16, buf.capacity);

  own = cldb_slice_make(cldb_buf_cstr(&buf) + 12, 3);
  CLDB_EXPECT_TRUE(cldb_buf_append_slice(&buf, own));
  CLDB_EXPECT_TRUE(buf.capacity > 16);
  expect_content(&buf, "0123456789abcdecde", 18);
  cldb_buf_free(&buf);
}

CLDB_TEST(buf_swap) {
  cldb_buf_t a;
  cldb_buf_t b;

  cldb_buf_init(&a);
  cldb_buf_init(&b);
  CLDB_EXPECT_TRUE(cldb_buf_append(&a, "A", 1));
  CLDB_EXPECT_TRUE(cldb_buf_append(&b, "B", 1));
  cldb_buf_swap(&a, &b);
  expect_content(&a, "B", 1);
  expect_content(&b, "A", 1);
  cldb_buf_swap(&a, &a);
  cldb_buf_swap(&a, NULL);
  cldb_buf_swap(NULL, NULL);
  cldb_buf_free(&a);
  cldb_buf_free(&b);
}

CLDB_TEST(buf_failures) {
  cldb_buf_t buf;

  cldb_buf_init(&buf);
  CLDB_EXPECT_TRUE(cldb_buf_append(&buf, "ok", 2));
  CLDB_EXPECT_FALSE(cldb_buf_append(NULL, "x", 1));
  CLDB_EXPECT_FALSE(cldb_buf_append(&buf, NULL, 1));
  CLDB_EXPECT_TRUE(cldb_buf_append(&buf, NULL, 0));
  CLDB_EXPECT_FALSE(cldb_buf_append_slice(NULL, cldb_slice_empty()));
  CLDB_EXPECT_FALSE(cldb_buf_append_char(NULL, 'x'));
  CLDB_EXPECT_FALSE(cldb_buf_reserve(NULL, 1));
  CLDB_EXPECT_FALSE(cldb_buf_resize(NULL, 1));
  CLDB_EXPECT_FALSE(cldb_buf_assign(NULL, "x", 1));
  CLDB_EXPECT_FALSE(cldb_buf_assign(&buf, NULL, 1));
  CLDB_EXPECT_FALSE(cldb_buf_append(&buf, "x", SIZE_MAX));
  CLDB_EXPECT_FALSE(cldb_buf_resize(&buf, SIZE_MAX));
  expect_content(&buf, "ok", 2);
  cldb_buf_free(&buf);
}

void cldb_register_buf_tests(void) {
  CLDB_RUN_TEST(buf_init_free);
  CLDB_RUN_TEST(buf_append_binary);
  CLDB_RUN_TEST(buf_append_char);
  CLDB_RUN_TEST(buf_append_slice);
  CLDB_RUN_TEST(buf_reserve_resize);
  CLDB_RUN_TEST(buf_growth_keeps_nul);
  CLDB_RUN_TEST(buf_assign);
  CLDB_RUN_TEST(buf_self_append);
  CLDB_RUN_TEST(buf_self_append_realloc_offset);
  CLDB_RUN_TEST(buf_swap);
  CLDB_RUN_TEST(buf_failures);
}
