#ifndef CLDB_INCLUDE_CLEVELDB_FILTER_POLICY_H_
#define CLDB_INCLUDE_CLEVELDB_FILTER_POLICY_H_

#include <stdbool.h>
#include <stddef.h>

#include "cleveldb/slice.h"
#include "cleveldb/status.h"

/* 过滤器策略接口；对应上游 leveldb::FilterPolicy。策略把一组 key 摘要成
 * 过滤器，读取时用于排除确定不包含目标 key 的数据块。除 destroy 外，策略
 * 方法必须线程安全（sink 的线程安全由调用者保证）。Slice 和 keys 数组均为
 * 借用。具体实现把 cldb_filter_policy_t 放在结构体首成员，并在回调中转回
 * 外层类型。 */
typedef struct cldb_filter_policy cldb_filter_policy_t;

/* 过滤器追加目标；对应上游 CreateFilter 的 std::string* dst。append 向
 * 目标追加 size 字节并返回可写起点：data 非 NULL 时复制，data 为 NULL 时
 * 填零；失败返回 NULL 且目标不变。size 必须大于 0；返回指针只在下一次
 * append 前有效。arg 为 borrowed。 */
typedef char *(*cldb_filter_sink_append_fn_t)(void *arg, const void *data,
                                              size_t size);

typedef struct cldb_filter_sink {
  void *arg; /* borrowed；回调上下文，由调用者保证本次调用期间有效 */
  cldb_filter_sink_append_fn_t append; /* 必须非 NULL */
} cldb_filter_sink_t;

/* 各函数指针必须非 NULL，覆盖上游 FilterPolicy 的纯虚方法和虚析构。 */
typedef struct cldb_filter_policy_vtbl {
  /* 策略名称；对应上游 Name。返回 borrowed 静态字符串，生命周期覆盖策略。
   * 名称会写入表元数据；过滤器编码不兼容变化时必须换名称。 */
  const char *(*name)(const cldb_filter_policy_t *policy);

  /* 把 keys[0,n-1] 的过滤器追加到 dst；对应上游 CreateFilter。keys 为
   * borrowed 数组，n == 0 时可为 NULL；不得修改 dst 中已有内容。成功返回
   * OK；参数错误返回 InvalidArgument，追加失败返回 IOError。失败时目标
   * 可能已被部分追加，调用者应丢弃整个目标。 */
  cldb_status_t (*create_filter)(const cldb_filter_policy_t *policy,
                                 const cldb_slice_t *keys, size_t n,
                                 cldb_filter_sink_t *dst);

  /* 判断 key 是否可能在 filter 中；对应上游 KeyMayMatch。若 key 确实由
   * 同一策略写入，必须返回 true；否则允许 false positive，但应尽量返回
   * false。filter 可能为空或损坏，实现不得越界或崩溃。 */
  bool (*key_may_match)(const cldb_filter_policy_t *policy, cldb_slice_t key,
                        cldb_slice_t filter);

  /* 销毁策略；对应上游虚析构。对象非 NULL；具体实现负责释放自身。
   * 自定义栈/静态对象可提供空实现。 */
  void (*destroy)(cldb_filter_policy_t *policy);
} cldb_filter_policy_vtbl_t;

struct cldb_filter_policy {
  const cldb_filter_policy_vtbl_t *vtbl; /* borrowed；非 NULL；通常 static const */
};

/* 借用策略名称；对应上游 FilterPolicy::Name。policy 必须非 NULL。 */
static inline const char *cldb_filter_policy_name(
    const cldb_filter_policy_t *policy) {
  return policy->vtbl->name(policy);
}

/* 生成并追加过滤器；对应上游 FilterPolicy::CreateFilter。dst 约定见
 * cldb_filter_sink_t 和 vtable 字段注释。 */
static inline cldb_status_t cldb_filter_policy_create_filter(
    const cldb_filter_policy_t *policy, const cldb_slice_t *keys, size_t n,
    cldb_filter_sink_t *dst) {
  return policy->vtbl->create_filter(policy, keys, n, dst);
}

/* 查询 key 是否可能匹配；对应上游 FilterPolicy::KeyMayMatch。
 * false 表示一定不存在，true 表示可能存在。 */
static inline bool cldb_filter_policy_key_may_match(
    const cldb_filter_policy_t *policy, cldb_slice_t key,
    cldb_slice_t filter) {
  return policy->vtbl->key_may_match(policy, key, filter);
}

/* 销毁策略；接受 NULL，每个对象只能调用一次。内置 Bloom 由库分配，必须
 * 用本函数释放；自定义策略由调用者按其 destroy 约定管理。DB 只借用策略，
 * 策略必须比 DB 活得久。 */
void cldb_filter_policy_destroy(cldb_filter_policy_t *policy);

/* 创建内置 Bloom 策略；对应上游 NewBloomFilterPolicy。bits_per_key 是每个
 * key 约分配的位数，推荐 10（约 1% 误判）；负数或分配失败返回 NULL。探测数
 * k 为 floor(bits_per_key * 0.69) 并 clamp 到 [1,30]，最少使用 64 位。成功
 * 对象由调用者拥有。KeyMayMatch 对长度小于 2 的过滤器返回 false，对 k > 30
 * 的保留编码返回 true。 */
cldb_filter_policy_t *cldb_new_bloom_filter_policy(int bits_per_key);

#endif /* CLDB_INCLUDE_CLEVELDB_FILTER_POLICY_H_ */
