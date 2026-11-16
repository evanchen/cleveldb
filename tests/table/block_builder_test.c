#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "cleveldb/options.h"
#include "cleveldb/slice.h"
#include "harness.h"
#include "status_test_util.h"
#include "table/block_builder.h"

/* 字节布局期望值由上游 BlockBuilder 对相同条目生成，覆盖空块、单条、
 * restart 边界、前缀压缩、空键和索引块式的 restart_interval=1。 */

typedef struct test_entry {
  const char *key;
  const char *value;
} test_entry_t;

static const uint8_t kEmptyBlock[] = {0x00, 0x00, 0x00, 0x00, 0x01,
                                      0x00, 0x00, 0x00};

static const uint8_t kSingleEmptyEntry[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
};

static const uint8_t kOneEntry[] = {
    0x00, 0x03, 0x05, 0x6b, 0x65, 0x79, 0x76, 0x61,
    0x6c, 0x75, 0x65, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00,
};

static const uint8_t kInterval4Exact[] = {
    0x00, 0x05, 0x03, 0x61, 0x70, 0x70, 0x6c, 0x65, 0x72, 0x65, 0x64, 0x02,
    0x05, 0x06, 0x72, 0x69, 0x63, 0x6f, 0x74, 0x6f, 0x72, 0x61, 0x6e, 0x67,
    0x65, 0x01, 0x06, 0x05, 0x76, 0x6f, 0x63, 0x61, 0x64, 0x6f, 0x67, 0x72,
    0x65, 0x65, 0x6e, 0x00, 0x06, 0x00, 0x62, 0x61, 0x6e, 0x61, 0x6e, 0x61,
    0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
};

static const uint8_t kInterval4Six[] = {
    0x00, 0x05, 0x03, 0x61, 0x70, 0x70, 0x6c, 0x65, 0x72, 0x65, 0x64, 0x02,
    0x05, 0x06, 0x72, 0x69, 0x63, 0x6f, 0x74, 0x6f, 0x72, 0x61, 0x6e, 0x67,
    0x65, 0x01, 0x06, 0x05, 0x76, 0x6f, 0x63, 0x61, 0x64, 0x6f, 0x67, 0x72,
    0x65, 0x65, 0x6e, 0x00, 0x06, 0x00, 0x62, 0x61, 0x6e, 0x61, 0x6e, 0x61,
    0x00, 0x04, 0x01, 0x62, 0x61, 0x6e, 0x64, 0x78, 0x04, 0x03, 0x02, 0x61,
    0x6e, 0x61, 0x79, 0x79, 0x00, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x00, 0x00,
};

static const uint8_t kInterval1Three[] = {
    0x00, 0x05, 0x03, 0x61, 0x70, 0x70, 0x6c, 0x65, 0x72, 0x65, 0x64, 0x00,
    0x07, 0x06, 0x61, 0x70, 0x72, 0x69, 0x63, 0x6f, 0x74, 0x6f, 0x72, 0x61,
    0x6e, 0x67, 0x65, 0x00, 0x07, 0x05, 0x61, 0x76, 0x6f, 0x63, 0x61, 0x64,
    0x6f, 0x67, 0x72, 0x65, 0x65, 0x6e, 0x00, 0x00, 0x00, 0x00, 0x0b, 0x00,
    0x00, 0x00, 0x1b, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
};

static const uint8_t kInterval16Seventeen[] = {
    0x00, 0x01, 0x01, 0x61, 0x30, 0x01, 0x01, 0x01, 0x61, 0x31, 0x02, 0x01,
    0x01, 0x61, 0x32, 0x03, 0x01, 0x01, 0x61, 0x33, 0x04, 0x01, 0x01, 0x61,
    0x34, 0x05, 0x01, 0x01, 0x61, 0x35, 0x06, 0x01, 0x01, 0x61, 0x36, 0x07,
    0x01, 0x01, 0x61, 0x37, 0x08, 0x01, 0x01, 0x61, 0x38, 0x09, 0x01, 0x01,
    0x61, 0x39, 0x0a, 0x01, 0x01, 0x61, 0x61, 0x0b, 0x01, 0x01, 0x61, 0x62,
    0x0c, 0x01, 0x01, 0x61, 0x63, 0x0d, 0x01, 0x01, 0x61, 0x64, 0x0e, 0x01,
    0x01, 0x61, 0x65, 0x0f, 0x01, 0x01, 0x61, 0x66, 0x00, 0x11, 0x01, 0x61,
    0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61,
    0x61, 0x61, 0x61, 0x61, 0x67, 0x00, 0x00, 0x00, 0x00, 0x50, 0x00, 0x00,
    0x00, 0x02, 0x00, 0x00, 0x00,
};

