#ifndef CLDB_SRC_DB_MEMTABLE_H_
#define CLDB_SRC_DB_MEMTABLE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cleveldb/iterator.h"
#include "cleveldb/slice.h"
#include "cleveldb/status.h"
#include "db/dbformat.h"

/* 内存表；对应上游 db/memtable.h。条目进入 Arena 后不再移动或删除，因此
 * 读取与迭代可在 Add 并发时进行；所有 Add 必须由调用者在外部串行化。 */
typedef struct cldb_memtable cldb_memtable_t;

/* 创建内存表并持有首个引用（计数为 1）。comparator 按值复制，其
 * user_comparator 只被引用且必须比内存表活得久；参数无效或分配失败
 * 返回 NULL。 */
cldb_memtable_t *cldb_memtable_create(
    const cldb_internal_key_comparator_t *comparator);

/* 原子增减引用计数；unref 归零时释放。接受 NULL。ref 后不得让计数溢出，
 * 最后一次 unref 后不得再访问对象。 */
void cldb_memtable_ref(cldb_memtable_t *memtable);
void cldb_memtable_unref(cldb_memtable_t *memtable);

/* Arena 的内存用量；对应上游 ApproximateMemoryUsage，可与 Add 并发读取。
 * 接受 NULL 并返回 0。 */
size_t cldb_memtable_approximate_memory_usage(
    const cldb_memtable_t *memtable);

/* 编码 varint32 内部键长度 | 内部键 | varint32 value 长度 | value，复制
 * key/value 后插入。需与其它 Add 外部串行化；成功返回 OK，参数或重复
 * 内部键返回 InvalidArgument，Arena 分配失败返回 IOError。 */
cldb_status_t cldb_memtable_add(cldb_memtable_t *memtable,
                                cldb_sequence_number_t sequence,
                                cldb_value_type_t type, cldb_slice_t key,
                                cldb_slice_t value);

/* 定位第一个 >= key 的条目；key 必须由 cldb_lookup_key_init 成功构造。
 * 命中同 user_key 时按 tag 返回对应版本：Value 写回借用 Slice 并返回 true，
 * Deletion 置 NotFound 并返回 true。未命中返回 false 且 status 为 OK；NULL
 * 参数返回 InvalidArgument。value 与 status 必须非 NULL；value 在内存表
 * 释放前有效。 */
bool cldb_memtable_get(const cldb_memtable_t *memtable,
                       const cldb_lookup_key_t *key, cldb_slice_t *value,
                       cldb_status_t *status);

/* 创建堆迭代器，调用者用 cldb_iter_destroy 释放；返回 key 为内部键、
 * value 为条目 value 的借用 Slice。迭代器不持有内存表引用，调用者必须
 * 保证内存表活得更久；memtable 为 NULL 或分配失败返回 NULL。 */
cldb_iterator_t *cldb_memtable_new_iterator(
    const cldb_memtable_t *memtable);

#endif /* CLDB_SRC_DB_MEMTABLE_H_ */
