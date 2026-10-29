#ifndef CLDB_SRC_UTIL_ENV_INTERNAL_H_
#define CLDB_SRC_UTIL_ENV_INTERNAL_H_

#include "cleveldb/status.h"

/* Env 实现共用的内部辅助；不对外安装。 */

/* 构造分配失败的 IOError；context 为调用方操作名。 */
cldb_status_t cldb_env_out_of_memory(const char *context);

/* 复制 NUL 结尾字符串；value 必须非 NULL。返回值由调用者 free，失败返回 NULL。 */
char *cldb_env_strdup(const char *value);

#endif /* CLDB_SRC_UTIL_ENV_INTERNAL_H_ */
