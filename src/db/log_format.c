#include "db/log_format.h"

#include <stddef.h>

/* 下列值与格式一起是磁盘契约，不能随实现调整。 */
_Static_assert(CLDB_LOG_BLOCK_SIZE == 32768, "WAL block size must be 32768");
_Static_assert(CLDB_LOG_HEADER_SIZE == 7, "WAL header size must be 7");
_Static_assert(CLDB_LOG_MAX_RECORD_TYPE == CLDB_LOG_RECORD_TYPE_LAST,
               "WAL record types must end at LAST");
_Static_assert(CLDB_LOG_TYPE_CRC_COUNT == 5, "WAL type CRC table size");

/* 单字节 type 的 CRC32C；对应上游 InitTypeCrc 的预计算结果。 */
static const uint32_t kTypeCrc[CLDB_LOG_TYPE_CRC_COUNT] = {
    UINT32_C(0x527d5351), /* Zero */
    UINT32_C(0xa016d052), /* Full */
    UINT32_C(0xb34623a6), /* First */
    UINT32_C(0x412da0a5), /* Middle */
    UINT32_C(0x95e7c44e), /* Last */
};

void cldb_log_init_type_crc(uint32_t type_crc[CLDB_LOG_TYPE_CRC_COUNT]) {
  size_t i;

  if (type_crc == NULL) {
    return;
  }
  for (i = 0; i < CLDB_LOG_TYPE_CRC_COUNT; ++i) {
    type_crc[i] = kTypeCrc[i];
  }
}
