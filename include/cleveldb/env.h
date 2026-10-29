#ifndef CLDB_INCLUDE_CLEVELDB_ENV_H_
#define CLDB_INCLUDE_CLEVELDB_ENV_H_

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cleveldb/buf.h"
#include "cleveldb/slice.h"
#include "cleveldb/status.h"

/* 平台接口；对应上游 leveldb::Env。把文件、线程、时间、日志等操作系统能力
 * 收敛到可替换的实现，便于测试注入故障。同一 Env 的普通方法可被多线程
 * 并发调用；destroy 是终止操作，不得与其它方法或尚未销毁的对象并发。
 * 具体实现把 cldb_env_t 放在结构体首成员，并在回调中转回外层类型。 */
typedef struct cldb_env cldb_env_t;

/* 以下文件/日志对象均由 Env 的 New* 方法创建，成功后归调用者所有，必须用
 * 对应 destroy 释放；基类结构必须放在具体实现的首成员。 */
typedef struct cldb_sequential_file cldb_sequential_file_t;
typedef struct cldb_random_access_file cldb_random_access_file_t;
typedef struct cldb_writable_file cldb_writable_file_t;
typedef struct cldb_logger cldb_logger_t;

/* 文件锁；对应上游 FileLock。由 LockFile 分配、UnlockFile 解锁并释放，调用
 * 者不得直接访问。state 供实现保存 fd/路径等私有数据。 */
typedef struct cldb_file_lock {
  void *state; /* 由锁实现管理；调用者不得读写 */
} cldb_file_lock_t;

/* 字符串数组；对应上游 std::vector<std::string>，用于 GetChildren 的结果。
 * 每个元素都是单独分配的 NUL 结尾字符串，由 cldb_strvec_free 释放。 */
typedef struct cldb_strvec {
  char **items;    /* 由 cldb_strvec_free 释放；每个元素另行分配 */
  size_t size;     /* 元素个数 */
  size_t capacity; /* items 可容纳的元素数；clear 后保留 */
} cldb_strvec_t;

/* 初始化为空数组；对应 vector 默认构造。接受 NULL。 */
void cldb_strvec_init(cldb_strvec_t *vec);

/* 释放所有元素和数组存储；对应 vector 析构。接受 NULL。 */
void cldb_strvec_free(cldb_strvec_t *vec);

/* 释放所有元素但保留数组容量；对应 vector::clear()。接受 NULL。 */
void cldb_strvec_clear(cldb_strvec_t *vec);

/* 复制 value 并追加到末尾；对应 vector::push_back()。value 必须非 NULL。
 * 分配失败返回 false 且数组不变。 */
bool cldb_strvec_push(cldb_strvec_t *vec, const char *value);

/* 返回元素个数；vec 为 NULL 时返回 0。 */
size_t cldb_strvec_size(const cldb_strvec_t *vec);

/* 返回内部元素指针，调用者不负责释放；越界或 vec 为 NULL 时返回 NULL。 */
const char *cldb_strvec_get(const cldb_strvec_t *vec, size_t index);

/* Env 方法表；对应上游 Env 的虚函数。除 new_appendable_file、
 * remove/delete_file、remove/delete_dir 和 destroy 可为 NULL 外，其余函数
 * 指针必须非 NULL。New* 成功时把新对象写入 *result，由调用者 destroy，并返回 OK；
 * 失败时写入 NULL 并返回非 OK。 */
