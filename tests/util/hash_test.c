#include <stdint.h>
#include <string.h>

#include "harness.h"
#include "util/hash.h"

/* 上游 hash_test.cc 的字节向量；验证负数/非 ASCII 字节按无符号处理。 */
CLDB_TEST(hash_signed_unsigned_issue) {
  static const uint8_t kData1[] = {0x62};
  static const uint8_t kData2[] = {0xc3, 0x97};
  static const uint8_t kData3[] = {0xe2, 0x99, 0xa5};
  static const uint8_t kData4[] = {0xe1, 0x80, 0xb9, 0x32};
  static const uint8_t kData5[] = {
      0x01, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00,
      0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x18, 0x28, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  };

  CLDB_ASSERT_EQ_UINT(UINT32_C(0xbc9f1d34),
                      cldb_hash(NULL, 0, UINT32_C(0xbc9f1d34)));
  CLDB_ASSERT_EQ_UINT(
      UINT32_C(0xef1345c4),
      cldb_hash((const char *)kData1, sizeof(kData1), UINT32_C(0xbc9f1d34)));
  CLDB_ASSERT_EQ_UINT(
      UINT32_C(0x5b663814),
      cldb_hash((const char *)kData2, sizeof(kData2), UINT32_C(0xbc9f1d34)));
  CLDB_ASSERT_EQ_UINT(
      UINT32_C(0x323c078f),
      cldb_hash((const char *)kData3, sizeof(kData3), UINT32_C(0xbc9f1d34)));
  CLDB_ASSERT_EQ_UINT(
      UINT32_C(0xed21633a),
      cldb_hash((const char *)kData4, sizeof(kData4), UINT32_C(0xbc9f1d34)));
  CLDB_ASSERT_EQ_UINT(
      UINT32_C(0xf333dabb),
      cldb_hash((const char *)kData5, sizeof(kData5), UINT32_C(0x12345678)));
}

CLDB_TEST(hash_empty) {
  CLDB_EXPECT_EQ_UINT(0, cldb_hash(NULL, 0, 0));
  CLDB_EXPECT_EQ_UINT(UINT32_C(0xbc9f1d34),
                      cldb_hash(NULL, 0, UINT32_C(0xbc9f1d34)));
  CLDB_EXPECT_EQ_UINT(UINT32_MAX, cldb_hash("", 0, UINT32_MAX));
  CLDB_EXPECT_EQ_UINT(123, cldb_hash("ignored", 0, 123));
}

/* 尾部 0..3 字节和 4 字节块的 golden，覆盖上游 switch 的每个分支。 */
CLDB_TEST(hash_tail_golden) {
  static const uint8_t kData[] = {0x01, 0xc0, 0x00, 0x00, 0x00,
                                  0x00, 0x00, 0x00, 0x14};
  static const uint32_t kExpected[] = {
      UINT32_C(0xbc9f1d34), UINT32_C(0xaaafc6d2), UINT32_C(0x3175c5d8),
      UINT32_C(0xaf4d2425), UINT32_C(0x77b8dac3), UINT32_C(0xc1e4205c),
      UINT32_C(0x85c255e4), UINT32_C(0x727cf766), UINT32_C(0xf0196dfd),
      UINT32_C(0x1c96d07d),
  };
  size_t n;

  for (n = 0; n <= sizeof(kData); ++n) {
    CLDB_ASSERT_EQ_UINT(
        kExpected[n],
        cldb_hash((const char *)kData, n, UINT32_C(0xbc9f1d34)));
  }
}

/* 逐字节解码，因此任意对齐都应得到相同结果（避免未对齐访问）。 */
CLDB_TEST(hash_unaligned_input) {
  static const uint8_t kData[] = {0x01, 0xc0, 0x00, 0x00, 0x00,
                                  0x00, 0x00, 0x00, 0x14};
  uint8_t buffer[sizeof(kData) + 4];
  size_t offset;

  for (offset = 0; offset < 4; ++offset) {
    memcpy(buffer + offset, kData, sizeof(kData));
    CLDB_EXPECT_EQ_UINT(
        UINT32_C(0x1c96d07d),
        cldb_hash((const char *)buffer + offset, sizeof(kData),
                  UINT32_C(0xbc9f1d34)));
  }
}

CLDB_TEST(hash_seed_and_determinism) {
  static const char kData[] = "abcdefg";
  uint32_t first = cldb_hash(kData, sizeof(kData) - 1, 1);

  CLDB_EXPECT_EQ_UINT(first, cldb_hash(kData, sizeof(kData) - 1, 1));
  CLDB_EXPECT_TRUE(first != cldb_hash(kData, sizeof(kData) - 1, 2));
}

void cldb_register_hash_tests(void) {
  CLDB_RUN_TEST(hash_signed_unsigned_issue);
  CLDB_RUN_TEST(hash_empty);
  CLDB_RUN_TEST(hash_tail_golden);
  CLDB_RUN_TEST(hash_unaligned_input);
  CLDB_RUN_TEST(hash_seed_and_determinism);
}
