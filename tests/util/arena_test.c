#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "util/arena.h"
#include "util/random.h"

/* 上游 ArenaTest.Empty 的 C 版，补充 NULL 与 0 字节边界。 */
CLDB_TEST(arena_empty) {
  cldb_arena_t arena;

  cldb_arena_init(&arena);
  CLDB_EXPECT_EQ_UINT(0, cldb_arena_memory_usage(&arena));
  CLDB_EXPECT_EQ_PTR(NULL, cldb_arena_allocate(&arena, 0));
  CLDB_EXPECT_EQ_PTR(NULL, cldb_arena_allocate_aligned(&arena, 0));
  CLDB_EXPECT_EQ_UINT(0, cldb_arena_memory_usage(&arena));

  cldb_arena_destroy(&arena);
  CLDB_EXPECT_EQ_UINT(0, cldb_arena_memory_usage(&arena));

  cldb_arena_init(NULL);
  cldb_arena_destroy(NULL);
  CLDB_EXPECT_EQ_UINT(0, cldb_arena_memory_usage(NULL));
  CLDB_EXPECT_EQ_PTR(NULL, cldb_arena_allocate(NULL, 1));
  CLDB_EXPECT_EQ_PTR(NULL, cldb_arena_allocate_aligned(NULL, 1));
}

/* 首个块按 4096 计，统计额外包含每块一个指针（对应上游 vector 槽位）。 */
CLDB_TEST(arena_first_block_accounting) {
  cldb_arena_t arena;
  char *first;
  char *big;
  char *next;
  size_t one_block = 4096 + sizeof(char *);

  cldb_arena_init(&arena);
  first = cldb_arena_allocate(&arena, 1);
  CLDB_ASSERT_TRUE(first != NULL);
  CLDB_ASSERT_EQ_UINT(one_block, cldb_arena_memory_usage(&arena));

  /* 先占满当前块，使 2000 触发 fallback 并单独成块。 */
  CLDB_ASSERT_TRUE(cldb_arena_allocate(&arena, 4000) != NULL);
  big = cldb_arena_allocate(&arena, 2000);
  CLDB_ASSERT_TRUE(big != NULL);
  CLDB_ASSERT_EQ_UINT(one_block + 2000 + sizeof(char *),
                      cldb_arena_memory_usage(&arena));

  next = cldb_arena_allocate(&arena, 7);
  CLDB_ASSERT_TRUE(next != NULL);
  CLDB_EXPECT_EQ_UINT((uintptr_t)first + 1 + 4000, (uintptr_t)next);
  CLDB_EXPECT_EQ_UINT(one_block + 2000 + sizeof(char *),
                      cldb_arena_memory_usage(&arena));

  cldb_arena_destroy(&arena);
}

/* 当前块放不下时换新块，旧块剩余空间被浪费（对应上游 fallback）。 */
CLDB_TEST(arena_block_rollover) {
  cldb_arena_t arena;
  char *ptrs[6];
  size_t i;

  cldb_arena_init(&arena);
  for (i = 0; i < 6; ++i) {
    ptrs[i] = cldb_arena_allocate(&arena, 1000);
    CLDB_ASSERT_TRUE(ptrs[i] != NULL);
    memset(ptrs[i], (int)(i + 1), 1000);
  }

  for (i = 1; i < 4; ++i) {
    CLDB_EXPECT_EQ_UINT((uintptr_t)ptrs[i - 1] + 1000,
                        (uintptr_t)ptrs[i]);
  }
  /* 第 5 次分配换新块，第 6 次紧接其后。 */
  CLDB_EXPECT_EQ_UINT((uintptr_t)ptrs[4] + 1000, (uintptr_t)ptrs[5]);
  CLDB_EXPECT_EQ_UINT(2 * (4096 + sizeof(char *)),
                      cldb_arena_memory_usage(&arena));

  for (i = 0; i < 6; ++i) {
    size_t b;

    for (b = 0; b < 1000; ++b) {
      CLDB_ASSERT_EQ_INT((int)(i + 1), (int)(uint8_t)ptrs[i][b]);
    }
  }
  cldb_arena_destroy(&arena);
}

