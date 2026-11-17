#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cleveldb/options.h"
#include "harness.h"
#include "status_test_util.h"
#include "table/block.h"
#include "table/block_builder.h"
#include "util/coding.h"

typedef struct test_entry {
  const char *key;
  const char *value;
} test_entry_t;

static void init_builder(cldb_block_builder_t *builder, int interval) {
  cldb_options_t options;

  cldb_options_init(&options);
  options.block_restart_interval = interval;
  CLDB_ASSERT_OK(cldb_block_builder_init(builder, &options));
}

static void add_entries(cldb_block_builder_t *builder,
                        const test_entry_t *entries, size_t count) {
  size_t i;

  for (i = 0; i < count; ++i) {
    cldb_slice_t key =
        cldb_slice_make(entries[i].key, strlen(entries[i].key));
    cldb_slice_t value =
        cldb_slice_make(entries[i].value, strlen(entries[i].value));

    CLDB_ASSERT_OK(cldb_block_builder_add(builder, key, value));
  }
}

static void init_borrowed_block(cldb_block_t *block, cldb_slice_t data) {
  cldb_block_contents_t contents = {data, false, false};

  CLDB_ASSERT_OK(cldb_block_init(block, contents));
}

static void expect_entry(const cldb_iterator_t *iterator, const char *key,
                         const char *value) {
  CLDB_ASSERT_TRUE(cldb_iter_valid(iterator));
  CLDB_ASSERT_TRUE(cldb_slice_equal(
      cldb_iter_key(iterator), cldb_slice_make(key, strlen(key))));
  CLDB_ASSERT_TRUE(cldb_slice_equal(
      cldb_iter_value(iterator), cldb_slice_make(value, strlen(value))));
}

/* 按字节序找到第一个 >= target 的下标；count 表示不存在。 */
static size_t lower_bound(const test_entry_t *entries, size_t count,
                          cldb_slice_t target) {
  size_t i;

  for (i = 0; i < count; ++i) {
    cldb_slice_t key =
        cldb_slice_make(entries[i].key, strlen(entries[i].key));

    if (cldb_slice_compare(key, target) >= 0) {
      break;
    }
  }
  return i;
}

static void expect_seek(cldb_iterator_t *iterator,
                        const test_entry_t *entries, size_t count,
                        cldb_slice_t target) {
  size_t expected = lower_bound(entries, count, target);

  cldb_iter_seek(iterator, target);
  CLDB_EXPECT_OK(cldb_iter_status(iterator));
  if (expected == count) {
    CLDB_EXPECT_FALSE(cldb_iter_valid(iterator));
  } else {
    CLDB_ASSERT_TRUE(cldb_iter_valid(iterator));
    expect_entry(iterator, entries[expected].key, entries[expected].value);
  }
}

/* 生成一份可篡改的块副本；builder 的缓冲在拷贝后即可销毁。 */
static char *copy_block_data(int interval, const test_entry_t *entries,
                             size_t count, size_t *size) {
  cldb_block_builder_t builder;
  cldb_slice_t block;
  char *copy;

  init_builder(&builder, interval);
  add_entries(&builder, entries, count);
  CLDB_ASSERT_OK(cldb_block_builder_finish(&builder, &block));
  copy = malloc(block.size);
  CLDB_ASSERT_TRUE(copy != NULL);
  memcpy(copy, block.data, block.size);
  *size = block.size;
  cldb_block_builder_destroy(&builder);
  return copy;
}

static cldb_iterator_t *new_iterator_for_copy(const char *data, size_t size,
                                              cldb_block_t *block) {
  cldb_block_contents_t contents;

  contents.data = cldb_slice_make(data, size);
  contents.cachable = false;
  contents.heap_allocated = false;
  CLDB_ASSERT_OK(cldb_block_init(block, contents));
  return cldb_block_new_iterator(block, NULL);
}

