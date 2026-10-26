#ifndef CLDB_SRC_PORT_PORT_ATOMIC_H_
#define CLDB_SRC_PORT_PORT_ATOMIC_H_

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "port/port.h"

/* C11 原子类型，对应上游 std::atomic<uint32_t/uint64_t/T*>。只封装
 * load/store/RMW/CAS；内存序由调用者用 memory_order_* 明确指定。
 * 原子对象必须先 init 再跨线程共享；atomic_init 本身不是线程安全的。 */

typedef _Atomic(uint32_t) cldb_atomic_u32_t;
typedef _Atomic(uint64_t) cldb_atomic_u64_t;
typedef _Atomic(void *) cldb_atomic_ptr_t;

/* 初始化原子量，必须在共享前调用；对应上游 std::atomic 构造。 */
static inline void cldb_atomic_u32_init(cldb_atomic_u32_t *object,
                                        uint32_t value) {
  atomic_init(object, value);
}

static inline void cldb_atomic_u64_init(cldb_atomic_u64_t *object,
                                        uint64_t value) {
  atomic_init(object, value);
}

static inline void cldb_atomic_ptr_init(cldb_atomic_ptr_t *object,
                                        void *value) {
  atomic_init(object, value);
}

/* 原子读取；对应上游 load(order)。 */
static inline uint32_t cldb_atomic_u32_load(const cldb_atomic_u32_t *object,
                                            memory_order order) {
  return atomic_load_explicit(object, order);
}

static inline uint64_t cldb_atomic_u64_load(const cldb_atomic_u64_t *object,
                                            memory_order order) {
  return atomic_load_explicit(object, order);
}

static inline void *cldb_atomic_ptr_load(const cldb_atomic_ptr_t *object,
                                         memory_order order) {
  return atomic_load_explicit(object, order);
}

/* 原子写入；对应上游 store(order)。 */
static inline void cldb_atomic_u32_store(cldb_atomic_u32_t *object,
                                         uint32_t value, memory_order order) {
  atomic_store_explicit(object, value, order);
}

static inline void cldb_atomic_u64_store(cldb_atomic_u64_t *object,
                                         uint64_t value, memory_order order) {
  atomic_store_explicit(object, value, order);
}

static inline void cldb_atomic_ptr_store(cldb_atomic_ptr_t *object,
                                         void *value, memory_order order) {
  atomic_store_explicit(object, value, order);
}

/* 原子加并返回旧值；对应上游 fetch_add，常用 relaxed 做统计计数。 */
static inline uint32_t cldb_atomic_u32_fetch_add(cldb_atomic_u32_t *object,
                                                 uint32_t delta,
                                                 memory_order order) {
  return atomic_fetch_add_explicit(object, delta, order);
}

static inline uint64_t cldb_atomic_u64_fetch_add(cldb_atomic_u64_t *object,
                                                 uint64_t delta,
                                                 memory_order order) {
  return atomic_fetch_add_explicit(object, delta, order);
}

/* 强比较交换：成功返回 true；失败把当前值写回 *expected 并返回 false。
 * 对应上游 compare_exchange_strong；failure 不能是 release/acq_rel，
 * 且不能强于 success。 */
static inline bool cldb_atomic_u32_compare_exchange(
    cldb_atomic_u32_t *object, uint32_t *expected, uint32_t desired,
    memory_order success, memory_order failure) {
  return atomic_compare_exchange_strong_explicit(object, expected, desired,
                                                 success, failure);
}

static inline bool cldb_atomic_u64_compare_exchange(
    cldb_atomic_u64_t *object, uint64_t *expected, uint64_t desired,
    memory_order success, memory_order failure) {
  return atomic_compare_exchange_strong_explicit(object, expected, desired,
                                                 success, failure);
}

/* 指针原子量只提供 load/store/CAS：上游 skiplist 链接不需要 void* 算术，
 * 因此不提供语义含糊的 fetch_add。 */
static inline bool cldb_atomic_ptr_compare_exchange(
    cldb_atomic_ptr_t *object, void **expected, void *desired,
    memory_order success, memory_order failure) {
  return atomic_compare_exchange_strong_explicit(object, expected, desired,
                                                 success, failure);
}

#endif /* CLDB_SRC_PORT_PORT_ATOMIC_H_ */
