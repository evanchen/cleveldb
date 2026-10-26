#ifndef CLDB_SRC_UTIL_ARENA_H_
#define CLDB_SRC_UTIL_ARENA_H_

#include <stddef.h>

/* 只增不减的按块分配器；对应上游 util/arena.h 的 Arena，用于 MemTable
 * 等频繁小分配且统一释放的场景。不提供单次 free，destroy 统一回收。 */
typedef struct cldb_arena {
  char *alloc_ptr;              /* 借用视图：当前块内下一个可写位置 */
  size_t alloc_bytes_remaining; /* 当前块剩余字节；对应上游同名成员 */
  char **blocks;                /* owned 块指针数组；块本身由 arena 拥有 */
  size_t block_count;           /* 已记录块数 */
  size_t block_capacity;        /* blocks 数组容量；容量开销不计入统计 */
  size_t memory_usage;          /* 总量含每块指针开销；对应上游 memory_usage_ */
} cldb_arena_t;

/* 初始化为空；对应上游构造函数。接受 NULL。 */
void cldb_arena_init(cldb_arena_t *arena);

/* 释放所有块并重置；对应上游析构函数。接受 NULL；重置后可复用。 */
void cldb_arena_destroy(cldb_arena_t *arena);

/* 分配 bytes 字节；对应上游 Allocate。bytes 必须 > 0；失败返回 NULL，
 * 且不改变分配状态。返回值由 arena 拥有，直到 destroy；不保证对齐。 */
char *cldb_arena_allocate(cldb_arena_t *arena, size_t bytes);

/* 分配并保证按 max(sizeof(void*), 8) 对齐；对应上游 AllocateAligned。
 * 失败返回 NULL；返回值由 arena 拥有，直到 destroy。 */
char *cldb_arena_allocate_aligned(cldb_arena_t *arena, size_t bytes);

/* 已分配块的估计用量；对应上游 MemoryUsage，含每块一个指针的开销，
 * 不含数组容量与分配器自身开销。接受 NULL，返回 0。 */
size_t cldb_arena_memory_usage(const cldb_arena_t *arena);

#endif /* CLDB_SRC_UTIL_ARENA_H_ */
