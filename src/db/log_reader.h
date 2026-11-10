#ifndef CLDB_SRC_DB_LOG_READER_H_
#define CLDB_SRC_DB_LOG_READER_H_

#include <stdbool.h>
#include <stdint.h>

#include "cleveldb/buf.h"
#include "cleveldb/env.h"
#include "cleveldb/slice.h"
#include "cleveldb/status.h"
#include "db/log_format.h"

/* WAL 读取器；对应上游 db/log_reader.h。从 sequential file 逐条重组逻辑记录，
 * 校验每条物理记录的 CRC，并把损坏经 reporter 上报。不拥有 file，不负责关闭；
 * 同一 reader 只允许单线程使用。 */

/* 损坏/读错误回调；对应上游 Reader::Reporter::Corruption。bytes 是因该错误
 * 丢弃的近似字节数，status 为 Corruption 或底层读错误。reporter 与 arg 的
 * 生命周期必须覆盖 reader。 */
typedef void (*cldb_log_reader_reporter_fn_t)(void *arg, uint64_t bytes,
                                              cldb_status_t status);

typedef struct cldb_log_reader {
  cldb_sequential_file_t *file; /* 仅引用；由调用者保证生命周期 */
  cldb_log_reader_reporter_fn_t reporter; /* 可为 NULL */
  void *reporter_arg;                     /* 原样传给 reporter */
  bool checksum;                          /* 是否校验物理记录 CRC */
  char backing_store[CLDB_LOG_BLOCK_SIZE]; /* 当前块读缓冲 */
  cldb_slice_t buffer;        /* backing_store 中尚未消费的字节 */
  bool eof;                   /* 上次 Read 返回不足一个块 */
  uint64_t last_record_offset; /* 最近返回的逻辑记录起始物理偏移 */
  uint64_t end_of_buffer_offset; /* buffer 末尾对应的文件偏移 */
  uint64_t initial_offset;       /* 跳过此偏移之前的物理记录 */
  bool resyncing; /* 初始偏移后尚未找到 FULL/FIRST */
} cldb_log_reader_t;

cldb_status_t cldb_log_reader_create(cldb_sequential_file_t *file,
                                     cldb_log_reader_reporter_fn_t reporter,
                                     void *reporter_arg, bool checksum,
                                     uint64_t initial_offset,
                                     cldb_log_reader_t **out);
void cldb_log_reader_destroy(cldb_log_reader_t *reader);

/* 读取下一条逻辑记录。返回 false 表示 EOF、底层读失败或内存不足；损坏通过
 * reporter 上报后继续尝试。record 指向 reader 内部缓冲或 scratch，在下一次
 * 读取、修改 scratch 或销毁 reader 之前有效。scratch 必须已初始化，函数会
 * 清空并复用它拼装跨块记录。 */
bool cldb_log_reader_read_record(cldb_log_reader_t *reader,
                                 cldb_slice_t *record,
                                 cldb_buf_t *scratch);

/* 最近一条返回记录的起始偏移；尚未返回记录时为 0。 */
uint64_t cldb_log_reader_last_record_offset(const cldb_log_reader_t *reader);

#endif /* CLDB_SRC_DB_LOG_READER_H_ */
