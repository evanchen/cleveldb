#ifndef CLDB_SRC_PORT_PLATFORM_H_
#define CLDB_SRC_PORT_PLATFORM_H_

#include "port/port.h"

/* 平台名；供测试和诊断打印。上游没有同名接口，这里对应
 * port/port_posix.h 在编译期选定平台实现、运行期可识别的角色。 */
const char *cldb_platform_name(void);

#endif /* CLDB_SRC_PORT_PLATFORM_H_ */
