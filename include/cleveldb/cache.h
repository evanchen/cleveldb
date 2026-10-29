#ifndef CLDB_INCLUDE_CLEVELDB_CACHE_H_
#define CLDB_INCLUDE_CLEVELDB_CACHE_H_

#include <stddef.h>
#include <stdint.h>

#include "cleveldb/slice.h"

/* 键值缓存接口；对应上游 leveldb::Cache。缓存自动驱逐条目，并把 charge
 * 计入容量。除 destroy 外，同一 cache 可被多个线程并发调用；handle 只能
 * 交给产生它的 cache 使用。具体实现把 cldb_cache_t 放在结构体首成员，并在
 * 回调中转回外层类型。 */
typedef struct cldb_cache cldb_cache_t;

/* 不透明缓存句柄；布局由具体实现定义，调用者只持有指针并通过 cache 方法
 * 访问。对应上游 Cache::Handle。 */
typedef struct cldb_cache_handle cldb_cache_handle_t;

/* 条目被移出缓存且最后一个客户端 handle 释放时调用；对应上游 Insert 的
 * deleter。arg 是 Insert 传入的上下文，cache 不负责释放，必须存活到 deleter 被
 * 调用（可能远晚于 Insert）；key 是缓存内部副本，仅本次调用有效；value 是
 * 交给缓存的指针。回调可能持有 shard 锁，禁止重入同一 cache 或等待使用该
 * cache 的线程。 */
typedef void (*cldb_cache_deleter_fn_t)(void *arg, cldb_slice_t key,
                                        void *value);

/* 各函数指针必须非 NULL，覆盖上游 Cache 的纯虚方法和虚析构。自定义实现可
 * 把 prune/set_capacity 做成空操作。 */
typedef struct cldb_cache_vtbl {
  /* 插入或替换 key；对应上游 Insert。成功返回调用者持有的 handle，必须
   * 恰好 Release 一次。key 会被复制，返回后原 key 可释放。成功时 value
   * 释放责任转交缓存，条目最终删除时把 deleter_arg 原样传给 deleter；
   * deleter 可为 NULL，表示无需清理 value。失败返回 NULL，value 仍归调用
   * 者，且不会调用 deleter。charge 可大于容量；该条目在有外部引用时不会
   * 被驱逐。 */
  cldb_cache_handle_t *(*insert)(cldb_cache_t *cache, cldb_slice_t key,
                                 void *value, size_t charge,
                                 cldb_cache_deleter_fn_t deleter,
                                 void *deleter_arg);

  /* 查找 key；对应上游 Lookup。命中返回调用者持有的 handle，必须 Release；
   * 未命中返回 NULL。 */
  cldb_cache_handle_t *(*lookup)(cldb_cache_t *cache, cldb_slice_t key);

  /* 释放一个由 insert/lookup 返回的 handle；对应上游 Release。每个 handle
   * 恰好调用一次；最后一个引用释放后可能立即触发 deleter。 */
  void (*release)(cldb_cache_t *cache, cldb_cache_handle_t *handle);

  /* 读取 handle 中的 value；对应上游 Value。返回内部指针，调用者不负责释放，至少在有
   * 效到该 handle 被 Release 前；handle 必须尚未释放。 */
  void *(*value)(cldb_cache_t *cache, cldb_cache_handle_t *handle);

  /* 移除 key 对应的缓存引用；对应上游 Erase。已有 handle 仍有效，条目要等
   * 最后一个 handle 释放后才删除。key 不存在时无操作。 */
  void (*erase)(cldb_cache_t *cache, cldb_slice_t key);

  /* 返回同一 cache 内唯一的数值 id；对应上游 NewId。客户端可把它拼进 key，
   * 共享同一个 cache 而互不冲突。 */
  uint64_t (*new_id)(cldb_cache_t *cache);

  /* 删除当前没有客户端引用的条目；对应上游 Prune。仍被引用的条目保留。 */
  void (*prune)(cldb_cache_t *cache);

  /* 调整容量并立即从 LRU 尾部驱逐；对应上游 SetCapacity。仍被引用的条目
   * 不会被驱逐，可能使实际 usage 暂时高于新容量。 */
  void (*set_capacity)(cldb_cache_t *cache, size_t capacity);

  /* 返回所有条目 charge 之和的估计值；对应上游 TotalCharge。并发访问时
   * 各分片不在同一时刻采样，只应作为估计。 */
  size_t (*total_charge)(cldb_cache_t *cache);

  /* 销毁实现并释放剩余条目；对应上游虚析构。调用前所有 handle 必须已
   * Release，之后不得再访问 cache。 */
  void (*destroy)(cldb_cache_t *cache);
} cldb_cache_vtbl_t;

