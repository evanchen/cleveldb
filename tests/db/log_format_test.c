#include <stdint.h>
#include <string.h>

#include "db/log_format.h"
#include "harness.h"
#include "util/coding.h"
#include "util/crc32c.h"

/* WAL 常量、记录类型与 type CRC；对应上游 db/log_format.h 和 InitTypeCrc。 */

CLDB_TEST(log_format_constants) {
  CLDB_EXPECT_EQ_INT(32768, CLDB_LOG_BLOCK_SIZE);
  CLDB_EXPECT_EQ_INT(7, CLDB_LOG_HEADER_SIZE);
  CLDB_EXPECT_EQ_INT(4, CLDB_LOG_MAX_RECORD_TYPE);
  CLDB_EXPECT_EQ_INT(5, CLDB_LOG_TYPE_CRC_COUNT);
  CLDB_EXPECT_EQ_INT(4, CLDB_LOG_CRC_SIZE);
  CLDB_EXPECT_EQ_INT(2, CLDB_LOG_LENGTH_SIZE);
  CLDB_EXPECT_EQ_INT(1, CLDB_LOG_TYPE_SIZE);
  CLDB_EXPECT_EQ_INT(0, CLDB_LOG_CRC_OFFSET);
  CLDB_EXPECT_EQ_INT(4, CLDB_LOG_LENGTH_OFFSET);
  CLDB_EXPECT_EQ_INT(6, CLDB_LOG_TYPE_OFFSET);

  CLDB_EXPECT_EQ_INT(0, CLDB_LOG_RECORD_TYPE_ZERO);
  CLDB_EXPECT_EQ_INT(1, CLDB_LOG_RECORD_TYPE_FULL);
  CLDB_EXPECT_EQ_INT(2, CLDB_LOG_RECORD_TYPE_FIRST);
  CLDB_EXPECT_EQ_INT(3, CLDB_LOG_RECORD_TYPE_MIDDLE);
  CLDB_EXPECT_EQ_INT(4, CLDB_LOG_RECORD_TYPE_LAST);
}

CLDB_TEST(log_format_type_crc) {
  static const uint32_t kExpected[CLDB_LOG_TYPE_CRC_COUNT] = {
      UINT32_C(0x527d5351), /* Zero */
      UINT32_C(0xa016d052), /* Full */
      UINT32_C(0xb34623a6), /* First */
      UINT32_C(0x412da0a5), /* Middle */
      UINT32_C(0x95e7c44e), /* Last */
  };
  uint32_t type_crc[CLDB_LOG_TYPE_CRC_COUNT];
  size_t i;

  memset(type_crc, 0xff, sizeof(type_crc));
  cldb_log_init_type_crc(type_crc);
  for (i = 0; i < CLDB_LOG_TYPE_CRC_COUNT; ++i) {
    char type_byte = (char)i;

    CLDB_EXPECT_EQ_UINT(kExpected[i], type_crc[i]);
    CLDB_EXPECT_EQ_UINT(cldb_crc32c_value(&type_byte, 1), type_crc[i]);
  }
  cldb_log_init_type_crc(NULL);
}

/* 手写 FULL 记录：CRC 0x5857b90b、长度 5、类型 1、payload "hello"。
 * 不依赖 Writer，直接检查物理布局和 CRC 覆盖范围。 */
CLDB_TEST(log_format_hexdump) {
  static const char kRecord[] = {
      (char)0x0b, (char)0xb9, (char)0x57, (char)0x58, (char)0x05,
      (char)0x00, (char)0x01, 'h',         'e',         'l',
      'l',         'o',
  };
  uint32_t type_crc[CLDB_LOG_TYPE_CRC_COUNT];
  uint32_t stored_crc;
  uint32_t length;
  uint32_t raw_crc;
  uint8_t type;

  cldb_log_init_type_crc(type_crc);
  stored_crc = cldb_decode_fixed32(kRecord + CLDB_LOG_CRC_OFFSET);
  length = (uint32_t)(uint8_t)kRecord[CLDB_LOG_LENGTH_OFFSET] |
           ((uint32_t)(uint8_t)kRecord[CLDB_LOG_LENGTH_OFFSET + 1] << 8);
  type = (uint8_t)kRecord[CLDB_LOG_TYPE_OFFSET];

  CLDB_EXPECT_EQ_UINT(12, sizeof(kRecord));
  CLDB_EXPECT_EQ_UINT(UINT32_C(0x5857b90b), stored_crc);
  CLDB_EXPECT_EQ_UINT(5, length);
  CLDB_EXPECT_EQ_INT(CLDB_LOG_RECORD_TYPE_FULL, type);
  CLDB_EXPECT_EQ_UINT(sizeof(kRecord), CLDB_LOG_HEADER_SIZE + length);
  CLDB_EXPECT_EQ_INT(0, memcmp(kRecord + CLDB_LOG_HEADER_SIZE, "hello", 5));

  raw_crc = cldb_crc32c_extend(type_crc[type], kRecord + CLDB_LOG_HEADER_SIZE,
                               length);
  CLDB_EXPECT_EQ_UINT(raw_crc, cldb_crc32c_unmask(stored_crc));
}

void cldb_register_log_format_tests(void) {
  CLDB_RUN_TEST(log_format_constants);
  CLDB_RUN_TEST(log_format_type_crc);
  CLDB_RUN_TEST(log_format_hexdump);
}
