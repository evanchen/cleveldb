#ifndef CLDB_TESTS_TEMP_DIR_H_
#define CLDB_TESTS_TEMP_DIR_H_

#include "cleveldb/env.h"

/* 测试临时目录；在 Env 的测试目录下创建唯一子目录，析构时递归清理。
 * 上游测试各自拼接测试目录；这里集中封装，避免残留影响后续用例。 */
typedef struct cldb_test_temp_dir {
  cldb_env_t *env; /* borrowed；生命周期必须覆盖 destroy */
  cldb_buf_t path; /* owned；创建成功后的目录路径 */
} cldb_test_temp_dir_t;

/* 创建唯一子目录；prefix 仅用于诊断残留，可为 NULL。成功时 dir->path
 * 保存路径，失败返回非 OK 且 dir 仍可安全 destroy。 */
cldb_status_t cldb_test_temp_dir_create(cldb_env_t *env, const char *prefix,
                                        cldb_test_temp_dir_t *dir);

/* 递归删除目录及其内容，并释放 path；接受 NULL。清理错误被忽略。 */
void cldb_test_temp_dir_destroy(cldb_test_temp_dir_t *dir);

/* 返回目录路径；dir 未创建或为 NULL 时返回空串。 */
const char *cldb_test_temp_dir_path(const cldb_test_temp_dir_t *dir);

#endif /* CLDB_TESTS_TEMP_DIR_H_ */