typedef struct cldb_env_vtbl {
  /* 打开只读顺序文件；对应上游 NewSequentialFile。返回的文件只允许单线程
   * 使用，调用者负责外部同步。 */
  cldb_status_t (*new_sequential_file)(cldb_env_t *env, const char *fname,
                                       cldb_sequential_file_t **result);

  /* 打开只读随机访问文件；对应上游 NewRandomAccessFile。返回的文件可被多个
   * 线程并发读取。 */
  cldb_status_t (*new_random_access_file)(cldb_env_t *env, const char *fname,
                                          cldb_random_access_file_t **result);

  /* 创建/截断只写文件；对应上游 NewWritableFile。返回的文件只允许单线程
   * 使用，调用者负责外部同步。 */
  cldb_status_t (*new_writable_file)(cldb_env_t *env, const char *fname,
                                     cldb_writable_file_t **result);

  /* 打开追加写文件，不存在则创建；对应上游 NewAppendableFile。为 NULL 时
   * 调用返回 NotSupported，调用者应能处理这种 Env。 */
  cldb_status_t (*new_appendable_file)(cldb_env_t *env, const char *fname,
                                       cldb_writable_file_t **result);

  /* 文件是否存在；对应上游 FileExists。无法访问时返回 false。 */
  bool (*file_exists)(cldb_env_t *env, const char *fname);

  /* 把目录项名称追加到 result；对应上游 GetChildren。名称相对于 dir，
   * 实现只追加，调用前 result 已被清空。 */
  cldb_status_t (*get_children)(cldb_env_t *env, const char *dir,
                                cldb_strvec_t *result);

  /* 删除文件；对应上游 RemoveFile。为 NULL 时回退到 delete_file。 */
  cldb_status_t (*remove_file)(cldb_env_t *env, const char *fname);

  /* 兼容上游旧接口 DeleteFile；为 NULL 时回退到 remove_file。 */
  cldb_status_t (*delete_file)(cldb_env_t *env, const char *fname);

  /* 创建目录；对应上游 CreateDir。 */
  cldb_status_t (*create_dir)(cldb_env_t *env, const char *dirname);

  /* 删除空目录；对应上游 RemoveDir。为 NULL 时回退到 delete_dir。 */
  cldb_status_t (*remove_dir)(cldb_env_t *env, const char *dirname);

  /* 兼容上游旧接口 DeleteDir；为 NULL 时回退到 remove_dir。 */
  cldb_status_t (*delete_dir)(cldb_env_t *env, const char *dirname);

  /* 读取文件大小；对应上游 GetFileSize。成功时写入 *size。 */
  cldb_status_t (*get_file_size)(cldb_env_t *env, const char *fname,
                                 uint64_t *size);

  /* 重命名文件；对应上游 RenameFile。target 已存在时由实现决定是否替换。 */
  cldb_status_t (*rename_file)(cldb_env_t *env, const char *src,
                               const char *target);

  /* 获取文件锁；对应上游 LockFile。不等待已有锁；成功时把新锁写入 *lock，
   * 由调用者解锁，失败时写入 NULL。 */
  cldb_status_t (*lock_file)(cldb_env_t *env, const char *fname,
                             cldb_file_lock_t **lock);

  /* 释放并销毁 LockFile 返回的锁；对应上游 UnlockFile。成功后 lock 失效，
   * 调用者不得再次使用或释放。 */
  cldb_status_t (*unlock_file)(cldb_env_t *env, cldb_file_lock_t *lock);

  /* 把 fn(arg) 排入后台执行；对应上游 Schedule。回调可能在任意线程运行，
   * 同一 Env 的多个任务是否并发由实现决定。 */
  void (*schedule)(cldb_env_t *env, void (*fn)(void *), void *arg);

  /* 启动新线程执行 fn(arg)，返回后线程销毁；对应上游 StartThread。 */
  void (*start_thread)(cldb_env_t *env, void (*fn)(void *), void *arg);

  /* 返回测试用临时目录，重复调用返回同一路径；对应上游 GetTestDirectory。
   * 成功时替换 path 内容，path 由调用者初始化。 */
  cldb_status_t (*get_test_directory)(cldb_env_t *env, cldb_buf_t *path);

  /* 创建日志文件；对应上游 NewLogger。成功时 *result 归调用者所有。 */
  cldb_status_t (*new_logger)(cldb_env_t *env, const char *fname,
                              cldb_logger_t **result);

  /* 返回某个固定起点的微秒数，只适合求差值；对应上游 NowMicros，
   * 不保证单调。 */
  uint64_t (*now_micros)(cldb_env_t *env);

  /* 睡眠指定微秒数；对应上游 SleepForMicroseconds。非正值立即返回。 */
  void (*sleep_for_microseconds)(cldb_env_t *env, int micros);

  /* 释放自定义 Env；对应上游虚析构。为 NULL 表示无需清理（如默认单例或
   * 调用者提供的栈对象）。终止操作：调用者必须保证没有并发方法调用，
   * 且由本 Env 创建的文件/日志/锁对象均已销毁。 */
  void (*destroy)(cldb_env_t *env);
} cldb_env_vtbl_t;