CLDB_TEST(block_roundtrip_order_and_seek) {
  enum { kCount = 20 };
  char key_buffers[kCount][16];
  char value_buffers[kCount][16];
  test_entry_t entries[kCount];
  cldb_block_builder_t builder;
  cldb_block_t block;
  cldb_iterator_t *iterator;
  cldb_slice_t data;
  int i;

  for (i = 0; i < kCount; ++i) {
    (void)snprintf(key_buffers[i], sizeof(key_buffers[i]), "key-%02d", i);
    (void)snprintf(value_buffers[i], sizeof(value_buffers[i]), "v-%02d", i);
    entries[i].key = key_buffers[i];
    entries[i].value = value_buffers[i];
  }

  init_builder(&builder, 4);
  add_entries(&builder, entries, kCount);
  CLDB_ASSERT_OK(cldb_block_builder_finish(&builder, &data));
  init_borrowed_block(&block, data);
  iterator = cldb_block_new_iterator(&block, cldb_bytewise_comparator());
  CLDB_ASSERT_TRUE(iterator != NULL);

  cldb_iter_seek_to_first(iterator);
  for (i = 0; i < kCount; ++i) {
    expect_entry(iterator, entries[i].key, entries[i].value);
    cldb_iter_next(iterator);
  }
  CLDB_EXPECT_FALSE(cldb_iter_valid(iterator));
  CLDB_EXPECT_OK(cldb_iter_status(iterator));

  cldb_iter_seek_to_last(iterator);
  for (i = kCount; i > 0; --i) {
    expect_entry(iterator, entries[i - 1].key, entries[i - 1].value);
    if (i > 1) {
      cldb_iter_prev(iterator);
    }
  }
  cldb_iter_prev(iterator);
  CLDB_EXPECT_FALSE(cldb_iter_valid(iterator));
  CLDB_EXPECT_OK(cldb_iter_status(iterator));

  expect_seek(iterator, entries, kCount, cldb_slice_make("", 0));
  expect_seek(iterator, entries, kCount, cldb_slice_make("key-00", 6));
  expect_seek(iterator, entries, kCount, cldb_slice_make("key-005", 7));
  expect_seek(iterator, entries, kCount, cldb_slice_make("key-19", 6));
  expect_seek(iterator, entries, kCount, cldb_slice_make("key-99", 6));

  /* 连续 Seek 会利用当前位置，覆盖向前、向后跳转。 */
  expect_seek(iterator, entries, kCount, cldb_slice_make("key-02", 6));
  expect_seek(iterator, entries, kCount, cldb_slice_make("key-15", 6));
  expect_seek(iterator, entries, kCount, cldb_slice_make("key-05", 6));
  expect_seek(iterator, entries, kCount, cldb_slice_make("key-19", 6));

  cldb_iter_seek_to_first(iterator);
  CLDB_ASSERT_TRUE(cldb_iter_valid(iterator));
  expect_entry(iterator, entries[0].key, entries[0].value);

  cldb_iter_destroy(iterator);
  iterator = cldb_block_new_iterator(&block, NULL);
  CLDB_ASSERT_TRUE(iterator != NULL);
  cldb_iter_seek_to_first(iterator);
  expect_entry(iterator, entries[0].key, entries[0].value);
  cldb_iter_destroy(iterator);

  cldb_block_destroy(&block);
  cldb_block_builder_destroy(&builder);
}

CLDB_TEST(block_restart_interval_one) {
  static const test_entry_t kEntries[] = {
      {"a", "1"}, {"b", "2"}, {"c", "3"}, {"d", "4"}, {"e", "5"},
  };
  cldb_block_builder_t builder;
  cldb_block_t block;
  cldb_iterator_t *iterator;
  cldb_slice_t data;
  size_t i;

  init_builder(&builder, 1);
  add_entries(&builder, kEntries, sizeof(kEntries) / sizeof(kEntries[0]));
  CLDB_ASSERT_OK(cldb_block_builder_finish(&builder, &data));
  init_borrowed_block(&block, data);
  iterator = cldb_block_new_iterator(&block, NULL);
  CLDB_ASSERT_TRUE(iterator != NULL);

  for (i = 0; i < sizeof(kEntries) / sizeof(kEntries[0]); ++i) {
    expect_seek(iterator, kEntries,
                sizeof(kEntries) / sizeof(kEntries[0]),
                cldb_slice_make(kEntries[i].key, 1));
    expect_entry(iterator, kEntries[i].key, kEntries[i].value);
  }
  cldb_iter_seek_to_last(iterator);
  for (i = sizeof(kEntries) / sizeof(kEntries[0]); i > 0; --i) {
    expect_entry(iterator, kEntries[i - 1].key, kEntries[i - 1].value);
    if (i > 1) {
      cldb_iter_prev(iterator);
    }
  }

  cldb_iter_destroy(iterator);
  cldb_block_destroy(&block);
  cldb_block_builder_destroy(&builder);
}