static const uint8_t kEmptyKeyFirst[] = {
    0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x61, 0x00, 0x02,
    0x01, 0x61, 0x62, 0x76, 0x00, 0x00, 0x00, 0x00, 0x07,
    0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
};

static const uint8_t kTwoEntries[] = {
    0x00, 0x01, 0x01, 0x61, 0x31, 0x00, 0x01, 0x01, 0x62,
    0x32, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
};

static const test_entry_t kAppleEntries[] = {
    {"apple", "red"}, {"apricot", "orange"}, {"avocado", "green"},
    {"banana", ""},  {"band", "x"},           {"bandana", "yy"},
};

static const test_entry_t kEmptyKeyEntries[] = {
    {"", ""}, {"a", ""}, {"ab", "v"},
};

/* 用 options 的默认比较器初始化；interval 由调用者指定。 */
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
    cldb_slice_t key = cldb_slice_make(entries[i].key,
                                        strlen(entries[i].key));
    cldb_slice_t value = cldb_slice_make(entries[i].value,
                                          strlen(entries[i].value));

    CLDB_ASSERT_OK(cldb_block_builder_add(builder, key, value));
  }
}

static void expect_block(cldb_slice_t block, const uint8_t *expected,
                         size_t size) {
  CLDB_ASSERT_EQ_UINT(size, block.size);
  if (size != 0) {
    CLDB_ASSERT_TRUE(block.data != NULL);
    CLDB_ASSERT_EQ_INT(0, memcmp(block.data, expected, size));
  }
}

/* 空块仍有 restart[0]=0 和 num_restarts=1；Finish 后 empty 变为 false。 */
CLDB_TEST(block_builder_empty) {
  cldb_block_builder_t builder;
  cldb_slice_t block;
  cldb_slice_t second;

  init_builder(&builder, 16);
  CLDB_EXPECT_TRUE(cldb_block_builder_empty(&builder));
  CLDB_EXPECT_TRUE(cldb_slice_is_empty(cldb_block_builder_last_key(&builder)));
  CLDB_EXPECT_EQ_UINT(sizeof(kEmptyBlock),
                      cldb_block_builder_current_size_estimate(&builder));

  CLDB_ASSERT_OK(cldb_block_builder_finish(&builder, &block));
  expect_block(block, kEmptyBlock, sizeof(kEmptyBlock));
  CLDB_EXPECT_FALSE(cldb_block_builder_empty(&builder));
  CLDB_EXPECT_EQ_UINT(sizeof(kEmptyBlock) + 2U * sizeof(uint32_t),
                      cldb_block_builder_current_size_estimate(&builder));

  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_block_builder_finish(&builder, &second));
  CLDB_ASSERT_TRUE(cldb_slice_is_empty(second));
  expect_block(block, kEmptyBlock, sizeof(kEmptyBlock));

  CLDB_EXPECT_STATUS(
      CLDB_STATUS_INVALID_ARGUMENT,
      cldb_block_builder_add(&builder, cldb_slice_make("a", 1),
                             cldb_slice_make("1", 1)));
  expect_block(block, kEmptyBlock, sizeof(kEmptyBlock));

  cldb_block_builder_destroy(&builder);
}

/* 单条目的三连 varint、完整键和值，以及尾部 restart 数组。 */
CLDB_TEST(block_builder_single_entry_golden) {
  cldb_block_builder_t builder;
  cldb_slice_t block;

  init_builder(&builder, 16);
  CLDB_ASSERT_OK(cldb_block_builder_add(
      &builder, cldb_slice_make("key", 3), cldb_slice_make("value", 5)));
  CLDB_EXPECT_FALSE(cldb_block_builder_empty(&builder));
  CLDB_EXPECT_TRUE(
      cldb_slice_equal(cldb_block_builder_last_key(&builder),
                       cldb_slice_make("key", 3)));
  CLDB_EXPECT_EQ_UINT(sizeof(kOneEntry),
                      cldb_block_builder_current_size_estimate(&builder));
  CLDB_ASSERT_OK(cldb_block_builder_finish(&builder, &block));
  expect_block(block, kOneEntry, sizeof(kOneEntry));

  cldb_block_builder_destroy(&builder);
}

