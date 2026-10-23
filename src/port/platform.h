#ifndef CLDB_SRC_PORT_PLATFORM_H_
#define CLDB_SRC_PORT_PLATFORM_H_

/*
 * 仅支持 Linux：CMake 配置期检查，这里再做一次编译期拦截。
 * T107/T114 在此基础上扩展 port 层。
 */
#if !defined(__linux__)
#error "cleveldb only supports Linux"
#endif

#if !defined(CLDB_PLATFORM_LINUX)
#define CLDB_PLATFORM_LINUX 1
#endif

/* 占位符号，T107/T114 替换或扩展。 */
const char *cldb_platform_name(void);

#endif /* CLDB_SRC_PORT_PLATFORM_H_ */
