#include <stddef.h>
#include <stdint.h>

#include "harness.h"
#include "port/port.h"

/* 平台标识、CLDB_DEBUG 与 NDEBUG 一致，以及 debug 下断言可执行。 */
CLDB_TEST(port_platform_and_debug) {
  CLDB_ASSERT_EQ_INT(1, CLDB_PLATFORM_LINUX);
#if defined(NDEBUG)
  CLDB_ASSERT_EQ_INT(0, CLDB_DEBUG);
#else
  CLDB_ASSERT_EQ_INT(1, CLDB_DEBUG);
#endif
  CLDB_ASSERT(2 + 2 == 4);
}

/* 用 CLDB_UNUSED 显式忽略参数，避免 -Wunused-parameter。 */
static int port_ignored_parameter(int value) {
  CLDB_UNUSED(value);
  return 7;
}

CLDB_TEST(port_unused_macro) {
  CLDB_ASSERT_EQ_INT(7, port_ignored_parameter(42));
}

/* debug 下断言会求值，release 下整段表达式被丢弃。 */
CLDB_TEST(port_assert_evaluation) {
  volatile int evaluations = 0;

  CLDB_ASSERT(evaluations += 1);
  CLDB_ASSERT_EQ_INT(CLDB_DEBUG, evaluations);
}

/* 缓存行对齐字段会抬高结构体对齐；局部变量也可按缓存行/任意 2 的幂对齐。 */
struct port_aligned_fields {
  char lead;
  CLDB_CACHELINE_ALIGNED uint64_t value;
};

CLDB_TEST(port_alignment) {
  struct port_aligned_fields fields;
  CLDB_CACHELINE_ALIGNED uint64_t local = 0;
  CLDB_ALIGNAS(32) uint64_t narrow = 0;

  CLDB_ASSERT_EQ_UINT(0, (uintptr_t)&fields & (CLDB_CACHELINE_SIZE - 1));
  CLDB_ASSERT_EQ_UINT(0, offsetof(struct port_aligned_fields, value) &
                             (CLDB_CACHELINE_SIZE - 1));
  CLDB_ASSERT_EQ_UINT(0, (uintptr_t)&local & (CLDB_CACHELINE_SIZE - 1));
  CLDB_ASSERT_EQ_UINT(0, (uintptr_t)&narrow & 31);

  fields.value = 1;
  local = 2;
  narrow = 3;
  CLDB_EXPECT_EQ_UINT(1, fields.value);
  CLDB_EXPECT_EQ_UINT(2, local);
  CLDB_EXPECT_EQ_UINT(3, narrow);
}

void cldb_register_port_tests(void) {
  CLDB_RUN_TEST(port_platform_and_debug);
  CLDB_RUN_TEST(port_unused_macro);
  CLDB_RUN_TEST(port_assert_evaluation);
  CLDB_RUN_TEST(port_alignment);
}
