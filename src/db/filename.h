#ifndef CLDB_SRC_DB_FILENAME_H_
#define CLDB_SRC_DB_FILENAME_H_

#include <stdbool.h>
#include <stdint.h>

#include "cleveldb/buf.h"
#include "cleveldb/env.h"
#include "cleveldb/status.h"

/* DB 文件命名与解析；对应上游 db/filename.h。文件编号在磁盘上为十进制，
 * 生成时至少补足 6 位，解析时允许不足 6 位，以便读取旧文件。 */

/* 文件类型；对应上游 FileType。 */
typedef enum cldb_file_type {
  CLDB_FILE_TYPE_LOG = 0,       /* 预写日志 .log */
  CLDB_FILE_TYPE_DB_LOCK = 1,   /* 数据库锁 LOCK */
  CLDB_FILE_TYPE_TABLE = 2,     /* SSTable .ldb 或 .sst */
  CLDB_FILE_TYPE_DESCRIPTOR = 3,/* MANIFEST 描述文件 */
  CLDB_FILE_TYPE_CURRENT = 4,   /* 指向当前 MANIFEST 的 CURRENT */
  CLDB_FILE_TYPE_TEMP = 5,      /* 临时文件 .dbtmp */
  CLDB_FILE_TYPE_INFO_LOG = 6   /* LOG 或 LOG.old */
} cldb_file_type_t;

/* 以下生成函数把完整路径写入 result（替换原内容），失败返回 false 且
 * result 不变；result 必须已初始化。number 为 0 时仍生成 000000，上游
 * debug 断言 number > 0，C 版保留确定性输出便于边界测试。 */
bool cldb_log_file_name(const char *dbname, uint64_t number,
                        cldb_buf_t *result);
bool cldb_table_file_name(const char *dbname, uint64_t number,
                          cldb_buf_t *result);
bool cldb_sst_table_file_name(const char *dbname, uint64_t number,
                              cldb_buf_t *result);
bool cldb_descriptor_file_name(const char *dbname, uint64_t number,
                               cldb_buf_t *result);
bool cldb_temp_file_name(const char *dbname, uint64_t number,
                         cldb_buf_t *result);

/* 无编号的固定文件名；语义同上面的生成函数。 */
bool cldb_current_file_name(const char *dbname, cldb_buf_t *result);
bool cldb_lock_file_name(const char *dbname, cldb_buf_t *result);
bool cldb_info_log_file_name(const char *dbname, cldb_buf_t *result);
bool cldb_old_info_log_file_name(const char *dbname, cldb_buf_t *result);

/* 解析单个文件名（不含目录）；对应上游 ParseFileName。成功写入 *number、
 * *type 并返回 true；失败返回 false 且输出不变。 */
bool cldb_parse_file_name(const char *filename, uint64_t *number,
                          cldb_file_type_t *type);

/* 原子更新 CURRENT；对应上游 SetCurrentFile。先写 <dbname>/<number>.dbtmp，
 * Sync 后 Rename 到 CURRENT；任一失败都删除临时文件。内容为 MANIFEST
 * basename 加换行。 */
cldb_status_t cldb_set_current_file(cldb_env_t *env, const char *dbname,
                                    uint64_t descriptor_number);

#endif /* CLDB_SRC_DB_FILENAME_H_ */
