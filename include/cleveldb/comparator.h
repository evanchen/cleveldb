#ifndef CLDB_INCLUDE_CLEVELDB_COMPARATOR_H_
#define CLDB_INCLUDE_CLEVELDB_COMPARATOR_H_

#include <stddef.h>

#include "cleveldb/slice.h"

/* 键比较器接口；对应上游 leveldb::Comparator。comparator 必须非 NULL，
 * vtbl 必须指向所有方法非 NULL 的静态表。所有方法必须线程安全，因为 DB
 * 可能并发调用；Slice 参数均为借用。具体实现把 cldb_comparator_t 放在
 * 结构体首成员，并在回调中把 comparator 指针转回外层类型。Bytewise 单例
 * 由库拥有，自定义 comparator 由调用者保证生命周期覆盖使用者。 */
typedef struct cldb_comparator cldb_comparator_t;

typedef struct cldb_comparator_vtbl {
  /* 三路比较；对应上游 Compare。a/b 为 borrowed，只在本次调用内有效。
   * 返回值只要求符号：a<b 为负、相等为 0、a>b 为正。必须是全序，且与
   * name 对应的排序规则一致。 */
  int (*compare)(const cldb_comparator_t *comparator, cldb_slice_t a,
                 cldb_slice_t b);

  /* 返回 comparator 名称；对应上游 Name。返回 borrowed 静态字符串，
   * 内容在整个 comparator 生命周期内不变。名称会写入 MANIFEST 并在
   * 重新打开时校验，实现改变排序时必须换名称；"leveldb." 前缀保留。 */
  const char *(*name)(const cldb_comparator_t *comparator);

  /* 尝试缩短 start；对应上游 FindShortestSeparator。start 为调用者所有的
   * 可写缓冲，limit 为 borrowed。仅在 compare(start, limit) < 0 时调用；
   * 允许原样返回。进入时 *start_size 为输入长度（至少有这么多可写字节），
   * 返回时为输出长度；只能修改字节或缩短，不能增长，结果必须在
   * [start, limit)。start 不要求 NUL 结尾；*start_size == 0 时可为 NULL。 */
  void (*find_shortest_separator)(const cldb_comparator_t *comparator,
                                  char *start, size_t *start_size,
                                  cldb_slice_t limit);

  /* 尝试生成不更长的 key'；对应上游 FindShortSuccessor，要求 key' >= key，
   * 允许同长或原样返回。长度字段和缓冲约定同 find_shortest_separator，
   * 只能修改或缩短，不能增长；不要求 NUL 结尾，*key_size == 0 时可为 NULL。 */
  void (*find_short_successor)(const cldb_comparator_t *comparator, char *key,
                               size_t *key_size);
} cldb_comparator_vtbl_t;

struct cldb_comparator {
  const cldb_comparator_vtbl_t *vtbl; /* borrowed；通常指向 static const 表 */
};

/* 三路比较；对应上游 Comparator::Compare。comparator 必须非 NULL。 */
static inline int cldb_comparator_compare(const cldb_comparator_t *comparator,
                                          cldb_slice_t a, cldb_slice_t b) {
  return comparator->vtbl->compare(comparator, a, b);
}

/* 借用 comparator 名称；对应上游 Comparator::Name。返回指针不得释放。 */
static inline const char *cldb_comparator_name(
    const cldb_comparator_t *comparator) {
  return comparator->vtbl->name(comparator);
}

/* 原地尝试缩短 start；对应上游 Comparator::FindShortestSeparator。
 * 前置条件与缓冲约定见 vtable 字段注释。 */
static inline void cldb_comparator_find_shortest_separator(
    const cldb_comparator_t *comparator, char *start, size_t *start_size,
    cldb_slice_t limit) {
  comparator->vtbl->find_shortest_separator(comparator, start, start_size,
                                            limit);
}

/* 原地尝试缩短 key 且保持 key' >= key；对应上游
 * Comparator::FindShortSuccessor。缓冲约定见 vtable 字段注释。 */
static inline void cldb_comparator_find_short_successor(
    const cldb_comparator_t *comparator, char *key, size_t *key_size) {
  comparator->vtbl->find_short_successor(comparator, key, key_size);
}

/* 返回内置字节序比较器单例；对应上游 BytewiseComparator。按无符号字节
 * 字典序比较，空串小于任何非空串。返回对象归库所有，不得销毁。 */
const cldb_comparator_t *cldb_bytewise_comparator(void);

#endif /* CLDB_INCLUDE_CLEVELDB_COMPARATOR_H_ */
