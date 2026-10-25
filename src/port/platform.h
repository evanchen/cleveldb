#ifndef CLDB_SRC_PORT_PLATFORM_H_
#define CLDB_SRC_PORT_PLATFORM_H_

/* 仅支持 Linux：编译期拦截；Makefile 入口也会检查。 */
#if !defined(__linux__)
#error "cleveldb only supports Linux"
#endif

#if !defined(CLDB_PLATFORM_LINUX)
#define CLDB_PLATFORM_LINUX 1
#endif

/* 平台名；占位实现。 */
const char *cldb_platform_name(void);

#endif /* CLDB_SRC_PORT_PLATFORM_H_ */