CLDB_TEST(block_empty_and_malformed_header) {
  cldb_block_builder_t builder;
  cldb_block_t block;
  cldb_iterator_t *iterator;
  cldb_slice_t data;
  char four_zero_bytes[4] = {0};
  char too_many_restarts[8] = {0};
  char short_header[3] = {0};
  cldb_block_contents_t contents;

  init_builder(&builder, 16);
  CLDB_ASSERT_OK(cldb_block_builder_finish(&builder, &data));
  init_borrowed_block(&block, data);
  CLDB_EXPECT_EQ_UINT(1, block.num_restarts);
  CLDB_EXPECT_EQ_UINT(0, block.restart_offset);
  CLDB_EXPECT_EQ_UINT(cldb_slice_size(data), cldb_block_size(&block));
  CLDB_EXPECT_FALSE(cldb_block_is_cachable(&block));

  iterator = cldb_block_new_iterator(&block, cldb_bytewise_comparator());
  CLDB_ASSERT_TRUE(iterator != NULL);
  CLDB_EXPECT_FALSE(cldb_iter_valid(iterator));
  CLDB_EXPECT_OK(cldb_iter_status(iterator));
  cldb_iter_seek_to_first(iterator);
  cldb_iter_seek_to_last(iterator);
  cldb_iter_seek(iterator, cldb_slice_make("x", 1));
  CLDB_EXPECT_FALSE(cldb_iter_valid(iterator));
  CLDB_EXPECT_OK(cldb_iter_status(iterator));
  cldb_iter_destroy(iterator);
  cldb_block_destroy(&block);
  cldb_block_builder_destroy(&builder);

  /* 只有 num_restarts=0 的 4 字节块也是合法空块。 */
  contents.data = cldb_slice_make(four_zero_bytes, sizeof(four_zero_bytes));
  contents.cachable = true;
  contents.heap_allocated = false;
  CLDB_ASSERT_OK(cldb_block_init(&block, contents));
  CLDB_EXPECT_EQ_UINT(4, cldb_block_size(&block));
  CLDB_EXPECT_EQ_UINT(0, block.num_restarts);
  CLDB_EXPECT_EQ_UINT(0, block.restart_offset);
  CLDB_EXPECT_TRUE(cldb_block_is_cachable(&block));
  iterator = cldb_block_new_iterator(&block, NULL);
  CLDB_ASSERT_TRUE(iterator != NULL);
  CLDB_EXPECT_FALSE(cldb_iter_valid(iterator));
  CLDB_EXPECT_OK(cldb_iter_status(iterator));
  cldb_iter_destroy(iterator);
  cldb_block_destroy(&block);

  /* 不足 4 字节：构造标记损坏，NewIterator 返回 Corruption。 */
  contents.data = cldb_slice_make(short_header, sizeof(short_header));
  contents.cachable = false;
  CLDB_ASSERT_OK(cldb_block_init(&block, contents));
  CLDB_EXPECT_EQ_UINT(0, cldb_block_size(&block));
  iterator = cldb_block_new_iterator(&block, NULL);
  CLDB_ASSERT_TRUE(iterator != NULL);
  CLDB_EXPECT_FALSE(cldb_iter_valid(iterator));
  CLDB_EXPECT_STATUS(CLDB_STATUS_CORRUPTION,
                     cldb_iter_status(iterator));
  cldb_iter_destroy(iterator);
  cldb_block_destroy(&block);

  /* num_restarts 超过尾部空间可容纳的数量。 */
  cldb_encode_fixed32(too_many_restarts + 4, 2);
  contents.data = cldb_slice_make(too_many_restarts,
                                  sizeof(too_many_restarts));
  CLDB_ASSERT_OK(cldb_block_init(&block, contents));
  CLDB_EXPECT_EQ_UINT(0, cldb_block_size(&block));
  iterator = cldb_block_new_iterator(&block, NULL);
  CLDB_ASSERT_TRUE(iterator != NULL);
  CLDB_EXPECT_FALSE(cldb_iter_valid(iterator));
  CLDB_EXPECT_STATUS(CLDB_STATUS_CORRUPTION,
                     cldb_iter_status(iterator));
  cldb_iter_destroy(iterator);
  cldb_block_destroy(&block);

  cldb_block_destroy(NULL);
  CLDB_EXPECT_EQ_UINT(0, cldb_block_size(NULL));
  CLDB_EXPECT_FALSE(cldb_block_is_cachable(NULL));
  CLDB_EXPECT_EQ_PTR(NULL, cldb_block_new_iterator(NULL, NULL));
}

