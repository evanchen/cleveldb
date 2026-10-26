#ifndef CLDB_SRC_PORT_PORT_PTHREAD_H_
#define CLDB_SRC_PORT_PORT_PTHREAD_H_

#include "cleveldb/status.h"
#include "port/port.h"

/* 把 pthread 返回的错误码映射为状态：参数或生命周期错误归为
 * InvalidArgument，资源不足等运行期错误归为 IOError。operation 是
 * 状态消息中的操作上下文。多个 pthread 封装共用，避免重复映射规则。 */
cldb_status_t cldb_pthread_status(const char *operation, int rc);

#endif /* CLDB_SRC_PORT_PORT_PTHREAD_H_ */
