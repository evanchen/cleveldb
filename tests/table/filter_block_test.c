#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "cleveldb/filter_policy.h"
#include "harness.h"
#include "status_test_util.h"
#include "table/filter_block.h"
#include "util/coding.h"
#include "util/hash.h"

/* 与上游 filter_block_test.cc 的 TestHashFilter 相同的测试策略：每个 key
 * 追加一个 fixed32 哈希，查询时逐个比较；过滤器为空则一定不匹配。 */
typedef struct test_hash_policy {
  cldb_filter_policy_t base; /* 必须为首成员，便于 vtable 回调转回本类型 */
} test_hash_policy_t;

static const char *test_hash_name(const cldb_filter_policy_t *policy) {
  (void)policy;
  return "TestHashFilter";
}

static cldb_status_t test_hash_create_filter(const cldb_filter_policy_t *policy,
                                             const cldb_slice_t *keys, size_t n,
                                             cldb_filter_sink_t *dst) {
  size_t i;

  (void)policy;
  if (keys == NULL && n != 0) {
    return cldb_status_invalid_argument("test_hash.create_filter",
                                        "keys is NULL");
  }
  if (dst == NULL || dst->append == NULL) {
    return cldb_status_invalid_argument("test_hash.create_filter",
                                        "destination is invalid");
  }
  for (i = 0; i < n; ++i) {
    char *entry = dst->append(dst->arg, NULL, sizeof(uint32_t));

    if (entry == NULL) {
      return cldb_status_io("test_hash.create_filter", "append failed");
    }
    cldb_encode_fixed32(
        entry, cldb_hash(keys[i].data, keys[i].size, UINT32_C(1)));
  }
  return cldb_status_ok();
}

static bool test_hash_key_may_match(const cldb_filter_policy_t *policy,
                                    cldb_slice_t key, cldb_slice_t filter) {
  uint32_t h;
  size_t i;

  (void)policy;
  if (key.data == NULL && key.size != 0) {
    return true;
  }
  if (filter.data == NULL && filter.size != 0) {
    return true;
  }
  h = cldb_hash(key.data, key.size, UINT32_C(1));
  if (filter.size >= sizeof(uint32_t)) {
    for (i = 0; i <= filter.size - sizeof(uint32_t);
         i += sizeof(uint32_t)) {
      if (h == cldb_decode_fixed32(filter.data + i)) {
        return true;
      }
    }
  }
  return false;
}

static void test_hash_destroy(cldb_filter_policy_t *policy) {
  (void)policy;
}

static const cldb_filter_policy_vtbl_t kTestHashVtbl = {
    .name = test_hash_name,
    .create_filter = test_hash_create_filter,
    .key_may_match = test_hash_key_may_match,
    .destroy = test_hash_destroy,
};

static void init_hash_policy(test_hash_policy_t *policy) {
  memset(policy, 0, sizeof(*policy));
  policy->base.vtbl = &kTestHashVtbl;
}

static void init_builder(cldb_filter_block_builder_t *builder,
                         cldb_filter_policy_t *policy) {
  CLDB_ASSERT_OK(cldb_filter_block_builder_init(builder, policy));
}

static void builder_start(cldb_filter_block_builder_t *builder,
                          uint64_t block_offset) {
  CLDB_ASSERT_OK(
      cldb_filter_block_builder_start_block(builder, block_offset));
}

static void builder_add(cldb_filter_block_builder_t *builder,
                        const char *key) {
  CLDB_ASSERT_OK(cldb_filter_block_builder_add_key(
      builder, cldb_slice_make(key, strlen(key))));
}

static void expect_block(cldb_slice_t block, const uint8_t *expected,
                         size_t size) {
  CLDB_ASSERT_EQ_UINT(size, block.size);
  if (size != 0) {
    CLDB_ASSERT_TRUE(block.data != NULL);
    CLDB_ASSERT_EQ_INT(0, memcmp(block.data, expected, size));
  }
}

static bool may_match(cldb_filter_block_reader_t *reader, uint64_t block_offset,
                      const char *key) {
  return cldb_filter_block_reader_key_may_match(
      reader, block_offset, cldb_slice_make(key, strlen(key)));
}

/* 上游 FilterBlockTest.EmptyBuilder 的字节布局。 */
static const uint8_t kEmptyBlock[] = {0x00, 0x00, 0x00, 0x00, 0x0b};

