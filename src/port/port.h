#ifndef CLDB_SRC_PORT_PORT_H_
#define CLDB_SRC_PORT_PORT_H_

/* 仅支持 Linux：上游 port/port.h 按平台选择 port_posix/port_win；
 * 本移植只保留 POSIX 分支，编译期直接拒绝其他平台。 */
#if !defined(__linux__)
#error "cleveldb only supports Linux"
#endif

/* 平台标识；对应上游 LEVELDB_PLATFORM_*，供必要时条件编译。 */
#if !defined(CLDB_PLATFORM_LINUX)
#define CLDB_PLATFORM_LINUX 1
#endif

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

/* 调试构建标识：release 定义 NDEBUG 时置 0。上游没有同名宏，
 * 对应各 .cc 中 #ifndef NDEBUG 与 assert 的用法。 */
#if defined(NDEBUG)
#undef CLDB_DEBUG
#define CLDB_DEBUG 0
#elif !defined(CLDB_DEBUG)
#define CLDB_DEBUG 1
#endif

/* 内部不变量断言：debug 生效，release 编译为无操作；对应上游 assert。
 * 外部输入错误必须返回 Status，不能用断言。 */
#if CLDB_DEBUG
#define CLDB_ASSERT(expr) assert(expr)
#else
#define CLDB_ASSERT(expr) ((void)0)
#endif

/* 显式忽略参数或变量；对应上游 (void)arg 写法，避免 -Wunused-* 告警。 */
#define CLDB_UNUSED(x) ((void)(x))

/* 缓存行提示值：常见 Linux 为 64 字节；仅用于避免伪共享，
 * 不保证等于当前 CPU 的实际缓存行大小。 */
#if !defined(CLDB_CACHELINE_SIZE)
#define CLDB_CACHELINE_SIZE 64
#endif

/* 按缓存行对齐的变量或结构体字段；对应上游 alignas 习惯用法。 */
#define CLDB_CACHELINE_ALIGNED _Alignas(CLDB_CACHELINE_SIZE)

/* 任意 2 的幂对齐；C11 _Alignas 的薄封装。 */
#define CLDB_ALIGNAS(n) _Alignas(n)

#endif /* CLDB_SRC_PORT_PORT_H_ */