/* 恰好 interval 条只写入首个 restart，后续键共享前缀；空值也保留 varint0。 */
CLDB_TEST(block_builder_exact_interval_golden) {
  cldb_block_builder_t builder;
  cldb_slice_t block;

  init_builder(&builder, 4);
  add_entries(&builder, kAppleEntries, 4);
  CLDB_EXPECT_EQ_UINT(sizeof(kInterval4Exact),
                      cldb_block_builder_current_size_estimate(&builder));
  CLDB_ASSERT_OK(cldb_block_builder_finish(&builder, &block));
  expect_block(block, kInterval4Exact, sizeof(kInterval4Exact));

  cldb_block_builder_destroy(&builder);
}

/* 超过 interval 后下一条成为新 restart：shared=0，偏移写入尾部数组。 */
CLDB_TEST(block_builder_over_interval_golden) {
  cldb_block_builder_t builder;
  cldb_slice_t block;

  init_builder(&builder, 4);
  add_entries(&builder, kAppleEntries, 6);
  CLDB_EXPECT_EQ_UINT(sizeof(kInterval4Six),
                      cldb_block_builder_current_size_estimate(&builder));
  CLDB_ASSERT_OK(cldb_block_builder_finish(&builder, &block));
  expect_block(block, kInterval4Six, sizeof(kInterval4Six));

  cldb_block_builder_destroy(&builder);
}

/* restart_interval=1 时每条都是 restart，索引块依赖这一布局。 */
CLDB_TEST(block_builder_restart_interval_one_golden) {
  cldb_block_builder_t builder;
  cldb_slice_t block;

  init_builder(&builder, 1);
  add_entries(&builder, kAppleEntries, 3);
  CLDB_ASSERT_OK(cldb_block_builder_finish(&builder, &block));
  expect_block(block, kInterval1Three, sizeof(kInterval1Three));

  cldb_block_builder_destroy(&builder);
}

/* 默认 interval 下前 16 条共享前缀，第 17 条在偏移 0x50 处重新开始。 */
CLDB_TEST(block_builder_interval16_golden) {
  static const test_entry_t kEntries[] = {
      {"a", "0"},        {"aa", "1"},       {"aaa", "2"},
      {"aaaa", "3"},     {"aaaaa", "4"},    {"aaaaaa", "5"},
      {"aaaaaaa", "6"},  {"aaaaaaaa", "7"}, {"aaaaaaaaa", "8"},
      {"aaaaaaaaaa", "9"}, {"aaaaaaaaaaa", "a"},
      {"aaaaaaaaaaaa", "b"}, {"aaaaaaaaaaaaa", "c"},
      {"aaaaaaaaaaaaaa", "d"}, {"aaaaaaaaaaaaaaa", "e"},
      {"aaaaaaaaaaaaaaaa", "f"}, {"aaaaaaaaaaaaaaaaa", "g"},
  };
  cldb_block_builder_t builder;
  cldb_slice_t block;

  init_builder(&builder, 16);
  add_entries(&builder, kEntries, sizeof(kEntries) / sizeof(kEntries[0]));
  CLDB_ASSERT_OK(cldb_block_builder_finish(&builder, &block));
  expect_block(block, kInterval16Seventeen, sizeof(kInterval16Seventeen));

  cldb_block_builder_destroy(&builder);
}

/* 空键可以是首条；空键之后按比较器继续递增。 */
CLDB_TEST(block_builder_empty_key_golden) {
  cldb_block_builder_t builder;
  cldb_slice_t block;

  init_builder(&builder, 2);
  add_entries(&builder, kEmptyKeyEntries,
              sizeof(kEmptyKeyEntries) / sizeof(kEmptyKeyEntries[0]));
  CLDB_ASSERT_OK(cldb_block_builder_finish(&builder, &block));
  expect_block(block, kEmptyKeyFirst, sizeof(kEmptyKeyFirst));

  cldb_block_builder_destroy(&builder);
}