/* StartBlock(100) foo/bar/box、StartBlock(200) box、StartBlock(300) hello：
 * 三个块落在同一个 2 KB 区间，最终只有一个过滤器；box 在 keys 中出现两次，
 * 与上游一样不去重。 */
static const uint8_t kSingleChunk[] = {
    0xc4, 0x35, 0x77, 0xfe, 0x37, 0x8c, 0x73, 0xd9, 0x26, 0x96, 0x0f,
    0xc8, 0x26, 0x96, 0x0f, 0xc8, 0xbf, 0x27, 0x8b, 0x0e, 0x00, 0x00,
    0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x0b,
};

/* 上游 MultiChunk：过滤器 0=foo/bar，1=box，2、3 为空，4=box/hello。 */
static const uint8_t kMultiChunk[] = {
    0xc4, 0x35, 0x77, 0xfe, 0x37, 0x8c, 0x73, 0xd9, 0x26, 0x96,
    0x0f, 0xc8, 0x26, 0x96, 0x0f, 0xc8, 0xbf, 0x27, 0x8b, 0x0e,
    0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x0c, 0x00,
    0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x0c, 0x00, 0x00, 0x00,
    0x14, 0x00, 0x00, 0x00, 0x0b,
};

/* 偏移 0/1/2047 落在同一区间，2048 开始第二组，4096 开始第三组。 */
static const uint8_t kBoundaryGroups[] = {
    0x8a, 0xf5, 0xc7, 0x03, 0xd6, 0x9d, 0x6c, 0xca, 0x3e, 0x44, 0x11,
    0x91, 0x15, 0xec, 0xb5, 0x57, 0xcb, 0x93, 0x5a, 0x1e, 0x00, 0x00,
    0x00, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x14,
    0x00, 0x00, 0x00, 0x0b,
};

CLDB_TEST(filter_block_empty_builder) {
  test_hash_policy_t policy;
  cldb_filter_block_builder_t builder;
  cldb_filter_block_reader_t reader;
  cldb_slice_t block;
  cldb_slice_t again;

  init_hash_policy(&policy);
  init_builder(&builder, &policy.base);
  CLDB_ASSERT_OK(cldb_filter_block_builder_finish(&builder, &block));
  expect_block(block, kEmptyBlock, sizeof(kEmptyBlock));

  cldb_filter_block_reader_init(&reader, &policy.base, block);
  CLDB_EXPECT_TRUE(may_match(&reader, 0, "foo"));
  CLDB_EXPECT_TRUE(may_match(&reader, 100000, "foo"));

  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_filter_block_builder_finish(&builder, &again));
  CLDB_EXPECT_EQ_UINT(0, again.size);
  CLDB_EXPECT_STATUS(
      CLDB_STATUS_INVALID_ARGUMENT,
      cldb_filter_block_builder_add_key(
          &builder, cldb_slice_make("late", strlen("late"))));
  CLDB_EXPECT_STATUS(
      CLDB_STATUS_INVALID_ARGUMENT,
      cldb_filter_block_builder_start_block(&builder, 0));
  cldb_filter_block_builder_destroy(&builder);
}

CLDB_TEST(filter_block_single_chunk) {
  test_hash_policy_t policy;
  cldb_filter_block_builder_t builder;
  cldb_filter_block_reader_t reader;
  cldb_slice_t block;

  init_hash_policy(&policy);
  init_builder(&builder, &policy.base);
  builder_start(&builder, 100);
  builder_add(&builder, "foo");
  builder_add(&builder, "bar");
  builder_add(&builder, "box");
  builder_start(&builder, 200);
  builder_add(&builder, "box");
  builder_start(&builder, 300);
  builder_add(&builder, "hello");
  CLDB_ASSERT_OK(cldb_filter_block_builder_finish(&builder, &block));
  expect_block(block, kSingleChunk, sizeof(kSingleChunk));

  cldb_filter_block_reader_init(&reader, &policy.base, block);
  CLDB_EXPECT_TRUE(may_match(&reader, 100, "foo"));
  CLDB_EXPECT_TRUE(may_match(&reader, 100, "bar"));
  CLDB_EXPECT_TRUE(may_match(&reader, 100, "box"));
  CLDB_EXPECT_TRUE(may_match(&reader, 100, "hello"));
  CLDB_EXPECT_FALSE(may_match(&reader, 100, "missing"));
  CLDB_EXPECT_FALSE(may_match(&reader, 100, "other"));
  CLDB_EXPECT_TRUE(may_match(&reader, 100000, "any"));
  cldb_filter_block_builder_destroy(&builder);
}

