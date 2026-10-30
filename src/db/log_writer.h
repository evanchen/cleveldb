#ifndef CLDB_SRC_DB_LOG_WRITER_H_
#define CLDB_SRC_DB_LOG_WRITER_H_

#include <stdint.h>

#include "cleveldb/env.h"
#include "cleveldb/slice.h"
#include "cleveldb/status.h"
#include "db/log_format.h"

/* WAL 写入器；对应上游 db/log_writer.h。把逻辑记录按 32 KB 块切分、计算每条
 * 物理记录的 CRC 后追加到 dest。不拥有 dest，不负责 Close/Sync；同一 writer
 * 只允许单线程使用。 */

typedef struct cldb_log_writer {
  cldb_writable_file_t *dest; /* 仅引用；由调用者保证生命周期 */
  int block_offset;           /* 当前块内偏移；0..CLDB_LOG_BLOCK_SIZE */
  uint32_t type_crc[CLDB_LOG_TYPE_CRC_COUNT]; /* 各记录类型的 CRC32C 初值 */
} cldb_log_writer_t;

/* 创建追加到空 dest 的 writer；dest 必须比 writer 活得久。成功时 *out 归
 * 调用者并需 destroy；失败返回非 OK 且 *out = NULL。 */
cldb_status_t cldb_log_writer_create(cldb_writable_file_t *dest,
                                     cldb_log_writer_t **out);

/* 同 create，但 dest 已有 dest_length 字节，从该偏移继续分片；对应上游
 * Writer(dest, dest_length)，用于重开 WAL 追加。 */
cldb_status_t cldb_log_writer_create_with_length(cldb_writable_file_t *dest,
                                                 uint64_t dest_length,
                                                 cldb_log_writer_t **out);

/* 释放 writer；不 Close/Sync dest。接受 NULL。 */
void cldb_log_writer_destroy(cldb_log_writer_t *writer);

/* 追加一条逻辑记录；空记录也会写一条 FULL(length=0)。返回 dest 的 Append/
 * Flush 错误；出错后 writer 状态可能已推进，调用者不应继续使用。 */
cldb_status_t cldb_log_writer_add_record(cldb_log_writer_t *writer,
                                         cldb_slice_t record);

#endif /* CLDB_SRC_DB_LOG_WRITER_H_ */