struct cldb_env {
  const cldb_env_vtbl_t *vtbl; /* 不负责释放；非 NULL；通常 static const */
};

/* 打开只读顺序文件；语义见 cldb_env_vtbl_t::new_sequential_file。 */
static inline cldb_status_t cldb_env_new_sequential_file(
    cldb_env_t *env, const char *fname, cldb_sequential_file_t **result) {
  return env->vtbl->new_sequential_file(env, fname, result);
}

/* 打开只读随机访问文件；对应上游 Env::NewRandomAccessFile。 */
static inline cldb_status_t cldb_env_new_random_access_file(
    cldb_env_t *env, const char *fname, cldb_random_access_file_t **result) {
  return env->vtbl->new_random_access_file(env, fname, result);
}

/* 创建/截断只写文件；对应上游 Env::NewWritableFile。 */
static inline cldb_status_t cldb_env_new_writable_file(
    cldb_env_t *env, const char *fname, cldb_writable_file_t **result) {
  return env->vtbl->new_writable_file(env, fname, result);
}

/* 打开追加写文件；vtable 未提供实现时返回 NotSupported。 */
static inline cldb_status_t cldb_env_new_appendable_file(
    cldb_env_t *env, const char *fname, cldb_writable_file_t **result) {
  if (env->vtbl->new_appendable_file == NULL) {
    *result = NULL;
    return cldb_status_not_supported("NewAppendableFile", fname);
  }
  return env->vtbl->new_appendable_file(env, fname, result);
}

/* 判断文件是否存在；对应上游 Env::FileExists。 */
static inline bool cldb_env_file_exists(cldb_env_t *env, const char *fname) {
  return env->vtbl->file_exists(env, fname);
}

/* 读取目录项；对应上游 Env::GetChildren。先清空 result 再调用实现，保证
 * 重复使用同一数组时不会混入旧内容。 */
static inline cldb_status_t cldb_env_get_children(cldb_env_t *env,
                                                  const char *dir,
                                                  cldb_strvec_t *result) {
  cldb_strvec_clear(result);
  return env->vtbl->get_children(env, dir, result);
}

/* 删除文件；remove_file 为空时回退到 delete_file，避免互调递归。 */
static inline cldb_status_t cldb_env_remove_file(cldb_env_t *env,
                                                 const char *fname) {
  if (env->vtbl->remove_file != NULL) {
    return env->vtbl->remove_file(env, fname);
  }
  if (env->vtbl->delete_file != NULL) {
    return env->vtbl->delete_file(env, fname);
  }
  return cldb_status_not_supported("RemoveFile", fname);
}

/* 兼容旧接口删除文件；delete_file 为空时回退到 remove_file。 */
static inline cldb_status_t cldb_env_delete_file(cldb_env_t *env,
                                                 const char *fname) {
  if (env->vtbl->delete_file != NULL) {
    return env->vtbl->delete_file(env, fname);
  }
  if (env->vtbl->remove_file != NULL) {
    return env->vtbl->remove_file(env, fname);
  }
  return cldb_status_not_supported("DeleteFile", fname);
}

/* 创建目录；对应上游 Env::CreateDir。 */
static inline cldb_status_t cldb_env_create_dir(cldb_env_t *env,
                                                const char *dirname) {
  return env->vtbl->create_dir(env, dirname);
}

/* 删除空目录；remove_dir 为空时回退到 delete_dir。 */
static inline cldb_status_t cldb_env_remove_dir(cldb_env_t *env,
                                                const char *dirname) {
  if (env->vtbl->remove_dir != NULL) {
    return env->vtbl->remove_dir(env, dirname);
  }
  if (env->vtbl->delete_dir != NULL) {
    return env->vtbl->delete_dir(env, dirname);
  }
  return cldb_status_not_supported("RemoveDir", dirname);
}

