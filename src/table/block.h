#ifndef CLDB_SRC_TABLE_BLOCK_H_
#define CLDB_SRC_TABLE_BLOCK_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cleveldb/comparator.h"
#include "cleveldb/iterator.h"
#include "cleveldb/slice.h"
#include "cleveldb/status.h"

/* 块数据来源与所有权提示；对应上游 format.h 的 BlockContents。 */
typedef struct cldb_block_contents {
  cldb_slice_t data;    /* 块字节；heap_allocated 为真时由 Block 接管并 free */
  bool cachable;        /* 数据可放入 block cache，由使用者决定是否缓存 */
  bool heap_allocated;  /* data 来自堆分配，Destroy 时释放 */
} cldb_block_contents_t;

/* 只读数据块/索引块；对应上游 table/block.h 的 Block。构造时解析尾部
 * restart 数组位置并做基本大小校验；块内条目在迭代时惰性解码。 */
typedef struct cldb_block {
  cldb_slice_t data;      /* 借用或拥有；size 为 0 表示头部损坏 */
  size_t restart_offset;  /* restart 数组起点；条目区为 [0, restart_offset) */
  uint32_t num_restarts;  /* restart 元素个数 */
  bool heap_allocated;    /* 为真时 Destroy 释放 data.data */
  bool cachable;          /* 来自 BlockContents，仅作提示 */
} cldb_block_t;

/* 初始化并接管 contents；对应上游 Block(const BlockContents&)。成功时
 * data 生命周期转给 block，heap_allocated 为真则 Destroy 负责释放。头部
 * 损坏（不足 4 字节或 restart 数量越界）也返回 OK，只把 data.size 置 0，
 * 由 NewIterator 报告 Corruption；参数无效返回 InvalidArgument 且不接管。
 * block 必须尚未初始化。 */
cldb_status_t cldb_block_init(cldb_block_t *block,
                              cldb_block_contents_t contents);

/* 销毁块；接受 NULL，重复调用安全。仅释放 heap_allocated 的数据。 */
void cldb_block_destroy(cldb_block_t *block);

/* 块字节数；对应上游 Block::size，头部损坏时返回 0。接受 NULL 返回 0。 */
size_t cldb_block_size(const cldb_block_t *block);

/* 是否可缓存；对应上游 BlockContents::cachable。接受 NULL 返回 false。 */
bool cldb_block_is_cachable(const cldb_block_t *block);

/* 创建堆迭代器，调用者用 cldb_iter_destroy 释放；对应上游 Block::NewIterator
 * （返回不到结束的迭代器时走空/错误迭代器路径）。comparator 为 NULL 时用
 * 字节序比较器。空块返回 OK 的空迭代器；头部损坏返回 Corruption 错误迭代器。
 * block 不转移所有权，block 及其数据必须活到迭代器销毁；分配失败返回 NULL。
 * 键/值借用块数据，仅在迭代器下一次移动或销毁前有效。 */
cldb_iterator_t *cldb_block_new_iterator(const cldb_block_t *block,
                                         const cldb_comparator_t *comparator);

#endif /* CLDB_SRC_TABLE_BLOCK_H_ */