struct cldb_cache {
  const cldb_cache_vtbl_t *vtbl; /* 不负责释放；非 NULL；通常 static const */
};

/* 插入或替换 key；语义见 cldb_cache_vtbl_t::insert。 */
static inline cldb_cache_handle_t *cldb_cache_insert(
    cldb_cache_t *cache, cldb_slice_t key, void *value, size_t charge,
    cldb_cache_deleter_fn_t deleter, void *deleter_arg) {
  return cache->vtbl->insert(cache, key, value, charge, deleter, deleter_arg);
}

/* 查找 key；对应上游 Cache::Lookup。 */
static inline cldb_cache_handle_t *cldb_cache_lookup(cldb_cache_t *cache,
                                                     cldb_slice_t key) {
  return cache->vtbl->lookup(cache, key);
}

/* 释放 handle；对应上游 Cache::Release。 */
static inline void cldb_cache_release(cldb_cache_t *cache,
                                      cldb_cache_handle_t *handle) {
  cache->vtbl->release(cache, handle);
}

/* 读取 handle 的 value；对应上游 Cache::Value。 */
static inline void *cldb_cache_value(cldb_cache_t *cache,
                                     cldb_cache_handle_t *handle) {
  return cache->vtbl->value(cache, handle);
}

/* 移除 key；对应上游 Cache::Erase。 */
static inline void cldb_cache_erase(cldb_cache_t *cache, cldb_slice_t key) {
  cache->vtbl->erase(cache, key);
}

/* 返回新的唯一 id；对应上游 Cache::NewId。 */
static inline uint64_t cldb_cache_new_id(cldb_cache_t *cache) {
  return cache->vtbl->new_id(cache);
}

/* 驱逐未被引用的条目；对应上游 Cache::Prune。 */
static inline void cldb_cache_prune(cldb_cache_t *cache) {
  cache->vtbl->prune(cache);
}

/* 调整容量；对应上游 Cache::SetCapacity。 */
static inline void cldb_cache_set_capacity(cldb_cache_t *cache,
                                           size_t capacity) {
  cache->vtbl->set_capacity(cache, capacity);
}

/* 返回 charge 估计值；对应上游 Cache::TotalCharge。 */
static inline size_t cldb_cache_total_charge(cldb_cache_t *cache) {
  return cache->vtbl->total_charge(cache);
}

/* 销毁 cache；接受 NULL。每个对象只能调用一次，调用前必须 Release 所有
 * handle。内置 LRU 会断言缓存内仍有未释放引用；capacity 为 0 产生的 handle
 * 不进入缓存，只能由调用者保证释放。自定义 cache 的 destroy 可只释放内部
 * 资源而不释放栈/静态对象本身。 */
void cldb_cache_destroy(cldb_cache_t *cache);

/* 创建内置分片 LRU cache；对应上游 NewLRUCache。容量按 16 个 shard 均分并
 * 向上取整，实际总容量可能略大于 capacity。capacity 为 0 时仍可 Insert 和
 * 使用返回的 handle，但不会缓存，最后一个 Release 立即删除。成功返回由调用者
 * destroy 的对象，分配失败返回 NULL。 */
cldb_cache_t *cldb_new_lru_cache(size_t capacity);

#endif /* CLDB_INCLUDE_CLEVELDB_CACHE_H_ */
