#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "harness.h"
#include "util/crc32c.h"
#include "util/random.h"

/* 位级参考实现：反射多项式 0x82f63b78，输入/输出均异或全 1。
 * 用于独立校验查表与 4 路并行实现的分组、对齐路径。 */
static uint32_t ref_extend(uint32_t init_crc, const uint8_t *data,
                           size_t n) {
  static const uint32_t kPoly = UINT32_C(0x82f63b78);
  uint32_t crc = init_crc ^ UINT32_C(0xffffffff);
  size_t i;
  int bit;

  for (i = 0; i < n; ++i) {
    crc ^= (uint32_t)data[i];
    for (bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ ((crc & 1) ? kPoly : 0);
    }
  }
  return crc ^ UINT32_C(0xffffffff);
}

/* 上游 CRC.StandardResults 的 RFC 3720 向量，另加 CRC-32C 标准校验值。 */
CLDB_TEST(crc32c_standard_results) {
  static const uint8_t kIscsiData[48] = {
      0x01, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x04, 0x00, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x18, 0x28,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00,
  };
  char buf[48];
  size_t i;

  memset(buf, 0, 32);
  CLDB_ASSERT_EQ_UINT(UINT32_C(0x8a9136aa), cldb_crc32c_value(buf, 32));

  memset(buf, 0xff, 32);
  CLDB_ASSERT_EQ_UINT(UINT32_C(0x62a8ab43), cldb_crc32c_value(buf, 32));

  for (i = 0; i < 32; ++i) {
    buf[i] = (char)i;
  }
  CLDB_ASSERT_EQ_UINT(UINT32_C(0x46dd794e), cldb_crc32c_value(buf, 32));

  for (i = 0; i < 32; ++i) {
    buf[i] = (char)(31 - i);
  }
  CLDB_ASSERT_EQ_UINT(UINT32_C(0x113fdb5c), cldb_crc32c_value(buf, 32));

  memcpy(buf, kIscsiData, sizeof(kIscsiData));
  CLDB_ASSERT_EQ_UINT(UINT32_C(0xd9963a56),
                      cldb_crc32c_value(buf, sizeof(kIscsiData)));

  CLDB_ASSERT_EQ_UINT(UINT32_C(0xe3069283),
                      cldb_crc32c_value("123456789", 9));
}

/* 空输入不读取 data，Extend 原样返回 init_crc。 */
CLDB_TEST(crc32c_empty) {
  const uint32_t kInit = UINT32_C(0x12345678);

  CLDB_EXPECT_EQ_UINT(0, cldb_crc32c_value(NULL, 0));
  CLDB_EXPECT_EQ_UINT(kInit, cldb_crc32c_extend(kInit, NULL, 0));
  CLDB_EXPECT_EQ_UINT(0, cldb_crc32c_value("", 0));
  CLDB_EXPECT_EQ_UINT(kInit, cldb_crc32c_extend(kInit, "", 0));
}

/* 上游 CRC.Extend：分段计算等于一次性计算，且不同内容不相等。 */
CLDB_TEST(crc32c_extend_incremental) {
  const uint32_t kFull = cldb_crc32c_value("hello world", 11);
  const uint32_t kPrefix = cldb_crc32c_value("hello ", 6);

  CLDB_ASSERT_EQ_UINT(kFull, cldb_crc32c_extend(kPrefix, "world", 5));
  CLDB_EXPECT_TRUE(kFull != cldb_crc32c_value("foo", 3));
}

/* 覆盖起始地址 0..7 偏移和 0..128 长度，验证对齐前缀、16 字节并行
 * 与 4 字节收尾路径都和位级参考实现一致。 */
CLDB_TEST(crc32c_alignment_and_lengths) {
  uint8_t buffer[160];
  size_t i;
  size_t offset;
  size_t length;

  for (i = 0; i < sizeof(buffer); ++i) {
    buffer[i] = (uint8_t)(i * 37u + 11u);
  }
  for (offset = 0; offset < 8; ++offset) {
    const uint8_t *start = buffer + offset;
    uint32_t init = UINT32_C(0x9e3779b9) ^ (uint32_t)offset;

    for (length = 0; length <= 128; ++length) {
      CLDB_ASSERT_EQ_UINT(ref_extend(init, start, length),
                          cldb_crc32c_extend(init, (const char *)start,
                                             length));
    }
  }
}