/* 兼容旧接口删除目录；delete_dir 为空时回退到 remove_dir。 */
static inline cldb_status_t cldb_env_delete_dir(cldb_env_t *env,
                                                const char *dirname) {
  if (env->vtbl->delete_dir != NULL) {
    return env->vtbl->delete_dir(env, dirname);
  }
  if (env->vtbl->remove_dir != NULL) {
    return env->vtbl->remove_dir(env, dirname);
  }
  return cldb_status_not_supported("DeleteDir", dirname);
}

/* 读取文件大小；对应上游 Env::GetFileSize。 */
static inline cldb_status_t cldb_env_get_file_size(cldb_env_t *env,
                                                   const char *fname,
                                                   uint64_t *size) {
  return env->vtbl->get_file_size(env, fname, size);
}

/* 重命名文件；对应上游 Env::RenameFile。 */
static inline cldb_status_t cldb_env_rename_file(cldb_env_t *env,
                                                 const char *src,
                                                 const char *target) {
  return env->vtbl->rename_file(env, src, target);
}

/* 获取文件锁；对应上游 Env::LockFile。 */
static inline cldb_status_t cldb_env_lock_file(cldb_env_t *env,
                                               const char *fname,
                                               cldb_file_lock_t **lock) {
  return env->vtbl->lock_file(env, fname, lock);
}

/* 解锁并销毁锁；对应上游 Env::UnlockFile。 */
static inline cldb_status_t cldb_env_unlock_file(cldb_env_t *env,
                                                 cldb_file_lock_t *lock) {
  return env->vtbl->unlock_file(env, lock);
}

/* 排入后台任务；对应上游 Env::Schedule。 */
static inline void cldb_env_schedule(cldb_env_t *env, void (*fn)(void *),
                                     void *arg) {
  env->vtbl->schedule(env, fn, arg);
}

/* 启动 detached 线程；对应上游 Env::StartThread。 */
static inline void cldb_env_start_thread(cldb_env_t *env, void (*fn)(void *),
                                         void *arg) {
  env->vtbl->start_thread(env, fn, arg);
}

/* 读取测试目录；对应上游 Env::GetTestDirectory。path 由调用者初始化，
 * 失败时内容可能不变或只写入了部分内容。 */
static inline cldb_status_t cldb_env_get_test_directory(cldb_env_t *env,
                                                        cldb_buf_t *path) {
  return env->vtbl->get_test_directory(env, path);
}

/* 创建日志；对应上游 Env::NewLogger。 */
static inline cldb_status_t cldb_env_new_logger(cldb_env_t *env,
                                                const char *fname,
                                                cldb_logger_t **result) {
  return env->vtbl->new_logger(env, fname, result);
}

/* 读取当前微秒数；对应上游 Env::NowMicros。 */
static inline uint64_t cldb_env_now_micros(cldb_env_t *env) {
  return env->vtbl->now_micros(env);
}

/* 睡眠指定微秒数；对应上游 Env::SleepForMicroseconds。 */
static inline void cldb_env_sleep_for_microseconds(cldb_env_t *env,
                                                   int micros) {
  env->vtbl->sleep_for_microseconds(env, micros);
}

/* 销毁自定义 Env；接受 NULL。destroy 为空时无操作。默认 Env 由库管理，
 * 不得调用本函数。 */
void cldb_env_destroy(cldb_env_t *env);

/* 顺序读文件；对应上游 SequentialFile。Read 要求调用者外部同步。 */
typedef struct cldb_sequential_file_vtbl {
  /* 最多读 n 字节；对应上游 Read。成功时把数据写入 *result（可能指向
   * scratch，也可能指向实现内部存储），到达文件末尾返回空 Slice 和 OK。n 为
   * 0 时 scratch 可为 NULL；否则 scratch 至少有 n 字节可写。 */
  cldb_status_t (*read)(cldb_sequential_file_t *file, size_t n,
                        cldb_slice_t *result, char *scratch);

  /* 向前跳过 n 字节，到达末尾即停止并返回 OK；对应上游 Skip。 */
  cldb_status_t (*skip)(cldb_sequential_file_t *file, uint64_t n);

  /* 释放文件；对应上游虚析构。 */
  void (*destroy)(cldb_sequential_file_t *file);
} cldb_sequential_file_vtbl_t;

