#ifndef CLDB_SRC_DB_WRITE_BATCH_INTERNAL_H_
#define CLDB_SRC_DB_WRITE_BATCH_INTERNAL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cleveldb/buf.h"
#include "cleveldb/slice.h"
#include "cleveldb/write_batch.h"

/* WriteBatch 内部布局与存取器；对应上游 db/write_batch_internal.h。
 * 公开头只暴露不透明类型，DB 恢复/组提交需要直接读写头部。 */

#define CLDB_WRITE_BATCH_HEADER_SIZE 12

struct cldb_write_batch {
  cldb_buf_t rep; /* 12 字节头 + 记录；拥有全部字节 */
};

/* 初始化/释放 stack 对象，供公开 create/destroy 内部及测试使用。失败时
 * 对象保持可安全 free 的空状态。 */
bool cldb_write_batch_init(cldb_write_batch_t *batch);
void cldb_write_batch_free(cldb_write_batch_t *batch);

/* 头部字段；头不足 12 字节时 sequence/count 返回 0。 */
uint64_t cldb_write_batch_sequence(const cldb_write_batch_t *batch);
uint32_t cldb_write_batch_count(const cldb_write_batch_t *batch);

/* 写入 fixed64 sequence；对应上游 SetSequence。仅接受不超过 56 位的
 * 序列号，失败返回 false 且 batch 不变。 */
bool cldb_write_batch_set_sequence(cldb_write_batch_t *batch,
                                   uint64_t sequence);

/* rep 完整字节数与只读视图，均含 12 字节头；对应 ByteSize/Contents。
 * 视图在下次修改或销毁前有效。 */
size_t cldb_write_batch_byte_size(const cldb_write_batch_t *batch);
cldb_slice_t cldb_write_batch_contents(const cldb_write_batch_t *batch);

/* 复制外部内容作为 rep；对应上游 SetContents。允许长度不足 12 字节，
 * 由 iterate 在遍历时报告 Corruption；分配失败返回 false 且 batch 不变。 */
bool cldb_write_batch_set_contents(cldb_write_batch_t *batch,
                                   cldb_slice_t contents);

#endif /* CLDB_SRC_DB_WRITE_BATCH_INTERNAL_H_ */
