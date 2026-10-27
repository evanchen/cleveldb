#include <stdint.h>
#include <string.h>

#include "cleveldb/filter_policy.h"
#include "harness.h"
#include "status_test_util.h"
#include "util/buf.h"
#include "util/coding.h"

/* 把过滤器追加到 cldb_buf_t 的 sink 适配器；用内部 buf 验证公开 sink 契约。
 * data 为 NULL 时由 resize 补零。 */
static char *buf_append(void *arg, const void *data, size_t size) {
  cldb_buf_t *buf = arg;
  size_t old_size;

  if (buf == NULL || size == 0 || size > SIZE_MAX - buf->size) {
    return NULL;
  }
  old_size = buf->size;
  if (data == NULL) {
    if (!cldb_buf_resize(buf, old_size + size)) {
      return NULL;
    }
  } else if (!cldb_buf_append(buf, data, size)) {
    return NULL;
  }
  return buf->data + old_size;
}

static char *failing_append(void *arg, const void *data, size_t size) {
  (void)arg;
  (void)data;
  (void)size;
  return NULL;
}

/* 校验固定输入的完整过滤器字节；prefix 模拟 FilterBlock 已追加的前序内容，
 * 同时确认 Bloom 只追加、不改写前缀。 */
static void expect_bloom_filter(int bits_per_key, const cldb_slice_t *keys,
                                size_t n, const void *prefix,
                                size_t prefix_size, const uint8_t *expected,
                                size_t expected_size) {
  cldb_filter_policy_t *policy;
  cldb_buf_t dst;
  cldb_filter_sink_t sink;
  size_t expected_total = prefix_size + expected_size;
  size_t i;

  cldb_buf_init(&dst);
  if (prefix_size != 0) {
    CLDB_ASSERT_TRUE(cldb_buf_append(&dst, prefix, prefix_size));
  }
  policy = cldb_new_bloom_filter_policy(bits_per_key);
  CLDB_ASSERT_TRUE(policy != NULL);
  sink.arg = &dst;
  sink.append = buf_append;
  CLDB_ASSERT_OK(cldb_filter_policy_create_filter(policy, keys, n, &sink));

  CLDB_EXPECT_EQ_UINT(expected_total, dst.size);
  if (prefix_size != 0) {
    CLDB_EXPECT_TRUE(memcmp(dst.data, prefix, prefix_size) == 0);
  }
  if (dst.size == expected_total) {
    cldb_slice_t filter =
        cldb_slice_make(dst.data + prefix_size, expected_size);

    CLDB_EXPECT_EQ_INT(0,
                       memcmp(dst.data + prefix_size, expected, expected_size));
    for (i = 0; i < n; ++i) {
      CLDB_EXPECT_TRUE(
          cldb_filter_policy_key_may_match(policy, keys[i], filter));
    }
    if (n == 0) {
      CLDB_EXPECT_FALSE(cldb_filter_policy_key_may_match(
          policy, cldb_slice_make("absent", 6), filter));
    }
  }

  cldb_filter_policy_destroy(policy);
  cldb_buf_free(&dst);
}

CLDB_TEST(bloom_name_and_factory) {
  static const char kExpectedName[] = "leveldb.BuiltinBloomFilter2";
  cldb_filter_policy_t *policy;
  const char *name;

  CLDB_EXPECT_TRUE(cldb_new_bloom_filter_policy(-1) == NULL);

  policy = cldb_new_bloom_filter_policy(10);
  CLDB_ASSERT_TRUE(policy != NULL);
  name = cldb_filter_policy_name(policy);
  CLDB_ASSERT_TRUE(name != NULL);
  CLDB_EXPECT_EQ_STR(kExpectedName, name);
  CLDB_EXPECT_EQ_UINT(sizeof(kExpectedName) - 1, strlen(name));

  cldb_filter_policy_destroy(policy);
  cldb_filter_policy_destroy(NULL);
}