struct cldb_sequential_file {
  const cldb_sequential_file_vtbl_t *vtbl; /* 不负责释放；非 NULL */
};

/* 读取最多 n 字节；对应上游 SequentialFile::Read。 */
static inline cldb_status_t cldb_sequential_file_read(
    cldb_sequential_file_t *file, size_t n, cldb_slice_t *result,
    char *scratch) {
  return file->vtbl->read(file, n, result, scratch);
}

/* 跳过 n 字节；对应上游 SequentialFile::Skip。 */
static inline cldb_status_t cldb_sequential_file_skip(
    cldb_sequential_file_t *file, uint64_t n) {
  return file->vtbl->skip(file, n);
}

/* 销毁顺序文件；接受 NULL。每个 NewSequentialFile 成功结果必须调用一次。 */
void cldb_sequential_file_destroy(cldb_sequential_file_t *file);

/* 随机读文件；对应上游 RandomAccessFile。Read 必须线程安全。 */
typedef struct cldb_random_access_file_vtbl {
  /* 从 offset 读取最多 n 字节；对应上游 Read。语义同顺序读，scratch 要求
   * 相同；多个线程可同时调用。 */
  cldb_status_t (*read)(cldb_random_access_file_t *file, uint64_t offset,
                        size_t n, cldb_slice_t *result, char *scratch);

  /* 释放文件；对应上游虚析构。 */
  void (*destroy)(cldb_random_access_file_t *file);
} cldb_random_access_file_vtbl_t;

struct cldb_random_access_file {
  const cldb_random_access_file_vtbl_t *vtbl; /* 不负责释放；非 NULL */
};

/* 从 offset 读取最多 n 字节；对应上游 RandomAccessFile::Read。 */
static inline cldb_status_t cldb_random_access_file_read(
    cldb_random_access_file_t *file, uint64_t offset, size_t n,
    cldb_slice_t *result, char *scratch) {
  return file->vtbl->read(file, offset, n, result, scratch);
}

/* 销毁随机读文件；接受 NULL。 */
void cldb_random_access_file_destroy(cldb_random_access_file_t *file);

/* 顺序写文件；对应上游 WritableFile。实现必须内部缓冲，调用者按顺序
 * Append，且只允许单线程使用。 */
typedef struct cldb_writable_file_vtbl {
  /* 追加数据；对应上游 Append。失败时磁盘上可能已有部分字节。 */
  cldb_status_t (*append)(cldb_writable_file_t *file, cldb_slice_t data);

  /* 刷新并关闭文件；对应上游 Close。成功或失败后都不得再 Append；Close
   * 不隐含持久化同步。 */
  cldb_status_t (*close)(cldb_writable_file_t *file);

  /* 把用户态缓冲交给操作系统；对应上游 Flush，不保证掉电持久。 */
  cldb_status_t (*flush)(cldb_writable_file_t *file);

  /* 刷新并等待数据落盘；对应上游 Sync。 */
  cldb_status_t (*sync)(cldb_writable_file_t *file);

  /* 释放文件；对应上游虚析构。未 Close 时实现应自行关闭；Close 后仍需调用
   * 本函数释放对象。 */
  void (*destroy)(cldb_writable_file_t *file);
} cldb_writable_file_vtbl_t;

struct cldb_writable_file {
  const cldb_writable_file_vtbl_t *vtbl; /* 不负责释放；非 NULL */
};

/* 追加数据；对应上游 WritableFile::Append。 */
static inline cldb_status_t cldb_writable_file_append(
    cldb_writable_file_t *file, cldb_slice_t data) {
  return file->vtbl->append(file, data);
}