CLDB_TEST(filter_block_multi_chunk) {
  test_hash_policy_t policy;
  cldb_filter_block_builder_t builder;
  cldb_filter_block_reader_t reader;
  cldb_slice_t block;

  init_hash_policy(&policy);
  init_builder(&builder, &policy.base);
  builder_start(&builder, 0);
  builder_add(&builder, "foo");
  builder_start(&builder, 2000);
  builder_add(&builder, "bar");
  builder_start(&builder, 3100);
  builder_add(&builder, "box");
  builder_start(&builder, 9000);
  builder_add(&builder, "box");
  builder_add(&builder, "hello");
  CLDB_ASSERT_OK(cldb_filter_block_builder_finish(&builder, &block));
  expect_block(block, kMultiChunk, sizeof(kMultiChunk));

  cldb_filter_block_reader_init(&reader, &policy.base, block);
  CLDB_EXPECT_TRUE(may_match(&reader, 0, "foo"));
  CLDB_EXPECT_TRUE(may_match(&reader, 2000, "bar"));
  CLDB_EXPECT_FALSE(may_match(&reader, 0, "box"));
  CLDB_EXPECT_FALSE(may_match(&reader, 0, "hello"));
  CLDB_EXPECT_TRUE(may_match(&reader, 3100, "box"));
  CLDB_EXPECT_FALSE(may_match(&reader, 3100, "foo"));
  CLDB_EXPECT_FALSE(may_match(&reader, 3100, "bar"));
  CLDB_EXPECT_FALSE(may_match(&reader, 3100, "hello"));
  CLDB_EXPECT_FALSE(may_match(&reader, 4100, "foo"));
  CLDB_EXPECT_FALSE(may_match(&reader, 4100, "bar"));
  CLDB_EXPECT_FALSE(may_match(&reader, 4100, "box"));
  CLDB_EXPECT_FALSE(may_match(&reader, 4100, "hello"));
  CLDB_EXPECT_TRUE(may_match(&reader, 9000, "box"));
  CLDB_EXPECT_TRUE(may_match(&reader, 9000, "hello"));
  CLDB_EXPECT_FALSE(may_match(&reader, 9000, "foo"));
  CLDB_EXPECT_FALSE(may_match(&reader, 9000, "bar"));
  cldb_filter_block_builder_destroy(&builder);
}

CLDB_TEST(filter_block_boundary_groups) {
  test_hash_policy_t policy;
  cldb_filter_block_builder_t builder;
  cldb_filter_block_reader_t reader;
  cldb_slice_t block;
  size_t array_offset;
  size_t num_offsets;

  init_hash_policy(&policy);
  init_builder(&builder, &policy.base);
  builder_start(&builder, 0);
  builder_add(&builder, "a");
  builder_start(&builder, 1);
  builder_add(&builder, "b");
  builder_start(&builder, 2047);
  builder_add(&builder, "c");
  builder_start(&builder, 2048);
  builder_add(&builder, "d");
  builder_start(&builder, 4096);
  builder_add(&builder, "e");
  CLDB_ASSERT_OK(cldb_filter_block_builder_finish(&builder, &block));
  expect_block(block, kBoundaryGroups, sizeof(kBoundaryGroups));

  array_offset =
      (size_t)cldb_decode_fixed32(block.data + block.size - 5U);
  num_offsets = (block.size - 5U - array_offset) / sizeof(uint32_t);
  CLDB_EXPECT_EQ_UINT(3, num_offsets);

  cldb_filter_block_reader_init(&reader, &policy.base, block);
  CLDB_EXPECT_TRUE(may_match(&reader, 0, "a"));
  CLDB_EXPECT_TRUE(may_match(&reader, 1, "b"));
  CLDB_EXPECT_TRUE(may_match(&reader, 2047, "c"));
  CLDB_EXPECT_FALSE(may_match(&reader, 0, "d"));
  CLDB_EXPECT_FALSE(may_match(&reader, 2046, "d"));
  CLDB_EXPECT_TRUE(may_match(&reader, 2048, "d"));
  CLDB_EXPECT_FALSE(may_match(&reader, 2048, "a"));
  CLDB_EXPECT_TRUE(may_match(&reader, 4096, "e"));
  CLDB_EXPECT_TRUE(may_match(&reader, 4097, "e"));
  CLDB_EXPECT_TRUE(may_match(&reader, 6144, "anything"));
  CLDB_EXPECT_TRUE(may_match(&reader, 100000, "anything"));
  cldb_filter_block_builder_destroy(&builder);
}