CLDB_TEST(bloom_golden_filter_bytes) {
  static const char kHello[] = "hello";
  static const char kWorld[] = "world";
  static const char kA[] = "a";
  static const char kAbc[] = "abc";
  static const char kBinary1[] = {0x00, 0x01, 0x02};
  static const char kBinary2[] = {(char)0xff, (char)0xfe};
  static const char kHelloWorld[] = "hello world";
  static const char kPrefix[] = {(char)0xaa, (char)0xbb};

  static const uint8_t kExpectedHelloWorld[] = {
      0x11, 0x40, 0x00, 0x41, 0x44, 0x10, 0x40, 0x10, 0x06};
  static const uint8_t kExpectedBits1[] = {0x00, 0x00, 0x00, 0x00, 0x00,
                                           0x80, 0x11, 0x00, 0x01};
  static const uint8_t kExpectedBits10[] = {0x08, 0x18, 0x24, 0x60, 0x82,
                                            0x80, 0x11, 0x82, 0x06};
  static const uint8_t kExpectedBinary[] = {
      0xd2, 0x62, 0x66, 0x4e, 0x8e, 0x3c, 0x6d,
      0x71, 0xe2, 0xa4, 0xcc, 0xcc, 0x14};
  static const uint8_t kExpectedBits100[] = {
      0x04, 0x11, 0x44, 0x10, 0x41, 0x04, 0x11,
      0x44, 0x10, 0x11, 0x44, 0x11, 0x40, 0x1e};
  static const uint8_t kExpectedEmpty[] = {0x00, 0x00, 0x00, 0x00, 0x00,
                                           0x00, 0x00, 0x00, 0x06};

  cldb_slice_t two_keys[] = {cldb_slice_make(kHello, 5),
                             cldb_slice_make(kWorld, 5)};
  cldb_slice_t three_keys[] = {cldb_slice_make(NULL, 0),
                               cldb_slice_make(kA, 1),
                               cldb_slice_make(kAbc, 3)};
  cldb_slice_t binary_keys[] = {cldb_slice_make(kBinary1, 3),
                                cldb_slice_make(kBinary2, 2),
                                cldb_slice_make(kHelloWorld, 11)};
  cldb_slice_t one_key[] = {cldb_slice_make("k", 1)};

  expect_bloom_filter(10, two_keys, 2, NULL, 0, kExpectedHelloWorld,
                      sizeof(kExpectedHelloWorld));
  expect_bloom_filter(1, three_keys, 3, NULL, 0, kExpectedBits1,
                      sizeof(kExpectedBits1));
  /* 已有前缀时只追加新过滤器，前缀字节保持不变。 */
  expect_bloom_filter(10, three_keys, 3, kPrefix, sizeof(kPrefix),
                      kExpectedBits10, sizeof(kExpectedBits10));
  expect_bloom_filter(30, binary_keys, 3, NULL, 0, kExpectedBinary,
                      sizeof(kExpectedBinary));
  /* bits_per_key=100 触发 k 的上限 clamp（30）。 */
  expect_bloom_filter(100, one_key, 1, NULL, 0, kExpectedBits100,
                      sizeof(kExpectedBits100));
  /* 空 key 集合仍生成 64 位最小过滤器，且不匹配任意 key。 */
  expect_bloom_filter(10, NULL, 0, NULL, 0, kExpectedEmpty,
                      sizeof(kExpectedEmpty));
}

CLDB_TEST(bloom_empty_and_malformed_filters) {
  static const char kEmptyFilter[] = {0, 0, 0, 0, 0, 0, 0, 0, 6};
  static const char kOneByte[] = {0};
  static const char kReserved31[] = {0, 31};
  static const char kReserved255[] = {0, (char)0xff};
  static const char kZeroProbes[] = {0, 0};
  cldb_filter_policy_t *policy = cldb_new_bloom_filter_policy(10);
  cldb_slice_t key = cldb_slice_make("hello", 5);

  CLDB_ASSERT_TRUE(policy != NULL);

  CLDB_EXPECT_FALSE(cldb_filter_policy_key_may_match(
      policy, key, cldb_slice_make(NULL, 0)));
  CLDB_EXPECT_FALSE(cldb_filter_policy_key_may_match(
      policy, key, cldb_slice_make(kOneByte, sizeof(kOneByte))));
  CLDB_EXPECT_TRUE(cldb_filter_policy_key_may_match(
      policy, key, cldb_slice_make(kReserved31, sizeof(kReserved31))));
  CLDB_EXPECT_TRUE(cldb_filter_policy_key_may_match(
      policy, key, cldb_slice_make(kReserved255, sizeof(kReserved255))));
  /* k == 0 不检查任何位；与上游一致返回 true。 */
  CLDB_EXPECT_TRUE(cldb_filter_policy_key_may_match(
      policy, key, cldb_slice_make(kZeroProbes, sizeof(kZeroProbes))));
  CLDB_EXPECT_FALSE(cldb_filter_policy_key_may_match(
      policy, key, cldb_slice_make(kEmptyFilter, sizeof(kEmptyFilter))));

  cldb_filter_policy_destroy(policy);
}