/* 阈值是严格的 bytes > 1024；等于 1024 仍从新块中切分。 */
CLDB_TEST(arena_threshold) {
  cldb_arena_t arena;
  char *ptr;
  char *equal_threshold;
  char *above_threshold;
  char *next;
  size_t usage;
  size_t i;

  cldb_arena_init(&arena);
  for (i = 0; i < 4; ++i) {
    ptr = cldb_arena_allocate(&arena, 1000);
    CLDB_ASSERT_TRUE(ptr != NULL);
  }
  usage = cldb_arena_memory_usage(&arena);
  CLDB_ASSERT_EQ_UINT(4096 + sizeof(char *), usage);

  equal_threshold = cldb_arena_allocate(&arena, 1024);
  CLDB_ASSERT_TRUE(equal_threshold != NULL);
  CLDB_ASSERT_EQ_UINT(usage + 4096 + sizeof(char *),
                      cldb_arena_memory_usage(&arena));

  /* 新块剩余 1024；1025 放不下，触发 fallback 并按精确大小单独成块。 */
  CLDB_ASSERT_TRUE(cldb_arena_allocate(&arena, 2048) != NULL);
  above_threshold = cldb_arena_allocate(&arena, 1025);
  CLDB_ASSERT_TRUE(above_threshold != NULL);
  CLDB_ASSERT_EQ_UINT(usage + 4096 + sizeof(char *) + 1025 + sizeof(char *),
                      cldb_arena_memory_usage(&arena));

  next = cldb_arena_allocate(&arena, 10);
  CLDB_ASSERT_TRUE(next != NULL);
  CLDB_EXPECT_EQ_UINT((uintptr_t)equal_threshold + 1024 + 2048,
                      (uintptr_t)next);

  cldb_arena_destroy(&arena);
}

/* AllocateAligned 在普通分配造成错位后仍返回对齐指针。 */
CLDB_TEST(arena_aligned) {
  const size_t align = sizeof(void *) > 8 ? sizeof(void *) : 8;
  cldb_arena_t arena;
  cldb_random_t rnd;
  uint32_t i;

  cldb_arena_init(&arena);
  cldb_random_init(&rnd, 301);
  CLDB_ASSERT_TRUE(cldb_arena_allocate(&arena, 1) != NULL);
  for (i = 0; i < 2000; ++i) {
    size_t size = cldb_random_uniform(&rnd, 64) + 1;
    char *ptr;

    if (i % 3 == 0) {
      ptr = cldb_arena_allocate_aligned(&arena, size);
      CLDB_ASSERT_TRUE(ptr != NULL);
      CLDB_ASSERT_EQ_UINT(0, (uintptr_t)ptr & (align - 1));
    } else {
      ptr = cldb_arena_allocate(&arena, size);
      CLDB_ASSERT_TRUE(ptr != NULL);
    }
    memset(ptr, (int)(i % 251), size);
  }
  cldb_arena_destroy(&arena);
}

/* 移植上游 ArenaTest.Simple：随机大小、内容校验和内存统计上界。 */
CLDB_TEST(arena_simple) {
  const size_t kN = 100000;
  cldb_arena_t arena;
  cldb_random_t rnd;
  struct allocation {
    size_t size;
    char *ptr;
  } *allocated;
  size_t bytes = 0;
  size_t i;

  allocated = malloc(kN * sizeof(*allocated));
  CLDB_ASSERT_TRUE(allocated != NULL);
  cldb_arena_init(&arena);
  cldb_random_init(&rnd, 301);

  for (i = 0; i < kN; ++i) {
    size_t size;
    size_t b;
    char *ptr;

    if (i % (kN / 10) == 0) {
      size = i;
    } else {
      size = cldb_random_one_in(&rnd, 4000)
                 ? cldb_random_uniform(&rnd, 6000)
                 : (cldb_random_one_in(&rnd, 10)
                        ? cldb_random_uniform(&rnd, 100)
                        : cldb_random_uniform(&rnd, 20));
    }
    if (size == 0) {
      size = 1;
    }
    if (cldb_random_one_in(&rnd, 10)) {
      ptr = cldb_arena_allocate_aligned(&arena, size);
    } else {
      ptr = cldb_arena_allocate(&arena, size);
    }
    CLDB_ASSERT_TRUE(ptr != NULL);
    for (b = 0; b < size; ++b) {
      ptr[b] = (char)(i % 256);
    }
    bytes += size;
    allocated[i].size = size;
    allocated[i].ptr = ptr;

    CLDB_ASSERT_TRUE(cldb_arena_memory_usage(&arena) >= bytes);
    if (i > kN / 10) {
      CLDB_ASSERT_TRUE((double)cldb_arena_memory_usage(&arena) <=
                       (double)bytes * 1.10);
    }
  }

  for (i = 0; i < kN; ++i) {
    size_t b;

    for (b = 0; b < allocated[i].size; ++b) {
      CLDB_ASSERT_EQ_INT((int)(i % 256),
                         (int)(uint8_t)allocated[i].ptr[b]);
    }
  }
  cldb_arena_destroy(&arena);
  free(allocated);
}

void cldb_register_arena_tests(void) {
  CLDB_RUN_TEST(arena_empty);
  CLDB_RUN_TEST(arena_first_block_accounting);
  CLDB_RUN_TEST(arena_block_rollover);
  CLDB_RUN_TEST(arena_threshold);
  CLDB_RUN_TEST(arena_aligned);
  CLDB_RUN_TEST(arena_simple);
}
