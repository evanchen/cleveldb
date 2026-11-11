#ifndef CLDB_INCLUDE_CLEVELDB_WRITE_BATCH_H_
#define CLDB_INCLUDE_CLEVELDB_WRITE_BATCH_H_

#include <stdbool.h>
#include <stddef.h>

#include "cleveldb/slice.h"
#include "cleveldb/status.h"

/* 批量写；对应上游 include/leveldb/write_batch.h 与 db/write_batch.cc。
 * 字节格式为 12 字节头（fixed64 sequence、fixed32 count）后接操作记录：
 * Put 为 tag(1) + varint32 key 长度 + key + varint32 value 长度 + value，
 * Delete 为 tag(0) + varint32 key 长度 + key。key/value 均复制进内部缓冲，
 * 调用者的 Slice 无需在调用结束后保持有效。 */
typedef struct cldb_write_batch cldb_write_batch_t;

/* 创建空 batch（头中 sequence/count 均为 0）；分配失败返回 NULL。 */
cldb_write_batch_t *cldb_write_batch_create(void);

/* 释放 batch；接受 NULL。 */
void cldb_write_batch_destroy(cldb_write_batch_t *batch);

/* 清空全部记录并把 sequence/count 归零；失败返回 false 且 batch 不变。
 * 仅在内部缓冲无法恢复 12 字节头时失败。 */
bool cldb_write_batch_clear(cldb_write_batch_t *batch);

/* 追加 Put/Delete 记录；成功时 count 加一。失败返回 false 且 batch 不变：
 * batch 无效、key/value 非法、长度超过 UINT32_MAX、count 已达上限或分配
 * 失败。 */
bool cldb_write_batch_put(cldb_write_batch_t *batch, cldb_slice_t key,
                          cldb_slice_t value);
bool cldb_write_batch_delete(cldb_write_batch_t *batch, cldb_slice_t key);

/* 把 source 去掉 12 字节头后的记录追加到 batch，并累加 count；对应上游
 * Append。目标 sequence 保持不变，source 不被修改；source == batch 时
 * 安全，结果相当于记录与 count 都翻倍。失败返回 false 且 batch 不变：
 * 参数无效、source 头不足 12 字节、count 相加溢出或分配失败。 */
bool cldb_write_batch_append(cldb_write_batch_t *batch,
                             const cldb_write_batch_t *source);

/* 当前批次的近似内存占用，等于含 12 字节头的 rep 长度；对应上游
 * ApproximateSize。接受 NULL 并返回 0。 */
size_t cldb_write_batch_approximate_size(const cldb_write_batch_t *batch);

/* 遍历回调接口；对应上游 WriteBatch::Handler。base 必须为首成员，派生
 * 对象通过初始化 vtbl 后把 handler 指针交给 iterate。回调收到的 key/value
 * 只在本次调用期间有效；返回非 OK 状态会立即终止遍历并原样返回。 */
typedef struct cldb_write_batch_handler cldb_write_batch_handler_t;

typedef struct cldb_write_batch_handler_vtbl {
  cldb_status_t (*put)(cldb_write_batch_handler_t *handler,
                       cldb_slice_t key, cldb_slice_t value);
  cldb_status_t (*delete)(cldb_write_batch_handler_t *handler,
                          cldb_slice_t key);
} cldb_write_batch_handler_vtbl_t;

struct cldb_write_batch_handler {
  const cldb_write_batch_handler_vtbl_t *vtbl; /* 必须非 NULL */
};

/* 按记录顺序同步回调 handler；对应上游 Iterate。每条记录交给 handler，
 * sequence 由 handler 自己维护（MemTableInserter 由 MemTable 模块提供）。batch
 * 头不足 12 字节、记录截断、tag 未知、实际记录数与 count 不符时返回
 * Corruption；参数无效返回 InvalidArgument。 */
cldb_status_t cldb_write_batch_iterate(
    const cldb_write_batch_t *batch, cldb_write_batch_handler_t *handler);

#endif /* CLDB_INCLUDE_CLEVELDB_WRITE_BATCH_H_ */