enum { kFprKeyCount = 10000 };

/* 插入 kFprKeyCount 个 key，再用另一批 key 查询并统计 false positive；
 * 阈值留有余量，验证 bits_per_key 对误判率的影响而不绑定精确计数。 */
static void check_false_positive_rate(int bits_per_key,
                                      size_t max_false_positives) {
  char key_storage[kFprKeyCount][4];
  cldb_slice_t keys[kFprKeyCount];
  char query[4];
  cldb_buf_t filter;
  cldb_filter_sink_t sink;
  cldb_filter_policy_t *policy;
  cldb_slice_t filter_slice;
  size_t false_positives = 0;
  size_t i;

  for (i = 0; i < kFprKeyCount; ++i) {
    cldb_encode_fixed32(key_storage[i], (uint32_t)i);
    keys[i] = cldb_slice_make(key_storage[i], 4);
  }

  cldb_buf_init(&filter);
  sink.arg = &filter;
  sink.append = buf_append;
  policy = cldb_new_bloom_filter_policy(bits_per_key);
  CLDB_ASSERT_TRUE(policy != NULL);
  CLDB_ASSERT_OK(cldb_filter_policy_create_filter(policy, keys,
                                                   kFprKeyCount, &sink));
  CLDB_EXPECT_TRUE(filter.size <=
                   (size_t)bits_per_key * kFprKeyCount / 8 + 40);
  filter_slice = cldb_buf_slice(&filter);

  for (i = 0; i < kFprKeyCount; ++i) {
    CLDB_EXPECT_TRUE(
        cldb_filter_policy_key_may_match(policy, keys[i], filter_slice));
  }
  for (i = 0; i < kFprKeyCount; ++i) {
    cldb_encode_fixed32(query, UINT32_C(1000000000) + (uint32_t)i);
    if (cldb_filter_policy_key_may_match(
            policy, cldb_slice_make(query, sizeof(query)), filter_slice)) {
      ++false_positives;
    }
  }
  CLDB_EXPECT_TRUE(false_positives <= max_false_positives);

  cldb_filter_policy_destroy(policy);
  cldb_buf_free(&filter);
}

CLDB_TEST(bloom_false_positive_rate) {
  /* 实测约 63%、0.8%、0%；阈值用于容忍哈希分布的正常波动。 */
  check_false_positive_rate(1, 7000);
  check_false_positive_rate(10, 200);
  check_false_positive_rate(30, 50);
}

CLDB_TEST(bloom_create_filter_failures) {
  cldb_filter_policy_t *policy = cldb_new_bloom_filter_policy(10);
  cldb_slice_t key = cldb_slice_make("key", 3);
  cldb_buf_t dst;
  cldb_filter_sink_t sink;
  cldb_filter_sink_t invalid_sink = {0};
  cldb_filter_sink_t failing_sink;

  CLDB_ASSERT_TRUE(policy != NULL);
  cldb_buf_init(&dst);
  CLDB_ASSERT_TRUE(cldb_buf_append(&dst, "xyz", 3));
  sink.arg = &dst;
  sink.append = buf_append;

  CLDB_EXPECT_STATUS(
      CLDB_STATUS_INVALID_ARGUMENT,
      cldb_filter_policy_create_filter(policy, NULL, 1, &sink));
  CLDB_EXPECT_STATUS(
      CLDB_STATUS_INVALID_ARGUMENT,
      cldb_filter_policy_create_filter(policy, &key, 1, NULL));
  CLDB_EXPECT_STATUS(
      CLDB_STATUS_INVALID_ARGUMENT,
      cldb_filter_policy_create_filter(policy, &key, 1, &invalid_sink));
  /* n * bits_per_key 溢出必须在分配和追加之前拒绝。 */
  CLDB_EXPECT_STATUS(
      CLDB_STATUS_INVALID_ARGUMENT,
      cldb_filter_policy_create_filter(policy, &key, SIZE_MAX, &sink));
  CLDB_EXPECT_EQ_UINT(3, dst.size);

  failing_sink.arg = &dst;
  failing_sink.append = failing_append;
  CLDB_EXPECT_STATUS(
      CLDB_STATUS_IO_ERROR,
      cldb_filter_policy_create_filter(policy, &key, 1, &failing_sink));
  CLDB_EXPECT_EQ_UINT(3, dst.size);
  CLDB_EXPECT_TRUE(memcmp(dst.data, "xyz", 3) == 0);

  cldb_filter_policy_destroy(policy);
  cldb_buf_free(&dst);
}