CLDB_TEST(filter_block_corrupt_reader) {
  test_hash_policy_t policy;
  cldb_filter_block_builder_t builder;
  cldb_filter_block_reader_t reader;
  cldb_slice_t block;
  cldb_slice_t contents;
  char corrupt[64];
  size_t n;
  size_t array_offset;

  init_hash_policy(&policy);
  init_builder(&builder, &policy.base);
  builder_start(&builder, 0);
  builder_add(&builder, "foo");
  builder_add(&builder, "bar");
  builder_add(&builder, "box");
  builder_add(&builder, "hello");
  builder_start(&builder, 2048);
  builder_add(&builder, "second");
  CLDB_ASSERT_OK(cldb_filter_block_builder_finish(&builder, &block));
  n = block.size;
  CLDB_ASSERT_TRUE(n <= sizeof(corrupt));
  array_offset = (size_t)cldb_decode_fixed32(block.data + n - 5U);

  cldb_filter_block_reader_init(&reader, &policy.base, block);
  CLDB_EXPECT_TRUE(may_match(&reader, 0, "foo"));
  CLDB_EXPECT_FALSE(may_match(&reader, 0, "missing"));

  /* 少于 5 字节：无法读取 array_offset/base_lg，保守返回可能匹配。 */
  memcpy(corrupt, block.data, n);
  contents = cldb_slice_make(corrupt, 4);
  cldb_filter_block_reader_init(&reader, &policy.base, contents);
  CLDB_EXPECT_TRUE(may_match(&reader, 0, "foo"));

  /* array_offset 指向块尾之后：块损坏，保守放行。 */
  memcpy(corrupt, block.data, n);
  cldb_encode_fixed32(corrupt + n - 5U, (uint32_t)(n - 4U));
  contents = cldb_slice_make(corrupt, n);
  cldb_filter_block_reader_init(&reader, &policy.base, contents);
  CLDB_EXPECT_TRUE(may_match(&reader, 0, "foo"));

  /* offsets[0] 超过 data_size：区间非法，必须放行而不是判不存在。 */
  memcpy(corrupt, block.data, n);
  cldb_encode_fixed32(corrupt + array_offset, 17);
  contents = cldb_slice_make(corrupt, n);
  cldb_filter_block_reader_init(&reader, &policy.base, contents);
  CLDB_EXPECT_TRUE(may_match(&reader, 0, "foo"));

  /* start == limit 但仍在数据范围外：同样属于损坏，必须放行。 */
  memcpy(corrupt, block.data, n);
  cldb_encode_fixed32(corrupt + array_offset, 21);
  cldb_encode_fixed32(corrupt + array_offset + sizeof(uint32_t), 21);
  contents = cldb_slice_make(corrupt, n);
  cldb_filter_block_reader_init(&reader, &policy.base, contents);
  CLDB_EXPECT_TRUE(may_match(&reader, 0, "foo"));

  /* base_lg >= 64 会超出移位宽度；按损坏处理。 */
  memcpy(corrupt, block.data, n);
  corrupt[n - 1U] = 64;
  contents = cldb_slice_make(corrupt, n);
  cldb_filter_block_reader_init(&reader, &policy.base, contents);
  CLDB_EXPECT_TRUE(may_match(&reader, 0, "foo"));

  cldb_filter_block_builder_destroy(&builder);
}

