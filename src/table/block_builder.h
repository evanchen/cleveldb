#ifndef CLDB_SRC_TABLE_BLOCK_BUILDER_H_
#define CLDB_SRC_TABLE_BLOCK_BUILDER_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cleveldb/buf.h"
#include "cleveldb/comparator.h"
#include "cleveldb/options.h"
#include "cleveldb/slice.h"
#include "cleveldb/status.h"

/* 数据块/索引块构建器；对应上游 table/block_builder.h。键按前缀压缩，
 * 每 restart_interval 条写一个保存完整键的 restart 点；Finish 在条目区
 * 尾部追加 restart 偏移数组和数量。输出与上游 BlockBuilder 逐字节一致。
 * 单线程使用；key/value 仅在调用期间借用。 */
typedef struct cldb_block_builder {
  const cldb_comparator_t *comparator; /* 仅引用；用于校验键严格递增，须比 builder 活得久 */
  int restart_interval;                /* >= 1；索引块由 TableBuilder 传 1 */
  cldb_buf_t buffer;                   /* destroy 释放；条目区，Finish 后含 trailer */
  uint32_t *restarts;                  /* destroy 释放；条目区内的升序 restart 偏移 */
  size_t restarts_count;               /* 已记录偏移数；至少为 1 */
  size_t restarts_capacity;            /* restarts 数组容量 */
  int counter;                         /* 距当前 restart 的条目数 */
  bool finished;                       /* Finish 成功后为 true，Add 不再接受 */
  cldb_buf_t last_key;                 /* destroy 释放；最近一次 Add 的完整键 */
} cldb_block_builder_t;

/* 初始化构建器；对应上游 BlockBuilder 构造函数。复制 options 的
 * block_restart_interval 并解析 comparator（NULL 表示字节序）；builder
 * 必须尚未初始化。失败返回 InvalidArgument/IOError，builder 已清零，
 * 仍可安全 destroy。 */
cldb_status_t cldb_block_builder_init(cldb_block_builder_t *builder,
                                      const cldb_options_t *options);

/* 释放内部缓冲；对应上游析构函数。接受 NULL；之后不得再使用 builder。 */
void cldb_block_builder_destroy(cldb_block_builder_t *builder);

/* 清空内容，恢复到刚初始化的状态并保留容量；对应上游 Reset。
 * 要求 builder 已成功初始化。接受 NULL，不做任何事。 */
void cldb_block_builder_reset(cldb_block_builder_t *builder);

/* 自上次 reset 后是否还没有条目；对应上游 empty。空块 Finish 后返回 false，
 * 因为 buffer 已包含 trailer。接受 NULL 并返回 true。 */
bool cldb_block_builder_empty(const cldb_block_builder_t *builder);

/* 最近一次 Add 的完整键；对应上游 LastKey。返回借用视图，在下一次
 * Add/Reset/Destroy 前有效；没有条目或 builder 为 NULL 时返回空 Slice。 */
cldb_slice_t cldb_block_builder_last_key(
    const cldb_block_builder_t *builder);

/* 未压缩块大小估计；对应上游 CurrentSizeEstimate，等于当前条目区 +
 * restart 数组 + 数组长度字段。Finish 前即为最终块大小；Finish 后公式
 * 会重复计入已追加的 trailer。接受 NULL 并返回 0。 */
size_t cldb_block_builder_current_size_estimate(
    const cldb_block_builder_t *builder);

/* 追加一个键值对；对应上游 Add。key 必须按 comparator 严格递增，否则
 * 返回 InvalidArgument；同一键不会写入块。key/value 长度须能装入 varint32，
 * 条目区也不能超过 fixed32 restart 偏移范围；调用期间借用，且不得指向
 * builder 自身缓冲。失败不改动构建器状态；分配失败返回 IOError。 */
cldb_status_t cldb_block_builder_add(cldb_block_builder_t *builder,
                                     cldb_slice_t key, cldb_slice_t value);

/* 追加 restart 数组并标记完成；对应上游 Finish。只允许调用一次，成功后
 * 再调用返回 InvalidArgument。成功时 *block 借用 builder 的 buffer，在
 * Reset/Destroy 前有效；失败时 *block 为空，构建器仍可继续 Add。 */
cldb_status_t cldb_block_builder_finish(cldb_block_builder_t *builder,
                                        cldb_slice_t *block);

#endif /* CLDB_SRC_TABLE_BLOCK_BUILDER_H_ */