/* 非递增键被拒绝且不改变输出；Finish 后 Reset 可重新构建同一块。 */
CLDB_TEST(block_builder_order_and_reset) {
  cldb_block_builder_t builder;
  cldb_slice_t first_block;
  cldb_slice_t second_block;
  size_t estimate;

  init_builder(&builder, 16);
  CLDB_ASSERT_OK(cldb_block_builder_add(
      &builder, cldb_slice_make("a", 1), cldb_slice_make("1", 1)));
  estimate = cldb_block_builder_current_size_estimate(&builder);

  CLDB_EXPECT_STATUS(
      CLDB_STATUS_INVALID_ARGUMENT,
      cldb_block_builder_add(&builder, cldb_slice_make("a", 1),
                             cldb_slice_make("x", 1)));
  CLDB_EXPECT_STATUS(
      CLDB_STATUS_INVALID_ARGUMENT,
      cldb_block_builder_add(&builder, cldb_slice_make("A", 1),
                             cldb_slice_make("x", 1)));
  CLDB_EXPECT_EQ_UINT(estimate,
                      cldb_block_builder_current_size_estimate(&builder));
  CLDB_EXPECT_TRUE(
      cldb_slice_equal(cldb_block_builder_last_key(&builder),
                       cldb_slice_make("a", 1)));

  CLDB_ASSERT_OK(cldb_block_builder_add(
      &builder, cldb_slice_make("b", 1), cldb_slice_make("2", 1)));
  CLDB_ASSERT_OK(cldb_block_builder_finish(&builder, &first_block));
  expect_block(first_block, kTwoEntries, sizeof(kTwoEntries));

  cldb_block_builder_reset(&builder);
  CLDB_EXPECT_TRUE(cldb_block_builder_empty(&builder));
  CLDB_EXPECT_EQ_UINT(sizeof(kEmptyBlock),
                      cldb_block_builder_current_size_estimate(&builder));
  CLDB_EXPECT_TRUE(cldb_slice_is_empty(cldb_block_builder_last_key(&builder)));
  CLDB_ASSERT_OK(cldb_block_builder_add(
      &builder, cldb_slice_make("a", 1), cldb_slice_make("1", 1)));
  CLDB_ASSERT_OK(cldb_block_builder_add(
      &builder, cldb_slice_make("b", 1), cldb_slice_make("2", 1)));
  CLDB_ASSERT_OK(cldb_block_builder_finish(&builder, &second_block));
  expect_block(second_block, kTwoEntries, sizeof(kTwoEntries));

  cldb_block_builder_destroy(&builder);
}

/* 参数错误、非法 Slice、NULL 输出和未完成初始化都不能破坏状态。 */
CLDB_TEST(block_builder_invalid_arguments) {
  cldb_block_builder_t builder;
  cldb_block_builder_t bad_builder;
  cldb_options_t options;
  cldb_slice_t block;
  cldb_slice_t invalid_key = {NULL, 1};
  cldb_slice_t invalid_value = {NULL, 1};
  cldb_slice_t empty_slice = {NULL, 0};

  cldb_options_init(&options);
  options.block_restart_interval = 0;
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_block_builder_init(&bad_builder, &options));
  cldb_block_builder_destroy(&bad_builder);
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_block_builder_init(&bad_builder, NULL));
  cldb_block_builder_destroy(&bad_builder);
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_block_builder_init(NULL, &options));

  init_builder(&builder, 16);
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_block_builder_add(&builder, invalid_key,
                                            cldb_slice_make("v", 1)));
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_block_builder_add(&builder, cldb_slice_make("k", 1),
                                            invalid_value));
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_block_builder_finish(&builder, NULL));

  /* NULL+0 是合法空键；放在一条有效记录之前，Finish 仍可正常工作。 */
  cldb_block_builder_reset(&builder);
  CLDB_ASSERT_OK(cldb_block_builder_add(&builder, empty_slice, empty_slice));
  CLDB_ASSERT_OK(cldb_block_builder_finish(&builder, &block));
  expect_block(block, kSingleEmptyEntry, sizeof(kSingleEmptyEntry));

  cldb_block_builder_destroy(&builder);
  cldb_block_builder_destroy(NULL);
  CLDB_EXPECT_TRUE(cldb_block_builder_empty(NULL));
  CLDB_EXPECT_EQ_UINT(0, cldb_block_builder_current_size_estimate(NULL));
  CLDB_EXPECT_TRUE(cldb_slice_is_empty(cldb_block_builder_last_key(NULL)));
}

void cldb_register_block_builder_tests(void) {
  CLDB_RUN_TEST(block_builder_empty);
  CLDB_RUN_TEST(block_builder_single_entry_golden);
  CLDB_RUN_TEST(block_builder_exact_interval_golden);
  CLDB_RUN_TEST(block_builder_over_interval_golden);
  CLDB_RUN_TEST(block_builder_restart_interval_one_golden);
  CLDB_RUN_TEST(block_builder_interval16_golden);
  CLDB_RUN_TEST(block_builder_empty_key_golden);
  CLDB_RUN_TEST(block_builder_order_and_reset);
  CLDB_RUN_TEST(block_builder_invalid_arguments);
}