CLDB_TEST(filter_block_invalid_arguments) {
  test_hash_policy_t policy;
  cldb_filter_policy_t invalid_policy;
  cldb_filter_block_builder_t builder;
  cldb_filter_block_reader_t reader;
  cldb_slice_t block;
  cldb_slice_t contents;

  init_hash_policy(&policy);
  memset(&invalid_policy, 0, sizeof(invalid_policy));
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_filter_block_builder_init(NULL, &policy.base));
  CLDB_EXPECT_STATUS(
      CLDB_STATUS_INVALID_ARGUMENT,
      cldb_filter_block_builder_init(&builder, NULL));
  CLDB_EXPECT_STATUS(
      CLDB_STATUS_INVALID_ARGUMENT,
      cldb_filter_block_builder_init(&builder, &invalid_policy));
  cldb_filter_block_builder_destroy(NULL);

  init_builder(&builder, &policy.base);
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_filter_block_builder_finish(NULL, &block));
  {
    cldb_slice_t invalid_key = {NULL, 1};

    CLDB_EXPECT_STATUS(
        CLDB_STATUS_INVALID_ARGUMENT,
        cldb_filter_block_builder_add_key(&builder, invalid_key));
  }
  CLDB_EXPECT_STATUS(
      CLDB_STATUS_INVALID_ARGUMENT,
      cldb_filter_block_builder_start_block(NULL, 0));

  builder_start(&builder, 4096); /* 生成 0、1 两个空区间 */
  CLDB_EXPECT_STATUS(
      CLDB_STATUS_INVALID_ARGUMENT,
      cldb_filter_block_builder_start_block(&builder, 1000));
  builder_add(&builder, "late");
  CLDB_ASSERT_OK(cldb_filter_block_builder_finish(&builder, &block));
  cldb_filter_block_reader_init(&reader, &policy.base, block);
  CLDB_EXPECT_TRUE(may_match(&reader, 4096, "late"));
  CLDB_EXPECT_TRUE(may_match(&reader, 100000, "anything"));

  contents = cldb_slice_make(NULL, 0);
  cldb_filter_block_reader_init(NULL, &policy.base, contents);
  cldb_filter_block_reader_init(&reader, NULL, contents);
  CLDB_EXPECT_TRUE(may_match(&reader, 0, "foo"));
  cldb_filter_block_reader_init(&reader, &invalid_policy, contents);
  CLDB_EXPECT_TRUE(may_match(&reader, 0, "foo"));

  cldb_filter_block_builder_destroy(&builder);
}

CLDB_TEST(filter_block_empty_key) {
  test_hash_policy_t policy;
  cldb_filter_block_builder_t builder;
  cldb_filter_block_reader_t reader;
  cldb_slice_t block;
  cldb_slice_t empty_key = cldb_slice_make(NULL, 0);

  init_hash_policy(&policy);
  init_builder(&builder, &policy.base);
  builder_start(&builder, 0);
  CLDB_ASSERT_OK(cldb_filter_block_builder_add_key(&builder, empty_key));
  builder_add(&builder, "nonempty");
  CLDB_ASSERT_OK(cldb_filter_block_builder_finish(&builder, &block));

  cldb_filter_block_reader_init(&reader, &policy.base, block);
  CLDB_EXPECT_TRUE(
      cldb_filter_block_reader_key_may_match(&reader, 0, empty_key));
  CLDB_EXPECT_TRUE(may_match(&reader, 0, "nonempty"));
  CLDB_EXPECT_FALSE(may_match(&reader, 0, "missing"));
  cldb_filter_block_builder_destroy(&builder);
}

CLDB_TEST(filter_block_bloom_integration) {
  cldb_filter_policy_t *bloom;
  cldb_filter_block_builder_t builder;
  cldb_filter_block_reader_t reader;
  cldb_slice_t block;

  bloom = cldb_new_bloom_filter_policy(10);
  CLDB_ASSERT_TRUE(bloom != NULL);
  init_builder(&builder, bloom);
  builder_start(&builder, 0);
  builder_add(&builder, "apple");
  builder_add(&builder, "banana");
  builder_add(&builder, "cherry");
  CLDB_ASSERT_OK(cldb_filter_block_builder_finish(&builder, &block));

  cldb_filter_block_reader_init(&reader, bloom, block);
  CLDB_EXPECT_TRUE(may_match(&reader, 0, "apple"));
  CLDB_EXPECT_TRUE(may_match(&reader, 0, "banana"));
  CLDB_EXPECT_TRUE(may_match(&reader, 0, "cherry"));
  CLDB_EXPECT_FALSE(may_match(&reader, 0, "durian"));
  CLDB_EXPECT_FALSE(may_match(&reader, 0, "missing"));

  cldb_filter_block_builder_destroy(&builder);
  cldb_filter_policy_destroy(bloom);
}

void cldb_register_filter_block_tests(void) {
  CLDB_RUN_TEST(filter_block_empty_builder);
  CLDB_RUN_TEST(filter_block_single_chunk);
  CLDB_RUN_TEST(filter_block_multi_chunk);
  CLDB_RUN_TEST(filter_block_boundary_groups);
  CLDB_RUN_TEST(filter_block_corrupt_reader);
  CLDB_RUN_TEST(filter_block_invalid_arguments);
  CLDB_RUN_TEST(filter_block_empty_key);
  CLDB_RUN_TEST(filter_block_bloom_integration);
}
