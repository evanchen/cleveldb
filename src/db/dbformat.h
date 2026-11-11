#ifndef CLDB_SRC_DB_DBFORMAT_H_
#define CLDB_SRC_DB_DBFORMAT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cleveldb/buf.h"
#include "cleveldb/comparator.h"
#include "cleveldb/filter_policy.h"
#include "cleveldb/slice.h"
#include "cleveldb/status.h"
#include "port/port.h"

/* 内部键、InternalKeyComparator 与 LookupKey；对应上游 db/dbformat.h。
 * 内部键为 user_key + fixed64((sequence << 8) | type)：先按用户比较器升序，
 * 同一 user_key 再按 tag 降序。 */

enum { CLDB_NUM_LEVELS = 7 };

/* tag 固定 8 字节；低 8 位存 ValueType，其余存 SequenceNumber。 */
#define CLDB_INTERNAL_KEY_TAG_SIZE 8

/* sequence 占 56 位，类型占低 8 位；对应上游 kMaxSequenceNumber。 */
#define CLDB_MAX_SEQUENCE_NUMBER ((UINT64_C(1) << 56) - 1)

typedef uint64_t cldb_sequence_number_t;

/* ValueType 数值写入内部键，不能改变；对应上游 ValueType。
 * CLDB_VALUE_TYPE_FOR_SEEK 是查找时使用的最高类型，保证定位到
 * 所有 sequence <= 快照的版本之前。 */
typedef enum cldb_value_type {
  CLDB_VALUE_TYPE_DELETION = 0x0,
  CLDB_VALUE_TYPE_VALUE = 0x1,
  CLDB_VALUE_TYPE_FOR_SEEK = CLDB_VALUE_TYPE_VALUE
} cldb_value_type_t;

typedef struct cldb_parsed_internal_key {
  cldb_slice_t user_key;        /* 仅引用 internal_key 中的前缀 */
  cldb_sequence_number_t sequence;
  cldb_value_type_t type;
} cldb_parsed_internal_key_t;

/* 按值构造解析结果；对应上游 ParsedInternalKey(user, seq, type)。 */
static inline cldb_parsed_internal_key_t cldb_parsed_internal_key_make(
    cldb_slice_t user_key, cldb_sequence_number_t sequence,
    cldb_value_type_t type) {
  cldb_parsed_internal_key_t parsed;

  parsed.user_key = user_key;
  parsed.sequence = sequence;
  parsed.type = type;
  return parsed;
}

/* 编码长度 user_key.size + 8；对应上游 InternalKeyEncodingLength。 */
static inline size_t cldb_internal_key_encoding_length(
    cldb_slice_t user_key) {
  return user_key.size + CLDB_INTERNAL_KEY_TAG_SIZE;
}

/* 追加内部键；对应上游 AppendInternalKey。失败返回 false 且 result 不变：
 * 参数无效、sequence > 最大值、type 非法、user_key 长度溢出或分配失败。 */
bool cldb_append_internal_key(cldb_buf_t *result, cldb_slice_t user_key,
                              cldb_sequence_number_t sequence,
                              cldb_value_type_t type);

/* 解析内部键；对应上游 ParseInternalKey。长度不足 8 或 type > 1 返回
 * false，此时 result 不变；成功时 user_key 引用 internal_key。 */
bool cldb_parse_internal_key(cldb_slice_t internal_key,
                             cldb_parsed_internal_key_t *result);

/* 取 user_key 前缀；对应上游 ExtractUserKey。长度不足 tag 是编程错误：
 * debug 断言，release 返回空切片。 */
static inline cldb_slice_t cldb_extract_user_key(cldb_slice_t internal_key) {
  CLDB_ASSERT(internal_key.size >= CLDB_INTERNAL_KEY_TAG_SIZE);
  if (internal_key.size < CLDB_INTERNAL_KEY_TAG_SIZE) {
    return cldb_slice_empty();
  }
  return cldb_slice_make(internal_key.data,
                         internal_key.size - CLDB_INTERNAL_KEY_TAG_SIZE);
}

/* 内部键比较器；对应上游 InternalKeyComparator。base 必须为首成员，外层
 * 对象通过 init 配置，并把 &base 交给通用 Comparator 接口。 */
typedef struct cldb_internal_key_comparator {
  cldb_comparator_t base; /* 必须为首成员 */
  const cldb_comparator_t *user_comparator; /* 仅引用，必须非 NULL */
} cldb_internal_key_comparator_t;

/* 初始化栈/静态对象；user_comparator 必须非 NULL，且比本对象活得久。
 * 对应上游 InternalKeyComparator(c) 构造函数。 */
void cldb_internal_key_comparator_init(
    cldb_internal_key_comparator_t *comparator,
    const cldb_comparator_t *user_comparator);

/* 返回包装的用户比较器；对应上游 user_comparator()。仅引用。 */
static inline const cldb_comparator_t *
cldb_internal_key_comparator_user_comparator(
    const cldb_internal_key_comparator_t *comparator) {
  return comparator->user_comparator;
}

/* 内部键过滤器包装；对应上游 InternalFilterPolicy。Name 转发用户策略，
 * CreateFilter/KeyMayMatch 先剥掉 8 字节 tag。不拥有 user_policy；destroy
 * 不释放它，调用者仍需按用户策略的约定管理其生命周期。 */
typedef struct cldb_internal_filter_policy {
  cldb_filter_policy_t base; /* 必须为首成员 */
  const cldb_filter_policy_t *user_policy; /* 仅引用，必须非 NULL */
} cldb_internal_filter_policy_t;

/* 初始化栈/静态对象；user_policy 必须非 NULL，且比本对象活得久。 */
void cldb_internal_filter_policy_init(
    cldb_internal_filter_policy_t *policy,
    const cldb_filter_policy_t *user_policy);

/* 查找键；对应上游 LookupKey。内部依次保存
 * varint32(内部键长度) + user_key + fixed64(tag)。短键使用 space，
 * 长键分配堆内存；初始化后不可按值复制，必须用 destroy 释放。 */
typedef struct cldb_lookup_key {
  char *start; /* memtable key 起点；指向 space 或堆分配 */
  char *kstart; /* internal key 起点，位于 varint 之后 */
  char *end;    /* tag 之后；memtable key 终点 */
  char space[200]; /* 小 key 内联缓冲 */
  bool heap_allocated; /* start 是否由 malloc 分配 */
} cldb_lookup_key_t;

/* 为 user_key 构造以 sequence 快照查找的键；对应上游 LookupKey(user, seq)。
 * 成功返回 true；失败返回 false 且对象处于可安全 destroy 的空状态。失败
 * 原因：参数无效、sequence 越界、长度溢出或分配失败。 */
bool cldb_lookup_key_init(cldb_lookup_key_t *key, cldb_slice_t user_key,
                          cldb_sequence_number_t sequence);

/* 释放堆内存并重置；接受 NULL。 */
void cldb_lookup_key_destroy(cldb_lookup_key_t *key);

/* 三个视图；对应上游 memtable_key/internal_key/user_key。返回切片仅在
 * destroy 前有效；未初始化或失败时返回空切片。 */
cldb_slice_t cldb_lookup_key_memtable_key(const cldb_lookup_key_t *key);
cldb_slice_t cldb_lookup_key_internal_key(const cldb_lookup_key_t *key);
cldb_slice_t cldb_lookup_key_user_key(const cldb_lookup_key_t *key);

#endif /* CLDB_SRC_DB_DBFORMAT_H_ */
