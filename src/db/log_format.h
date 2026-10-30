#ifndef CLDB_SRC_DB_LOG_FORMAT_H_
#define CLDB_SRC_DB_LOG_FORMAT_H_

#include <stdint.h>

/* WAL 物理格式；对应上游 db/log_format.h。
 * 每个 block 为 32 KB，块尾不足一个 header 时补零：
 *   | header(7) + payload | header(7) + payload | ... | 0..6 字节零填充 |
 * header 为 masked_crc32c(4, 小端)、length(2, 小端)、type(1)。
 * 逻辑记录跨块时拆成 FIRST/MIDDLE.../LAST，整条位于块内时为 FULL。 */

#define CLDB_LOG_BLOCK_SIZE 32768
#define CLDB_LOG_MAX_RECORD_TYPE 4

#define CLDB_LOG_CRC_SIZE 4
#define CLDB_LOG_LENGTH_SIZE 2
#define CLDB_LOG_TYPE_SIZE 1
#define CLDB_LOG_CRC_OFFSET 0
#define CLDB_LOG_LENGTH_OFFSET (CLDB_LOG_CRC_OFFSET + CLDB_LOG_CRC_SIZE)
#define CLDB_LOG_TYPE_OFFSET (CLDB_LOG_LENGTH_OFFSET + CLDB_LOG_LENGTH_SIZE)
#define CLDB_LOG_HEADER_SIZE (CLDB_LOG_TYPE_OFFSET + CLDB_LOG_TYPE_SIZE)
#define CLDB_LOG_TYPE_CRC_COUNT (CLDB_LOG_MAX_RECORD_TYPE + 1)

/* 物理记录类型；对应上游 RecordType。Zero 仅为预分配文件保留，Writer 不生成。 */
typedef enum cldb_log_record_type {
  CLDB_LOG_RECORD_TYPE_ZERO = 0,
  CLDB_LOG_RECORD_TYPE_FULL = 1,
  CLDB_LOG_RECORD_TYPE_FIRST = 2,
  CLDB_LOG_RECORD_TYPE_MIDDLE = 3,
  CLDB_LOG_RECORD_TYPE_LAST = 4
} cldb_log_record_type_t;

/* 填入每种 type 单字节的 CRC32C；对应上游 InitTypeCrc。写入物理记录时以
 * type_crc[type] 作为 payload CRC 的初值，使 CRC 覆盖 type 字节。
 * type_crc 必须至少有 CLDB_LOG_TYPE_CRC_COUNT 个元素。 */
void cldb_log_init_type_crc(uint32_t type_crc[CLDB_LOG_TYPE_CRC_COUNT]);

#endif /* CLDB_SRC_DB_LOG_FORMAT_H_ */
