#include "util/arena.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>

#include "port/port.h"

/* 对应上游 kBlockSize；小于 1/4 块的分配从当前块切分。 */
static const size_t kBlockSize = 4096;

/* 新建块并登记；对应上游 AllocateNewBlock。失败时 arena 状态不变。 */
static char *arena_new_block(cldb_arena_t *arena, size_t block_bytes) {
  char *block;
  char **blocks;
  size_t new_capacity;
  uint64_t increment;
  uint64_t usage;

  if (block_bytes == 0 || block_bytes > SIZE_MAX - sizeof(char *)) {
    return NULL;
  }
  block = malloc(block_bytes);
  if (block == NULL) {
    return NULL;
  }
  if (arena->block_count == arena->block_capacity) {
    if (arena->block_capacity == 0) {
      new_capacity = 8;
    } else {
      if (arena->block_capacity > SIZE_MAX / 2) {
        free(block);
        return NULL;
      }
      new_capacity = arena->block_capacity * 2;
    }
    if (new_capacity > SIZE_MAX / sizeof(*blocks)) {
      free(block);
      return NULL;
    }
    blocks = realloc(arena->blocks, new_capacity * sizeof(*blocks));
    if (blocks == NULL) {
      free(block);
      return NULL;
    }
    arena->blocks = blocks;
    arena->block_capacity = new_capacity;
  }
  increment = (uint64_t)block_bytes + (uint64_t)sizeof(char *);
  usage = cldb_atomic_u64_load(&arena->memory_usage, memory_order_relaxed);
  if (usage > UINT64_MAX - increment) {
    free(block);
    return NULL;
  }
  arena->blocks[arena->block_count] = block;
  ++arena->block_count;
  cldb_atomic_u64_fetch_add(&arena->memory_usage, increment,
                            memory_order_relaxed);
  return block;
}

/* 当前块不够时换新块；对应上游 AllocateFallback。 */
static char *arena_allocate_fallback(cldb_arena_t *arena, size_t bytes) {
  char *block;

  if (bytes > kBlockSize / 4) {
    /* 大分配单独成块，避免浪费当前块的剩余空间。 */
    return arena_new_block(arena, bytes);
  }
  block = arena_new_block(arena, kBlockSize);
  if (block == NULL) {
    return NULL;
  }
  arena->alloc_ptr = block;
  arena->alloc_bytes_remaining = kBlockSize;
  arena->alloc_ptr += bytes;
  arena->alloc_bytes_remaining -= bytes;
  return block;
}

void cldb_arena_init(cldb_arena_t *arena) {
  if (arena == NULL) {
    return;
  }
  arena->alloc_ptr = NULL;
  arena->alloc_bytes_remaining = 0;
  arena->blocks = NULL;
  arena->block_count = 0;
  arena->block_capacity = 0;
  cldb_atomic_u64_init(&arena->memory_usage, 0);
}

void cldb_arena_destroy(cldb_arena_t *arena) {
  size_t i;

  if (arena == NULL) {
    return;
  }
  for (i = 0; i < arena->block_count; ++i) {
    free(arena->blocks[i]);
  }
  free(arena->blocks);
  cldb_arena_init(arena);
}

char *cldb_arena_allocate(cldb_arena_t *arena, size_t bytes) {
  char *result;

  if (arena == NULL || bytes == 0) {
    return NULL;
  }
  if (bytes <= arena->alloc_bytes_remaining) {
    result = arena->alloc_ptr;
    arena->alloc_ptr += bytes;
    arena->alloc_bytes_remaining -= bytes;
    return result;
  }
  return arena_allocate_fallback(arena, bytes);
}

char *cldb_arena_allocate_aligned(cldb_arena_t *arena, size_t bytes) {
  const size_t align = sizeof(void *) > 8 ? sizeof(void *) : 8;
  size_t current_mod;
  size_t slop;
  size_t needed;
  char *result;

  if (arena == NULL || bytes == 0 || bytes > SIZE_MAX - (align - 1)) {
    return NULL;
  }
  current_mod = (uintptr_t)arena->alloc_ptr & (align - 1);
  slop = current_mod == 0 ? 0 : align - current_mod;
  needed = bytes + slop;
  if (needed <= arena->alloc_bytes_remaining) {
    result = arena->alloc_ptr + slop;
    arena->alloc_ptr += needed;
    arena->alloc_bytes_remaining -= needed;
  } else {
    /* fallback 总是返回新块首地址；malloc 存储满足 max_align_t 对齐。 */
    result = arena_allocate_fallback(arena, bytes);
  }
  /* 与上游一致，调试期校验最终结果；release 编译为空操作。 */
  CLDB_ASSERT(((uintptr_t)result & (align - 1)) == 0);
  return result;
}

size_t cldb_arena_memory_usage(const cldb_arena_t *arena) {
  if (arena == NULL) {
    return 0;
  }
  return (size_t)cldb_atomic_u64_load(&arena->memory_usage,
                                      memory_order_relaxed);
}
