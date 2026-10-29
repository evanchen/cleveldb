#ifndef CLDB_INCLUDE_CLEVELDB_OPTIONS_H_
#define CLDB_INCLUDE_CLEVELDB_OPTIONS_H_

#include <stdbool.h>
#include <stddef.h>

#include "cleveldb/cache.h"
#include "cleveldb/comparator.h"
#include "cleveldb/env.h"
#include "cleveldb/filter_policy.h"

/* 数据库选项；对应上游 include/leveldb/options.h。结构体按值复制，指针
 * 字段只引用对象，不负责释放。初始化用 cldb_options_init。 */

typedef struct cldb_snapshot cldb_snapshot_t; /* 数据库快照；仅引用 */

/* 块压缩类型；数值写入 SSTable，必须与上游保持一致。 */
typedef enum cldb_compression {
  CLDB_COMPRESSION_NONE = 0x0,
  CLDB_COMPRESSION_SNAPPY = 0x1,
  CLDB_COMPRESSION_ZSTD = 0x2
} cldb_compression_t;

typedef struct cldb_options {
  const cldb_comparator_t *comparator; /* 仅引用；NULL 表示字节序比较器 */
  cldb_env_t *env;                     /* 仅引用；NULL 表示默认 Env */
  cldb_logger_t *info_log;             /* 仅引用；NULL 表示默认 LOG 文件 */

  size_t write_buffer_size;   /* memtable 阈值；默认 4 MB */
  int max_open_files;         /* 打开文件上限；默认 1000 */
  cldb_cache_t *block_cache;  /* 仅引用；NULL 表示内部 8 MB 缓存 */
  size_t block_size;          /* 数据块目标大小；默认 4 KB */
  int block_restart_interval; /* 块内 restart 间隔；默认 16 */
  size_t max_file_size;       /* 输出表目标大小；默认 2 MB */

  cldb_compression_t compression; /* 默认 NONE，上游默认 Snappy */
  int zstd_compression_level;     /* 默认 1；未启用 Zstd 时忽略 */
  bool reuse_logs;                /* 打开时复用末尾 WAL；默认 false */
  const cldb_filter_policy_t *filter_policy; /* 仅引用；默认 NULL */
  bool paranoid_checks;           /* 默认 false */
  bool create_if_missing;         /* 默认 false */
  bool error_if_exists;           /* 默认 false */
} cldb_options_t;

/* 读选项；对应上游 ReadOptions。snapshot 必须属于同一 DB，仅引用。 */
typedef struct cldb_read_options {
  bool verify_checksums;           /* 默认 false */
  bool fill_cache;                 /* 默认 true */
  const cldb_snapshot_t *snapshot; /* NULL 表示隐式快照 */
} cldb_read_options_t;

/* 写选项；对应上游 WriteOptions。 */
typedef struct cldb_write_options {
  bool sync; /* 默认 false；true 时落盘后才算完成 */
} cldb_write_options_t;

/* 填入上游默认值，compression 例外为 NONE；接受 NULL。 */
void cldb_options_init(cldb_options_t *options);

/* 填入读选项默认值；接受 NULL。 */
void cldb_read_options_init(cldb_read_options_t *options);

/* 填入写选项默认值；接受 NULL。 */
void cldb_write_options_init(cldb_write_options_t *options);

#endif /* CLDB_INCLUDE_CLEVELDB_OPTIONS_H_ */