CLDB_TEST(block_invalid_contents_and_ownership) {
  cldb_block_t block;
  cldb_slice_t invalid = {NULL, 1};
  cldb_block_contents_t contents;
  char *heap_data;

  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_block_init(NULL, (cldb_block_contents_t){invalid,
                                                                   false,
                                                                   false}));
  contents.data = invalid;
  contents.cachable = false;
  contents.heap_allocated = false;
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_block_init(&block, contents));
  cldb_block_destroy(&block);

  heap_data = malloc(4);
  CLDB_ASSERT_TRUE(heap_data != NULL);
  memset(heap_data, 0, 4);
  contents.data = cldb_slice_make(heap_data, 4);
  contents.cachable = true;
  contents.heap_allocated = true;
  CLDB_ASSERT_OK(cldb_block_init(&block, contents));
  CLDB_EXPECT_TRUE(cldb_block_is_cachable(&block));
  cldb_block_destroy(&block);
}

/* 嵌入 NUL 的二进制 key/value 和 128 字节公共前缀的往返。 */
CLDB_TEST(block_binary_and_long_prefix) {
  enum { kPrefixSize = 128, kCount = 4 };
  char keys[kCount][kPrefixSize + 2];
  char values[kCount][3];
  cldb_slice_t key_slices[kCount];
  cldb_slice_t value_slices[kCount];
  cldb_block_builder_t builder;
  cldb_block_t block;
  cldb_iterator_t *iterator;
  cldb_slice_t data;
  cldb_slice_t prefix;
  size_t i;

  for (i = 0; i < kCount; ++i) {
    memset(keys[i], 'p', kPrefixSize);
    keys[i][kPrefixSize] = (char)i;
    key_slices[i] = cldb_slice_make(keys[i], kPrefixSize + 1);
    values[i][0] = (char)(0xffU - (unsigned)i);
    values[i][1] = '\0';
    value_slices[i] = cldb_slice_make(values[i], 2);
  }

  init_builder(&builder, 2);
  for (i = 0; i < kCount; ++i) {
    CLDB_ASSERT_OK(
        cldb_block_builder_add(&builder, key_slices[i], value_slices[i]));
  }
  CLDB_ASSERT_OK(cldb_block_builder_finish(&builder, &data));
  init_borrowed_block(&block, data);
  iterator = cldb_block_new_iterator(&block, NULL);
  CLDB_ASSERT_TRUE(iterator != NULL);

  cldb_iter_seek_to_first(iterator);
  for (i = 0; i < kCount; ++i) {
    CLDB_ASSERT_TRUE(cldb_iter_valid(iterator));
    CLDB_ASSERT_TRUE(
        cldb_slice_equal(cldb_iter_key(iterator), key_slices[i]));
    CLDB_ASSERT_TRUE(
        cldb_slice_equal(cldb_iter_value(iterator), value_slices[i]));
    cldb_iter_next(iterator);
  }
  CLDB_EXPECT_FALSE(cldb_iter_valid(iterator));
  CLDB_EXPECT_OK(cldb_iter_status(iterator));

  cldb_iter_seek_to_last(iterator);
  for (i = kCount; i > 0; --i) {
    CLDB_ASSERT_TRUE(cldb_iter_valid(iterator));
    CLDB_ASSERT_TRUE(
        cldb_slice_equal(cldb_iter_key(iterator), key_slices[i - 1]));
    CLDB_ASSERT_TRUE(
        cldb_slice_equal(cldb_iter_value(iterator), value_slices[i - 1]));
    if (i > 1) {
      cldb_iter_prev(iterator);
    }
  }

  prefix = cldb_slice_make(keys[0], kPrefixSize);
  cldb_iter_seek(iterator, prefix);
  CLDB_ASSERT_TRUE(cldb_iter_valid(iterator));
  CLDB_ASSERT_TRUE(
      cldb_slice_equal(cldb_iter_key(iterator), key_slices[0]));

  cldb_iter_seek(iterator, key_slices[2]);
  CLDB_ASSERT_TRUE(cldb_iter_valid(iterator));
  CLDB_ASSERT_TRUE(
      cldb_slice_equal(cldb_iter_key(iterator), key_slices[2]));

  cldb_iter_destroy(iterator);
  cldb_block_destroy(&block);
  cldb_block_builder_destroy(&builder);
}

