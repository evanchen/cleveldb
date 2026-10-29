#ifndef CLDB_SRC_HELPERS_MEMENV_MEMENV_H_
#define CLDB_SRC_HELPERS_MEMENV_MEMENV_H_

#include "cleveldb/env.h"

/* 全内存 Env；对应上游 helpers/memenv/memenv.cc。文件内容只存在于本对象
 * 内，destroy 后全部丢失，不跨实例持久化。目录由文件路径前缀隐式表示，
 * GetChildren 返回所有以 dir/ 开头的文件表条目（后缀可含 '/'），不返回
 * "."/".."（与 stock Env 不同）。
 * 普通方法可多线程并发调用；destroy 是终止操作，不得与其它方法以及
 * 尚未销毁的文件/锁/日志对象并发。 */
typedef enum cldb_memenv_log_target {
  CLDB_MEMENV_LOG_MEMORY = 0, /* 追加到同名内存文件，便于测试读取 */
  CLDB_MEMENV_LOG_STDERR = 1  /* 直接写 stderr，不创建文件 */
} cldb_memenv_log_target_t;

typedef struct cldb_memenv_options {
  cldb_memenv_log_target_t log_target; /* 默认 CLDB_MEMENV_LOG_MEMORY */
} cldb_memenv_options_t;

/* 初始化默认选项；接受 NULL。 */
void cldb_memenv_options_init(cldb_memenv_options_t *options);

/* 创建内存 Env；base_env 仅被引用且必须比返回值活得久，NULL 表示默认 Env。
 * 返回值归调用者，用 cldb_env_destroy 释放。 */
cldb_env_t *cldb_memenv_create(cldb_env_t *base_env);

/* 带选项版本；options 只在本调用内读取，NULL 等价默认选项。选项非法或
 * 分配失败返回 NULL。 */
cldb_env_t *cldb_memenv_create_with_options(
    cldb_env_t *base_env, const cldb_memenv_options_t *options);

#endif /* CLDB_SRC_HELPERS_MEMENV_MEMENV_H_ */