typedef struct {
  cldb_filter_policy_t base; /* 必须为首成员 */
  unsigned create_calls;
  unsigned match_calls;
  unsigned destroy_calls;
} custom_filter_policy_t;

static const char *custom_name(const cldb_filter_policy_t *policy) {
  (void)policy;
  return "test.SimpleFilter";
}

static cldb_status_t custom_create_filter(const cldb_filter_policy_t *policy,
                                          const cldb_slice_t *keys, size_t n,
                                          cldb_filter_sink_t *dst) {
  custom_filter_policy_t *self = (custom_filter_policy_t *)policy;
  static const char kFilter[] = {(char)0xaa, (char)0xbb};

  if (keys == NULL && n != 0) {
    return cldb_status_invalid_argument("custom.create_filter", "keys is NULL");
  }
  if (dst == NULL || dst->append == NULL) {
    return cldb_status_invalid_argument("custom.create_filter",
                                        "destination is invalid");
  }
  ++self->create_calls;
  if (dst->append(dst->arg, kFilter, sizeof(kFilter)) == NULL) {
    return cldb_status_io("custom.create_filter", "append failed");
  }
  return cldb_status_ok();
}

static bool custom_key_may_match(const cldb_filter_policy_t *policy,
                                 cldb_slice_t key, cldb_slice_t filter) {
  custom_filter_policy_t *self = (custom_filter_policy_t *)policy;

  (void)key;
  ++self->match_calls;
  return filter.size == 2 && (uint8_t)filter.data[0] == 0xaa &&
         (uint8_t)filter.data[1] == 0xbb;
}

static void custom_destroy(cldb_filter_policy_t *policy) {
  custom_filter_policy_t *self = (custom_filter_policy_t *)policy;

  ++self->destroy_calls;
}

static const cldb_filter_policy_vtbl_t kCustomVtbl = {
    .name = custom_name,
    .create_filter = custom_create_filter,
    .key_may_match = custom_key_may_match,
    .destroy = custom_destroy,
};

CLDB_TEST(filter_policy_custom_vtable) {
  custom_filter_policy_t custom = {0};
  cldb_buf_t dst;
  cldb_filter_sink_t sink;
  cldb_slice_t keys[] = {cldb_slice_make("a", 1),
                         cldb_slice_make("b", 1)};
  cldb_slice_t filter;

  custom.base.vtbl = &kCustomVtbl;
  cldb_buf_init(&dst);
  sink.arg = &dst;
  sink.append = buf_append;

  CLDB_EXPECT_EQ_STR("test.SimpleFilter",
                     cldb_filter_policy_name(&custom.base));
  CLDB_EXPECT_OK(
      cldb_filter_policy_create_filter(&custom.base, keys, 2, &sink));
  CLDB_EXPECT_EQ_UINT(1, custom.create_calls);
  CLDB_EXPECT_EQ_UINT(2, dst.size);
  CLDB_EXPECT_EQ_INT(0, memcmp(dst.data, "\xaa\xbb", 2));

  filter = cldb_buf_slice(&dst);
  CLDB_EXPECT_TRUE(
      cldb_filter_policy_key_may_match(&custom.base, keys[0], filter));
  CLDB_EXPECT_EQ_UINT(1, custom.match_calls);
  CLDB_EXPECT_EQ_UINT(0, custom.destroy_calls);

  cldb_filter_policy_destroy(&custom.base);
  CLDB_EXPECT_EQ_UINT(1, custom.destroy_calls);
  cldb_filter_policy_destroy(NULL);

  cldb_buf_free(&dst);
}

void cldb_register_bloom_tests(void) {
  CLDB_RUN_TEST(bloom_name_and_factory);
  CLDB_RUN_TEST(bloom_golden_filter_bytes);
  CLDB_RUN_TEST(bloom_empty_and_malformed_filters);
  CLDB_RUN_TEST(bloom_false_positive_rate);
  CLDB_RUN_TEST(bloom_create_filter_failures);
  CLDB_RUN_TEST(filter_policy_custom_vtable);
}