/* 大缓冲一次性计算必须等于逐字节与随机分段的结果。 */
CLDB_TEST(crc32c_large_incremental) {
  static uint8_t data[65537];
  const size_t kSize = sizeof(data);
  cldb_random_t rnd;
  uint32_t one_shot;
  uint32_t crc;
  size_t i;
  size_t pos;

  cldb_random_init(&rnd, 301);
  for (i = 0; i < kSize; ++i) {
    data[i] = (uint8_t)(cldb_random_next(&rnd) & UINT32_C(0xff));
  }

  one_shot = cldb_crc32c_value((const char *)data, kSize);
  CLDB_ASSERT_EQ_UINT(ref_extend(0, data, kSize), one_shot);

  crc = 0;
  for (i = 0; i < kSize; ++i) {
    crc = cldb_crc32c_extend(crc, (const char *)(data + i), 1);
  }
  CLDB_ASSERT_EQ_UINT(one_shot, crc);

  cldb_random_init(&rnd, 301);
  crc = 0;
  pos = 0;
  while (pos < kSize) {
    size_t step = (size_t)cldb_random_uniform(&rnd, 4096) + 1;

    if (step > kSize - pos) {
      step = kSize - pos;
    }
    crc = cldb_crc32c_extend(crc, (const char *)(data + pos), step);
    pos += step;
  }
  CLDB_ASSERT_EQ_UINT(one_shot, crc);
}

/* 上游 CRC.Mask：掩码/解掩码互逆，并覆盖已知常量与随机 CRC。 */
CLDB_TEST(crc32c_mask_roundtrip) {
  static const uint32_t kValues[] = {
      0, 1, UINT32_C(0xa282ead8), UINT32_C(0x7fffffff),
      UINT32_C(0x80000000), UINT32_MAX,
  };
  cldb_random_t rnd;
  uint32_t crc;
  size_t i;

  for (i = 0; i < sizeof(kValues) / sizeof(kValues[0]); ++i) {
    CLDB_ASSERT_EQ_UINT(kValues[i],
                        cldb_crc32c_unmask(cldb_crc32c_mask(kValues[i])));
  }

  crc = cldb_crc32c_value("foo", 3);
  CLDB_ASSERT_TRUE(crc != cldb_crc32c_mask(crc));
  CLDB_ASSERT_TRUE(crc != cldb_crc32c_mask(cldb_crc32c_mask(crc)));
  CLDB_ASSERT_EQ_UINT(crc,
                      cldb_crc32c_unmask(cldb_crc32c_mask(crc)));
  CLDB_ASSERT_EQ_UINT(
      crc, cldb_crc32c_unmask(cldb_crc32c_unmask(
               cldb_crc32c_mask(cldb_crc32c_mask(crc)))));
  CLDB_EXPECT_EQ_UINT(CLDB_CRC32C_MASK_DELTA, cldb_crc32c_mask(0));
  CLDB_EXPECT_EQ_UINT(0, cldb_crc32c_unmask(CLDB_CRC32C_MASK_DELTA));

  cldb_random_init(&rnd, 301);
  for (i = 0; i < 100000; ++i) {
    crc = cldb_random_next(&rnd);
    CLDB_ASSERT_EQ_UINT(crc,
                        cldb_crc32c_unmask(cldb_crc32c_mask(crc)));
  }
}

void cldb_register_crc32c_tests(void) {
  CLDB_RUN_TEST(crc32c_standard_results);
  CLDB_RUN_TEST(crc32c_empty);
  CLDB_RUN_TEST(crc32c_extend_incremental);
  CLDB_RUN_TEST(crc32c_alignment_and_lengths);
  CLDB_RUN_TEST(crc32c_large_incremental);
  CLDB_RUN_TEST(crc32c_mask_roundtrip);
}
