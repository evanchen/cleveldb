#ifndef CLDB_INCLUDE_CLEVELDB_ITERATOR_H_
#define CLDB_INCLUDE_CLEVELDB_ITERATOR_H_

#include <stdbool.h>
#include <stddef.h>

#include "cleveldb/slice.h"
#include "cleveldb/status.h"

/* 迭代器接口；对应上游 leveldb::Iterator。具体迭代器把 cldb_iterator_t
 * 放在结构体首成员，并在回调中转回外层类型。const 方法（valid/key/
 * value/status）可并发调用；只要有线程调用非 const 方法，所有访问都
 * 必须由调用者外部同步。 */
typedef struct cldb_iterator cldb_iterator_t;
typedef struct cldb_iter_cleanup cldb_iter_cleanup_t;

/* cleanup 回调；对应上游 Iterator::CleanupFunction。arg1/arg2 为 borrowed，
 * 注册者必须保证它们存活到迭代器销毁；库不释放。 */
typedef void (*cldb_iter_cleanup_fn_t)(void *arg1, void *arg2);

typedef struct cldb_iterator_vtbl {
  /* 当前是否位于某个 key/value 上；对应上游 Valid。 */
  bool (*valid)(const cldb_iterator_t *iterator);

  /* 定位到第一个/最后一个条目；对应上游 SeekToFirst/SeekToLast。
   * 源非空时调用后 valid 为真。 */
  void (*seek_to_first)(cldb_iterator_t *iterator);
  void (*seek_to_last)(cldb_iterator_t *iterator);

  /* 定位到第一个 >= target 的条目；对应上游 Seek。找到时 valid 为真。
   * target 为 borrowed，只在调用期间有效。 */
  void (*seek)(cldb_iterator_t *iterator, cldb_slice_t target);

  /* 移动到下一个/上一个条目；对应上游 Next/Prev。REQUIRES: 当前 valid。 */
  void (*next)(cldb_iterator_t *iterator);
  void (*prev)(cldb_iterator_t *iterator);

  /* 返回当前 key/value；对应上游 key()/value()。REQUIRES: 当前 valid。
   * 返回 Slice 为 borrowed，仅在下一次移动或销毁前有效。 */
  cldb_slice_t (*key)(const cldb_iterator_t *iterator);
  cldb_slice_t (*value)(const cldb_iterator_t *iterator);

  /* 返回当前错误状态；对应上游 status。valid 为假既可能是正常耗尽，
   * 也可能是错误，调用者需检查 status 区分。 */
  cldb_status_t (*status)(const cldb_iterator_t *iterator);

  /* 释放具体迭代器资源、调用 cldb_iter_run_cleanup，再释放对象本身。
   * 对应上游派生类析构后执行 Iterator 基类析构的顺序。具体实现必须
   * 遵守该顺序；调用者应使用 cldb_iter_destroy，不得直接调用本函数。 */
  void (*destroy)(cldb_iterator_t *iterator);
} cldb_iterator_vtbl_t;

struct cldb_iterator {
  const cldb_iterator_vtbl_t *vtbl; /* borrowed；通常指向 static const 表 */
  cldb_iter_cleanup_t *cleanup_head; /* internal；owned；由基类管理 */
  bool destroyed; /* internal；debug 重复销毁检查，调用者不得修改 */
};

/* 初始化基类；具体迭代器首次使用前必须调用。iterator 为 NULL 时无操作；
 * 否则 vtbl 及其各方法必须非 NULL。只能用于尚未使用的新对象。 */
void cldb_iter_init(cldb_iterator_t *iterator,
                    const cldb_iterator_vtbl_t *vtbl);

/* 登记销毁回调；对应上游 Iterator::RegisterCleanup。成功返回 OK，失败时
 * 不登记任何内容。执行顺序复刻上游链表：首个回调最先执行，其余按注册
 * 逆序执行（A、B、C 登记 => A、C、B）；同一迭代器不得在回调中重入。 */
cldb_status_t cldb_iter_register_cleanup(cldb_iterator_t *iterator,
                                         cldb_iter_cleanup_fn_t function,
                                         void *arg1, void *arg2);

/* 执行并释放全部 cleanup 节点；由具体 destroy 在释放自身资源之后、释放
 * 对象内存之前调用。接受 NULL；回调执行期间迭代器处于半销毁状态。 */
void cldb_iter_run_cleanup(cldb_iterator_t *iterator);

/* 销毁迭代器；接受 NULL。每个对象只能调用一次，重复调用是编程错误，
 * debug 构建会断言。内部设置 destroyed 标记后调用 vtbl->destroy。 */
void cldb_iter_destroy(cldb_iterator_t *iterator);

/* 判断当前是否有效；对应上游 Iterator::Valid。iterator 必须非 NULL。 */
static inline bool cldb_iter_valid(const cldb_iterator_t *iterator) {
  return iterator->vtbl->valid(iterator);
}

/* 定位到首/尾；对应上游 SeekToFirst/SeekToLast。 */
static inline void cldb_iter_seek_to_first(cldb_iterator_t *iterator) {
  iterator->vtbl->seek_to_first(iterator);
}

static inline void cldb_iter_seek_to_last(cldb_iterator_t *iterator) {
  iterator->vtbl->seek_to_last(iterator);
}

/* 定位到第一个 >= target 的条目；target 为借用。 */
static inline void cldb_iter_seek(cldb_iterator_t *iterator,
                                  cldb_slice_t target) {
  iterator->vtbl->seek(iterator, target);
}

/* 前后移动；调用者必须保证当前 valid。 */
static inline void cldb_iter_next(cldb_iterator_t *iterator) {
  iterator->vtbl->next(iterator);
}

static inline void cldb_iter_prev(cldb_iterator_t *iterator) {
  iterator->vtbl->prev(iterator);
}

/* 借用当前 key/value；调用者必须保证当前 valid，且在下一次移动/销毁前
 * 使用返回值。 */
static inline cldb_slice_t cldb_iter_key(const cldb_iterator_t *iterator) {
  return iterator->vtbl->key(iterator);
}

static inline cldb_slice_t cldb_iter_value(const cldb_iterator_t *iterator) {
  return iterator->vtbl->value(iterator);
}

/* 返回错误状态副本；对应上游 status。 */
static inline cldb_status_t cldb_iter_status(const cldb_iterator_t *iterator) {
  return iterator->vtbl->status(iterator);
}

/* 创建空迭代器（status 为 OK）或错误迭代器（保留 status 副本）；对应上游
 * NewEmptyIterator/NewErrorIterator。成功返回调用者拥有的堆对象，失败返回
 * NULL；两者都始终 valid 为假，seek 类方法为空操作。 */
cldb_iterator_t *cldb_new_empty_iterator(void);
cldb_iterator_t *cldb_new_error_iterator(cldb_status_t status);

#endif /* CLDB_INCLUDE_CLEVELDB_ITERATOR_H_ */