CLDB_TEST(block_random_seek_matches_lower_bound) {
  enum { kCount = 37, kQueries = 200 };
  char key_buffers[kCount][16];
  char value_buffers[kCount][16];
  test_entry_t entries[kCount];
  cldb_block_builder_t builder;
  cldb_block_t block;
  cldb_iterator_t *iterator;
  cldb_slice_t data;
  uint32_t state = UINT32_C(0x12345678);
  int i;

  for (i = 0; i < kCount; ++i) {
    (void)snprintf(key_buffers[i], sizeof(key_buffers[i]), "key-%03d", i);
    (void)snprintf(value_buffers[i], sizeof(value_buffers[i]), "value-%03d",
                   i);
    entries[i].key = key_buffers[i];
    entries[i].value = value_buffers[i];
  }

  init_builder(&builder, 5);
  add_entries(&builder, entries, kCount);
  CLDB_ASSERT_OK(cldb_block_builder_finish(&builder, &data));
  init_borrowed_block(&block, data);
  iterator = cldb_block_new_iterator(&block, NULL);
  CLDB_ASSERT_TRUE(iterator != NULL);

  for (i = 0; i < kQueries; ++i) {
    char target_buffer[16];

    state = (state * UINT32_C(1103515245) + UINT32_C(12345)) &
            UINT32_C(0x7fffffff);
    (void)snprintf(target_buffer, sizeof(target_buffer), "key-%03u",
                   (unsigned)(state % 50U));
    expect_seek(iterator, entries, kCount,
                cldb_slice_make(target_buffer, strlen(target_buffer)));
  }

  cldb_iter_destroy(iterator);
  cldb_block_destroy(&block);
  cldb_block_builder_destroy(&builder);
}

CLDB_TEST(block_out_of_range_restart_is_corruption) {
  static const test_entry_t kEntries[] = {{"a", "1"}};
  cldb_block_t block;
  cldb_iterator_t *iterator;
  char *bytes;
  size_t size;
  size_t restart_offset;

  bytes = copy_block_data(16, kEntries, 1, &size);
  iterator = new_iterator_for_copy(bytes, size, &block);
  CLDB_ASSERT_TRUE(iterator != NULL);
  restart_offset = block.restart_offset;
  CLDB_ASSERT_TRUE(restart_offset > 0);
  cldb_encode_fixed32(bytes + restart_offset, (uint32_t)restart_offset);

  cldb_iter_seek_to_first(iterator);
  CLDB_EXPECT_FALSE(cldb_iter_valid(iterator));
  CLDB_EXPECT_STATUS(CLDB_STATUS_CORRUPTION,
                     cldb_iter_status(iterator));
  cldb_iter_destroy(iterator);
  cldb_block_destroy(&block);
  free(bytes);
}