/* 刷新并关闭文件；对应上游 WritableFile::Close。 */
static inline cldb_status_t cldb_writable_file_close(
    cldb_writable_file_t *file) {
  return file->vtbl->close(file);
}

/* 刷新用户态缓冲；对应上游 WritableFile::Flush。 */
static inline cldb_status_t cldb_writable_file_flush(
    cldb_writable_file_t *file) {
  return file->vtbl->flush(file);
}

/* 等待数据落盘；对应上游 WritableFile::Sync。 */
static inline cldb_status_t cldb_writable_file_sync(
    cldb_writable_file_t *file) {
  return file->vtbl->sync(file);
}

/* 销毁写文件；接受 NULL。 */
void cldb_writable_file_destroy(cldb_writable_file_t *file);

/* 日志接口；对应上游 Logger。实现必须保证 Logv 线程安全。 */
typedef struct cldb_logger_vtbl {
  /* 按 printf 格式写入一条日志；对应上游 Logger::Logv。ap 由实现消费，
   * 调用者之后不得再使用。 */
  void (*logv)(cldb_logger_t *logger, const char *format, va_list ap);

  /* 释放日志；对应上游虚析构。 */
  void (*destroy)(cldb_logger_t *logger);
} cldb_logger_vtbl_t;

struct cldb_logger {
  const cldb_logger_vtbl_t *vtbl; /* 不负责释放；非 NULL */
};

/* 写入一条日志；对应上游 Logger::Logv。 */
static inline void cldb_logger_logv(cldb_logger_t *logger, const char *format,
                                    va_list ap) {
  logger->vtbl->logv(logger, format, ap);
}

/* 销毁日志；接受 NULL。每个 NewLogger 成功结果必须调用一次。 */
void cldb_logger_destroy(cldb_logger_t *logger);

/* 格式化并写入日志；对应上游 Log。logger 或 format 为 NULL 时直接返回，
 * 不读取可变参数。 */
void cldb_log(cldb_logger_t *logger, const char *format, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((__format__(__printf__, 2, 3)))
#endif
    ;

/* 把 data 写入 fname；对应上游 WriteStringToFile。先创建/截断文件，失败时
 * 尽力删除残留文件并返回原错误。 */
cldb_status_t cldb_write_string_to_file(cldb_env_t *env, cldb_slice_t data,
                                        const char *fname);

/* 同 cldb_write_string_to_file，但在 Close 前 Sync；对应上游
 * WriteStringToFileSync。 */
cldb_status_t cldb_write_string_to_file_sync(cldb_env_t *env,
                                             cldb_slice_t data,
                                             const char *fname);

/* 读取整个文件到 data；对应上游 ReadFileToString。进入时清空 data，失败时
 * data 可能保留已读到的前缀，调用者应丢弃。 */
cldb_status_t cldb_read_file_to_string(cldb_env_t *env, const char *fname,
                                       cldb_buf_t *data);

/* 返回默认 Env 单例；对应上游 Env::Default。对象由库管理，不得销毁，
 * 由平台实现（Linux/POSIX）提供。 */
cldb_env_t *cldb_env_default(void);

/* 转发 Env；对应上游 EnvWrapper。用于只覆盖部分方法或注入故障。base 必须
 * 为首成员，wrapper 存储由调用者管理，target 仅被引用。 */
typedef struct cldb_env_wrapper {
  cldb_env_t base;      /* 必须为首成员 */
  cldb_env_t *target;   /* 仅引用；不得为 NULL */
} cldb_env_wrapper_t;

/* 初始化 wrapper，把所有方法转发给 target；不接管 target 的释放。 */
void cldb_env_wrapper_init(cldb_env_wrapper_t *wrapper, cldb_env_t *target);

/* 返回被转发的 target；wrapper 为 NULL 时返回 NULL。 */
static inline cldb_env_t *cldb_env_wrapper_target(
    const cldb_env_wrapper_t *wrapper) {
  return wrapper != NULL ? wrapper->target : NULL;
}

#endif /* CLDB_INCLUDE_CLEVELDB_ENV_H_ */
