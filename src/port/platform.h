#ifndef CLDB_SRC_PORT_PLATFORM_H_
#define CLDB_SRC_PORT_PLATFORM_H_

/* 仅支持 Linux：业务代码无需平台分支，编译期直接拦截；
 * Makefile 入口也会检查，避免产生误导性的构建产物。 */
#if !defined(__linux__)
#error "cleveldb only supports Linux"
#endif

/* 平台标识；供条件编译，与 Makefile 传入的同名宏保持一致。 */
#if !defined(CLDB_PLATFORM_LINUX)
#define CLDB_PLATFORM_LINUX 1
#endif

/* 平台名；供测试和诊断打印。上游没有同名接口，这里对应
 * port/port_posix.h 在编译期选定平台实现、运行期可识别的角色。 */
const char *cldb_platform_name(void);

#endif /* CLDB_SRC_PORT_PLATFORM_H_ */
