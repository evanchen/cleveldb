#ifndef CLDB_SRC_DB_SNAPSHOT_H_
#define CLDB_SRC_DB_SNAPSHOT_H_

#include <stdbool.h>

#include "db/dbformat.h"

/* 快照链表；对应上游 db/snapshot.h。快照只保存 sequence，不复制数据；
 * 链表本身不做并发保护，调用者必须在 DB mutex 下操作所有修改接口。
 * 快照释放后，其 sequence 可能不再阻止 compaction 丢弃对应版本。 */

typedef struct cldb_snapshot cldb_snapshot_t;
typedef struct cldb_snapshot_list cldb_snapshot_list_t;

struct cldb_snapshot {
  cldb_sequence_number_t sequence; /* 创建后不变；可见版本上界 */
  cldb_snapshot_t *prev;           /* 前驱；链表修改需外部串行化 */
  cldb_snapshot_t *next;           /* 后继；链表修改需外部串行化 */
#if CLDB_DEBUG
  cldb_snapshot_list_t *list; /* 所属链表；仅用于 debug 校验 */
#endif
};

struct cldb_snapshot_list {
  cldb_snapshot_t head; /* 哨兵；空链时 prev/next 指向自身 */
};

/* 初始化空链；对应上游 SnapshotList()。接受 NULL；之后才能 new/delete。 */
void cldb_snapshot_list_init(cldb_snapshot_list_t *list);

/* 是否为空。链表未初始化是调用者错误；NULL 视为空并返回 true。 */
bool cldb_snapshot_list_empty(const cldb_snapshot_list_t *list);

/* 分配并追加到尾部（最新）的快照；对应上游 New。要求 sequence 不大于
 * 最大序列号，且不小于当前 newest 的 sequence（debug 断言）。链表拥有返回
 * 的快照，调用者只能通过 delete 释放；参数无效或分配失败返回 NULL。 */
cldb_snapshot_t *cldb_snapshot_list_new(cldb_snapshot_list_t *list,
                                        cldb_sequence_number_t sequence);

/* 从链表摘除并释放快照；对应上游 Delete。接受 const，因为公开释放接口
 * 持有 const 快照指针；每个快照必须恰好调用一次，重复删除会 use-after-free。
 * list/snapshot 为 NULL 时无操作；snapshot 必须属于 list（debug 断言）。 */
void cldb_snapshot_list_delete(cldb_snapshot_list_t *list,
                               const cldb_snapshot_t *snapshot);

/* 最老/最新快照；对应上游 oldest/newest。上游对空链断言，这里改为返回
 * NULL；list 必须已初始化，调用者仍应先检查 empty。 */
const cldb_snapshot_t *cldb_snapshot_list_oldest(
    const cldb_snapshot_list_t *list);
const cldb_snapshot_t *cldb_snapshot_list_newest(
    const cldb_snapshot_list_t *list);

/* 快照的 sequence；对应上游 SnapshotImpl::sequence_number。快照创建后
 * 只读；NULL 返回 0。 */
cldb_sequence_number_t cldb_snapshot_sequence(
    const cldb_snapshot_t *snapshot);

#endif /* CLDB_SRC_DB_SNAPSHOT_H_ */