CLDB_TEST(block_bad_entry_is_corruption) {
  static const test_entry_t kEmptyEntry[] = {{"", ""}};
  static const test_entry_t kOneEntry[] = {{"k", "v"}};
  cldb_block_t block;
  cldb_iterator_t *iterator;
  cldb_status_t status;
  char *bytes;
  size_t size;

  /* 三个续位字节让 varint 永远解析不完。 */
  bytes = copy_block_data(16, kEmptyEntry, 1, &size);
  iterator = new_iterator_for_copy(bytes, size, &block);
  CLDB_ASSERT_TRUE(iterator != NULL);
  CLDB_ASSERT_EQ_UINT(3, block.restart_offset);
  bytes[0] = (char)0x80;
  bytes[1] = (char)0x80;
  bytes[2] = (char)0x80;
  cldb_iter_seek_to_first(iterator);
  CLDB_EXPECT_FALSE(cldb_iter_valid(iterator));
  CLDB_EXPECT_STATUS(CLDB_STATUS_CORRUPTION,
                     cldb_iter_status(iterator));
  cldb_iter_destroy(iterator);
  cldb_block_destroy(&block);
  free(bytes);

  /* 首个条目的 shared 超过此前重建的 key 长度。 */
  bytes = copy_block_data(16, kOneEntry, 1, &size);
  iterator = new_iterator_for_copy(bytes, size, &block);
  CLDB_ASSERT_TRUE(iterator != NULL);
  bytes[0] = 1;
  cldb_iter_seek_to_first(iterator);
  CLDB_EXPECT_FALSE(cldb_iter_valid(iterator));
  CLDB_EXPECT_STATUS(CLDB_STATUS_CORRUPTION,
                     cldb_iter_status(iterator));
  cldb_iter_destroy(iterator);
  cldb_block_destroy(&block);
  free(bytes);

  /* value_length 超过条目区剩余空间。 */
  bytes = copy_block_data(16, kOneEntry, 1, &size);
  iterator = new_iterator_for_copy(bytes, size, &block);
  CLDB_ASSERT_TRUE(iterator != NULL);
  bytes[2] = (char)200;
  cldb_iter_seek_to_first(iterator);
  CLDB_EXPECT_FALSE(cldb_iter_valid(iterator));
  status = cldb_iter_status(iterator);
  CLDB_EXPECT_STATUS(CLDB_STATUS_CORRUPTION, status);
  CLDB_EXPECT_TRUE(strstr(cldb_status_message(&status),
                          "bad entry in block") != NULL);
  cldb_iter_destroy(iterator);
  cldb_block_destroy(&block);
  free(bytes);
}

CLDB_TEST(block_nonzero_shared_at_restart_is_corruption) {
  static const test_entry_t kEntries[] = {{"a", "1"}, {"b", "2"}};
  cldb_block_t block;
  cldb_iterator_t *iterator;
  char *bytes;
  size_t size;
  uint32_t second_restart;

  bytes = copy_block_data(1, kEntries, 2, &size);
  iterator = new_iterator_for_copy(bytes, size, &block);
  CLDB_ASSERT_TRUE(iterator != NULL);
  second_restart =
      cldb_decode_fixed32(bytes + block.restart_offset + sizeof(uint32_t));
  CLDB_ASSERT_TRUE(second_restart < block.restart_offset);
  bytes[second_restart] = 1; /* restart 点必须 shared=0 */

  /* 二分 restart 时会解码第二个 restart 的完整 key。 */
  cldb_iter_seek(iterator, cldb_slice_make("a", 1));
  CLDB_EXPECT_FALSE(cldb_iter_valid(iterator));
  CLDB_EXPECT_STATUS(CLDB_STATUS_CORRUPTION,
                     cldb_iter_status(iterator));
  cldb_iter_destroy(iterator);
  cldb_block_destroy(&block);
  free(bytes);
}

void cldb_register_block_tests(void) {
  CLDB_RUN_TEST(block_roundtrip_order_and_seek);
  CLDB_RUN_TEST(block_restart_interval_one);
  CLDB_RUN_TEST(block_empty_and_malformed_header);
  CLDB_RUN_TEST(block_invalid_contents_and_ownership);
  CLDB_RUN_TEST(block_binary_and_long_prefix);
  CLDB_RUN_TEST(block_random_seek_matches_lower_bound);
  CLDB_RUN_TEST(block_out_of_range_restart_is_corruption);
  CLDB_RUN_TEST(block_bad_entry_is_corruption);
  CLDB_RUN_TEST(block_nonzero_shared_at_restart_is_corruption);
}
