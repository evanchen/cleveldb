#include "cleveldb/env.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "port/port.h"
#include "port/port_mutex.h"
#include "port/port_thread.h"
#include "util/env_internal.h"

/* Linux/POSIX 默认 Env；对应上游 util/env_posix.cc 与 posix_logger.h。
 * 随机读只保留 pread 路径，不移植 mmap；文件操作统一带 O_CLOEXEC。 */

/* 所有 open 共用的标志；对应上游 kOpenBaseFlags。 */
static const int kOpenBaseFlags = O_CLOEXEC;

/* 可写文件的用户态缓冲大小；对应上游 kWritableFileBufferSize。 */
enum { kWritableFileBufferSize = 65536 };

/* errno 到状态的映射；对应上游 PosixError。 */
static cldb_status_t posix_error(const char *context, int error_number) {
  if (error_number == ENOENT) {
    return cldb_status_not_found(context, strerror(error_number));
  }
  return cldb_status_io(context, strerror(error_number));
}

/* 取路径的目录部分；对应上游 PosixWritableFile::Dirname。 */
static char *path_dirname(const char *filename) {
  const char *slash = strrchr(filename, '/');
  size_t length;
  char *dirname;

  if (slash == NULL) {
    return cldb_env_strdup(".");
  }
  length = (size_t)(slash - filename);
  dirname = malloc(length + 1);
  if (dirname != NULL) {
    memcpy(dirname, filename, length);
    dirname[length] = '\0';
  }
  return dirname;
}

/* 文件名是否以 MANIFEST 开头；对应上游 IsManifest。 */
static bool is_manifest_file(const char *filename) {
  const char *base = strrchr(filename, '/');
  const char *name = base != NULL ? base + 1 : filename;

  return strncmp(name, "MANIFEST", 8) == 0;
}

/* ---------- 顺序读文件 ---------- */

typedef struct posix_sequential_file {
  cldb_sequential_file_t base; /* 必须为首成员 */
  int fd;                      /* 打开时获得；destroy 关闭 */
  char *fname;                 /* 由本对象释放；仅用于错误消息 */
} posix_sequential_file_t;

/* 对应上游 PosixSequentialFile::Read；EINTR 自动重试。 */
static cldb_status_t sequential_file_read(cldb_sequential_file_t *file,
                                          size_t n, cldb_slice_t *result,
                                          char *scratch) {
  posix_sequential_file_t *self = (posix_sequential_file_t *)file;

  for (;;) {
    ssize_t read_size = read(self->fd, scratch, n);

    if (read_size < 0) {
      if (errno == EINTR) {
        continue;
      }
      *result = cldb_slice_empty();
      return posix_error(self->fname, errno);
    }
    *result = cldb_slice_make(scratch, (size_t)read_size);
    return cldb_status_ok();
  }
}

/* 对应上游 PosixSequentialFile::Skip。 */
static cldb_status_t sequential_file_skip(cldb_sequential_file_t *file,
                                          uint64_t n) {
  posix_sequential_file_t *self = (posix_sequential_file_t *)file;

  if (lseek(self->fd, (off_t)n, SEEK_CUR) == (off_t)-1) {
    return posix_error(self->fname, errno);
  }
  return cldb_status_ok();
}

/* 对应上游 PosixSequentialFile 析构；close 错误无法上报。 */
static void sequential_file_destroy(cldb_sequential_file_t *file) {
  posix_sequential_file_t *self = (posix_sequential_file_t *)file;

  (void)close(self->fd);
  free(self->fname);
  free(self);
}

static const cldb_sequential_file_vtbl_t kSequentialFileVtbl = {
    .read = sequential_file_read,
    .skip = sequential_file_skip,
    .destroy = sequential_file_destroy,
};

/* ---------- 随机读文件 ---------- */

typedef struct posix_random_access_file {
  cldb_random_access_file_t base; /* 必须为首成员 */
  int fd;                         /* 打开时获得；destroy 关闭 */
  char *fname;                    /* 由本对象释放；仅用于错误消息 */
} posix_random_access_file_t;

/* 对应上游 PosixRandomAccessFile::Read；pread 不改变文件偏移，可并发。 */
static cldb_status_t random_access_file_read(cldb_random_access_file_t *file,
                                             uint64_t offset, size_t n,
                                             cldb_slice_t *result,
                                             char *scratch) {
  posix_random_access_file_t *self = (posix_random_access_file_t *)file;
  ssize_t read_size = pread(self->fd, scratch, n, (off_t)offset);

  if (read_size < 0) {
    *result = cldb_slice_empty();
    return posix_error(self->fname, errno);
  }
  *result = cldb_slice_make(scratch, (size_t)read_size);
  return cldb_status_ok();
}

static void random_access_file_destroy(cldb_random_access_file_t *file) {
  posix_random_access_file_t *self = (posix_random_access_file_t *)file;

  (void)close(self->fd);
  free(self->fname);
  free(self);
}

static const cldb_random_access_file_vtbl_t kRandomAccessFileVtbl = {
    .read = random_access_file_read,
    .destroy = random_access_file_destroy,
};

/* ---------- 可写文件 ---------- */

typedef struct posix_writable_file {
  cldb_writable_file_t base; /* 必须为首成员 */
  char buffer[kWritableFileBufferSize]; /* 对应上游 buf_ */
  size_t pos;                           /* buffer 中待写字节数 */
  int fd;                               /* 打开时获得；Close 后为 -1 */
  bool is_manifest;                     /* Sync 前需要先同步目录 */
  char *fname;                          /* 由本对象释放；仅用于错误消息 */
  char *dirname;                        /* 由本对象释放；MANIFEST 所在目录 */
} posix_writable_file_t;

/* 循环写直到写完；对应上游 WriteUnbuffered。 */
static cldb_status_t write_unbuffered(posix_writable_file_t *file,
                                      const char *data, size_t size) {
  while (size > 0) {
    ssize_t written = write(file->fd, data, size);

    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return posix_error(file->fname, errno);
    }
    if (written == 0) {
      return cldb_status_io(file->fname, "write returned 0");
    }
    data += (size_t)written;
    size -= (size_t)written;
  }
  return cldb_status_ok();
}

/* 对应上游 FlushBuffer；失败时同样丢弃已缓冲内容。 */
static cldb_status_t flush_buffer(posix_writable_file_t *file) {
  cldb_status_t status =
      write_unbuffered(file, file->buffer, file->pos);

  file->pos = 0;
  return status;
}

/* 对应上游 PosixWritableFile::Append；小于缓冲区的尾巴留在用户态。 */
static cldb_status_t writable_file_append(cldb_writable_file_t *file,
                                          cldb_slice_t data) {
  posix_writable_file_t *self = (posix_writable_file_t *)file;
  size_t write_size = data.size;
  const char *write_data = data.data;
  size_t copy_size;
  cldb_status_t status;

  if (write_size > 0 && write_data == NULL) {
    return cldb_status_invalid_argument("Append", "data is NULL");
  }
  copy_size = write_size < kWritableFileBufferSize - self->pos
                  ? write_size
                  : kWritableFileBufferSize - self->pos;
  if (copy_size > 0) {
    memcpy(self->buffer + self->pos, write_data, copy_size);
    write_data += copy_size;
  }
  write_size -= copy_size;
  self->pos += copy_size;
  if (write_size == 0) {
    return cldb_status_ok();
  }

  status = flush_buffer(self);
  if (!cldb_status_is_ok(status)) {
    return status;
  }
  if (write_size < kWritableFileBufferSize) {
    memcpy(self->buffer, write_data, write_size);
    self->pos = write_size;
    return cldb_status_ok();
  }
  return write_unbuffered(self, write_data, write_size);
}

/* 对应上游 PosixWritableFile::Close；flush 失败时仍关闭 fd。 */
static cldb_status_t writable_file_close(cldb_writable_file_t *file) {
  posix_writable_file_t *self = (posix_writable_file_t *)file;
  cldb_status_t status = flush_buffer(self);
  int close_result = close(self->fd);

  if (close_result < 0 && cldb_status_is_ok(status)) {
    status = posix_error(self->fname, errno);
  }
  self->fd = -1;
  return status;
}

/* 对应上游 PosixWritableFile::Flush。 */
static cldb_status_t writable_file_flush(cldb_writable_file_t *file) {
  return flush_buffer((posix_writable_file_t *)file);
}

/* 同步 fd 内容；Linux 优先 fdatasync，不支持时回退 fsync。 */
static cldb_status_t sync_fd(int fd, const char *path) {
  if (fdatasync(fd) == 0) {
    return cldb_status_ok();
  }
  if (errno == EINVAL || errno == ENOSYS) {
    if (fsync(fd) == 0) {
      return cldb_status_ok();
    }
  }
  return posix_error(path, errno);
}

/* 对应上游 SyncDirIfManifest：先确保 MANIFEST 引用的文件已落盘。 */
static cldb_status_t sync_dir_if_manifest(posix_writable_file_t *file) {
  int fd;
  cldb_status_t status;

  if (!file->is_manifest) {
    return cldb_status_ok();
  }
  fd = open(file->dirname, O_RDONLY | kOpenBaseFlags);
  if (fd < 0) {
    return posix_error(file->dirname, errno);
  }
  status = sync_fd(fd, file->dirname);
  (void)close(fd);
  return status;
}

/* 对应上游 PosixWritableFile::Sync。 */
static cldb_status_t writable_file_sync(cldb_writable_file_t *file) {
  posix_writable_file_t *self = (posix_writable_file_t *)file;
  cldb_status_t status = sync_dir_if_manifest(self);

  if (!cldb_status_is_ok(status)) {
    return status;
  }
  status = flush_buffer(self);
  if (!cldb_status_is_ok(status)) {
    return status;
  }
  return sync_fd(self->fd, self->fname);
}

/* 对应上游 PosixWritableFile 析构；未 Close 时忽略关闭错误。 */
static void writable_file_destroy(cldb_writable_file_t *file) {
  posix_writable_file_t *self = (posix_writable_file_t *)file;

  if (self->fd >= 0) {
    (void)writable_file_close(file);
  }
  free(self->fname);
  free(self->dirname);
  free(self);
}

static const cldb_writable_file_vtbl_t kWritableFileVtbl = {
    .append = writable_file_append,
    .close = writable_file_close,
    .flush = writable_file_flush,
    .sync = writable_file_sync,
    .destroy = writable_file_destroy,
};

/* 用已打开的 fd 构造可写文件；失败时关闭 fd 并清空输出。 */
static cldb_status_t make_writable_file(const char *fname, int fd,
                                        cldb_writable_file_t **result) {
  posix_writable_file_t *file = malloc(sizeof(*file));

  if (file == NULL) {
    (void)close(fd);
    *result = NULL;
    return cldb_env_out_of_memory("NewWritableFile");
  }
  file->pos = 0;
  file->fd = fd;
  file->is_manifest = is_manifest_file(fname);
  file->fname = cldb_env_strdup(fname);
  file->dirname = path_dirname(fname);
  if (file->fname == NULL || file->dirname == NULL) {
    free(file->fname);
    free(file->dirname);
    (void)close(fd);
    free(file);
    *result = NULL;
    return cldb_env_out_of_memory("NewWritableFile");
  }
  file->base.vtbl = &kWritableFileVtbl;
  *result = &file->base;
  return cldb_status_ok();
}

/* ---------- 文件锁 ---------- */

typedef struct posix_file_lock {
  cldb_file_lock_t base; /* 必须为首成员 */
  int fd;                /* UnlockFile 关闭 */
  char *fname;           /* 由本对象释放；用于锁表与错误消息 */
} posix_file_lock_t;

/* 进程内已锁文件表；对应上游 PosixLockTable，用于弥补 fcntl 不防同进程
 * 重复加锁的问题。 */
typedef struct posix_lock_table {
  cldb_mutex_t mutex; /* 保护 names/size/capacity */
  char **names;       /* 由本对象释放；每项为复制后的路径 */
  size_t size;        /* 已锁文件数 */
  size_t capacity;    /* names 可容纳的元素数 */
} posix_lock_table_t;

typedef enum lock_insert_result {
  LOCK_INSERT_OK,
  LOCK_INSERT_DUPLICATE,
  LOCK_INSERT_NO_MEMORY
} lock_insert_result_t;

/* 插入路径；对应上游 PosixLockTable::Insert。 */
static lock_insert_result_t lock_table_insert(posix_lock_table_t *table,
                                              const char *fname) {
  size_t i;
  char *copy = NULL;

  if (!cldb_status_is_ok(cldb_mutex_lock(&table->mutex))) {
    return LOCK_INSERT_NO_MEMORY;
  }
  for (i = 0; i < table->size; ++i) {
    if (strcmp(table->names[i], fname) == 0) {
      (void)cldb_mutex_unlock(&table->mutex);
      return LOCK_INSERT_DUPLICATE;
    }
  }
  if (table->size == table->capacity) {
    size_t new_capacity;
    char **new_names;

    if (table->capacity == 0) {
      new_capacity = 8;
    } else {
      if (table->capacity > SIZE_MAX / 2) {
        (void)cldb_mutex_unlock(&table->mutex);
        return LOCK_INSERT_NO_MEMORY;
      }
      new_capacity = table->capacity * 2;
    }
    if (new_capacity > SIZE_MAX / sizeof(*new_names)) {
      (void)cldb_mutex_unlock(&table->mutex);
      return LOCK_INSERT_NO_MEMORY;
    }
    new_names = realloc(table->names, new_capacity * sizeof(*new_names));
    if (new_names == NULL) {
      (void)cldb_mutex_unlock(&table->mutex);
      return LOCK_INSERT_NO_MEMORY;
    }
    table->names = new_names;
    table->capacity = new_capacity;
  }
  copy = cldb_env_strdup(fname);
  if (copy == NULL) {
    (void)cldb_mutex_unlock(&table->mutex);
    return LOCK_INSERT_NO_MEMORY;
  }
  table->names[table->size] = copy;
  table->size++;
  (void)cldb_mutex_unlock(&table->mutex);
  return LOCK_INSERT_OK;
}

/* 移除路径；对应上游 PosixLockTable::Remove。 */
static void lock_table_remove(posix_lock_table_t *table, const char *fname) {
  size_t i;

  if (!cldb_status_is_ok(cldb_mutex_lock(&table->mutex))) {
    return;
  }
  for (i = 0; i < table->size; ++i) {
    if (strcmp(table->names[i], fname) == 0) {
      free(table->names[i]);
      table->names[i] = table->names[table->size - 1];
      table->size--;
      break;
    }
  }
  (void)cldb_mutex_unlock(&table->mutex);
}

/* 对 fd 加/解整文件锁；对应上游 LockOrUnlock。 */
static int lock_or_unlock(int fd, bool lock) {
  struct flock file_lock;

  memset(&file_lock, 0, sizeof(file_lock));
  file_lock.l_type = lock ? F_WRLCK : F_UNLCK;
  file_lock.l_whence = SEEK_SET;
  file_lock.l_start = 0;
  file_lock.l_len = 0;
  return fcntl(fd, F_SETLK, &file_lock);
}

/* ---------- 日志 ---------- */

typedef struct posix_logger {
  cldb_logger_t base; /* 必须为首成员 */
  FILE *fp;           /* destroy 关闭；flockfile 保护并发写 */
} posix_logger_t;

/* 对应上游 PosixLogger::Logv；时间戳格式与上游一致。 */
static void logger_logv(cldb_logger_t *logger, const char *format, va_list ap) {
  enum { kStackBufferSize = 512 };
  posix_logger_t *self = (posix_logger_t *)logger;
  struct timeval now;
  struct tm components;
  time_t seconds;
  char stack_buffer[kStackBufferSize];
  char *heap_buffer = NULL;
  char *buffer = stack_buffer;
  int buffer_size = kStackBufferSize;
  int total = 0;
  int iteration;

  if (format == NULL) {
    return;
  }
  (void)gettimeofday(&now, NULL);
  seconds = now.tv_sec;
  if (localtime_r(&seconds, &components) == NULL) {
    memset(&components, 0, sizeof(components));
  }
  for (iteration = 0; iteration < 2; ++iteration) {
    va_list copy;
    int header_size;
    int message_size;

    header_size = snprintf(
        buffer, (size_t)buffer_size, "%04d/%02d/%02d-%02d:%02d:%02d.%06d %lu ",
        components.tm_year + 1900, components.tm_mon + 1, components.tm_mday,
        components.tm_hour, components.tm_min, components.tm_sec,
        (int)now.tv_usec, (unsigned long)pthread_self());
    if (header_size < 0 || header_size >= buffer_size) {
      goto cleanup;
    }
    va_copy(copy, ap);
    message_size = vsnprintf(buffer + header_size,
                             (size_t)(buffer_size - header_size), format, copy);
    va_end(copy);
    if (message_size < 0 || message_size > INT_MAX - header_size - 2) {
      goto cleanup;
    }
    total = header_size + message_size;
    if (total < buffer_size - 1) {
      break;
    }
    if (iteration != 0) {
      /* 动态缓冲区仍不足：截断，保证一定写出内容。 */
      total = buffer_size - 1;
      break;
    }
    heap_buffer = malloc((size_t)total + 2);
    if (heap_buffer == NULL) {
      total = buffer_size - 1;
      break;
    }
    buffer = heap_buffer;
    buffer_size = total + 2;
  }
  if (total > 0 && buffer[total - 1] != '\n') {
    buffer[total] = '\n';
    total++;
  }
  flockfile(self->fp);
  (void)fwrite(buffer, 1, (size_t)total, self->fp);
  (void)fflush(self->fp);
  funlockfile(self->fp);

cleanup:
  free(heap_buffer);
}

/* 对应上游 PosixLogger 析构。 */
static void logger_destroy(cldb_logger_t *logger) {
  posix_logger_t *self = (posix_logger_t *)logger;

  (void)fclose(self->fp);
  free(self);
}

static const cldb_logger_vtbl_t kLoggerVtbl = {
    .logv = logger_logv,
    .destroy = logger_destroy,
};

/* ---------- 后台任务 ---------- */

typedef struct background_work {
  void (*fn)(void *); /* 仅引用；任务回调 */
  void *arg;          /* 仅引用；回调参数，执行前必须保持有效 */
  struct background_work *next; /* 由本队列释放；FIFO 链接 */
} background_work_t;

/* 默认 Env；对应上游 PosixEnv。所有可变字段由内部锁保护。 */
typedef struct posix_env {
  cldb_env_t base; /* 必须为首成员 */
  cldb_mutex_t background_mutex; /* 保护下面的队列与 started 标志 */
  cldb_condvar_t background_cv;  /* 绑定 background_mutex */
  bool background_started;       /* 后台线程已创建 */
  background_work_t *background_head; /* 由本对象释放；最旧任务 */
  background_work_t *background_tail; /* 由本对象释放；最新任务 */
  posix_lock_table_t locks;           /* 进程内文件锁表 */
} posix_env_t;

/* 内部同步原语误用只可能是生命周期错误；release 下调用也不能省。 */
static void background_lock(posix_env_t *env) {
  cldb_status_t status = cldb_mutex_lock(&env->background_mutex);

  CLDB_UNUSED(status);
  CLDB_ASSERT(cldb_status_is_ok(status));
}

static void background_unlock(posix_env_t *env) {
  cldb_status_t status = cldb_mutex_unlock(&env->background_mutex);

  CLDB_UNUSED(status);
  CLDB_ASSERT(cldb_status_is_ok(status));
}

static void background_wait(posix_env_t *env) {
  cldb_status_t status = cldb_condvar_wait(&env->background_cv);

  CLDB_UNUSED(status);
  CLDB_ASSERT(cldb_status_is_ok(status));
}

static void background_signal(posix_env_t *env) {
  cldb_status_t status = cldb_condvar_signal(&env->background_cv);

  CLDB_UNUSED(status);
  CLDB_ASSERT(cldb_status_is_ok(status));
}

/* 创建 detached 线程；返回 false 时调用者负责回退执行。 */
static bool start_detached_thread(void *(*fn)(void *), void *arg) {
  pthread_t thread;
  int error = pthread_create(&thread, NULL, fn, arg);

  if (error != 0) {
    (void)fprintf(stderr, "cleveldb: pthread_create failed: %s\n",
                  strerror(error));
    return false;
  }
  error = pthread_detach(thread);
  if (error != 0) {
    (void)fprintf(stderr, "cleveldb: pthread_detach failed: %s\n",
                  strerror(error));
  }
  return true;
}

/* 后台线程主循环；对应上游 BackgroundThreadMain，任务严格串行。 */
static void *background_thread_main(void *arg) {
  posix_env_t *env = arg;

  for (;;) {
    background_work_t *work;

    background_lock(env);
    while (env->background_head == NULL) {
      background_wait(env);
    }
    work = env->background_head;
    env->background_head = work->next;
    if (env->background_head == NULL) {
      env->background_tail = NULL;
    }
    background_unlock(env);
    work->fn(work->arg);
    free(work);
  }
  return NULL;
}

/* StartThread 的参数包；线程里取出回调与参数后立即释放。 */
typedef struct start_thread_context {
  void (*fn)(void *); /* 仅引用 */
  void *arg;          /* 仅引用 */
} start_thread_context_t;

static void *start_thread_trampoline(void *arg) {
  start_thread_context_t *context = arg;
  void (*fn)(void *) = context->fn;
  void *fn_arg = context->arg;

  free(context);
  fn(fn_arg);
  return NULL;
}

/* ---------- Env 方法 ---------- */

static cldb_status_t posix_new_sequential_file(
    cldb_env_t *env, const char *fname, cldb_sequential_file_t **result) {
  posix_sequential_file_t *file;
  int fd;

  CLDB_UNUSED(env);
  if (result == NULL) {
    return cldb_status_invalid_argument("NewSequentialFile",
                                        "result is NULL");
  }
  *result = NULL;
  if (fname == NULL) {
    return cldb_status_invalid_argument("NewSequentialFile",
                                        "fname is NULL");
  }
  fd = open(fname, O_RDONLY | kOpenBaseFlags);
  if (fd < 0) {
    return posix_error(fname, errno);
  }
  file = malloc(sizeof(*file));
  if (file == NULL) {
    (void)close(fd);
    return cldb_env_out_of_memory("NewSequentialFile");
  }
  file->fd = fd;
  file->fname = cldb_env_strdup(fname);
  if (file->fname == NULL) {
    (void)close(fd);
    free(file);
    return cldb_env_out_of_memory("NewSequentialFile");
  }
  file->base.vtbl = &kSequentialFileVtbl;
  *result = &file->base;
  return cldb_status_ok();
}

static cldb_status_t posix_new_random_access_file(
    cldb_env_t *env, const char *fname, cldb_random_access_file_t **result) {
  posix_random_access_file_t *file;
  int fd;

  CLDB_UNUSED(env);
  if (result == NULL) {
    return cldb_status_invalid_argument("NewRandomAccessFile",
                                        "result is NULL");
  }
  *result = NULL;
  if (fname == NULL) {
    return cldb_status_invalid_argument("NewRandomAccessFile",
                                        "fname is NULL");
  }
  fd = open(fname, O_RDONLY | kOpenBaseFlags);
  if (fd < 0) {
    return posix_error(fname, errno);
  }
  file = malloc(sizeof(*file));
  if (file == NULL) {
    (void)close(fd);
    return cldb_env_out_of_memory("NewRandomAccessFile");
  }
  file->fd = fd;
  file->fname = cldb_env_strdup(fname);
  if (file->fname == NULL) {
    (void)close(fd);
    free(file);
    return cldb_env_out_of_memory("NewRandomAccessFile");
  }
  file->base.vtbl = &kRandomAccessFileVtbl;
  *result = &file->base;
  return cldb_status_ok();
}

static cldb_status_t posix_new_writable_file(cldb_env_t *env,
                                             const char *fname,
                                             cldb_writable_file_t **result) {
  int fd;

  CLDB_UNUSED(env);
  if (result == NULL) {
    return cldb_status_invalid_argument("NewWritableFile",
                                        "result is NULL");
  }
  *result = NULL;
  if (fname == NULL) {
    return cldb_status_invalid_argument("NewWritableFile",
                                        "fname is NULL");
  }
  fd = open(fname, O_TRUNC | O_WRONLY | O_CREAT | kOpenBaseFlags, 0644);
  if (fd < 0) {
    return posix_error(fname, errno);
  }
  return make_writable_file(fname, fd, result);
}

static cldb_status_t posix_new_appendable_file(
    cldb_env_t *env, const char *fname, cldb_writable_file_t **result) {
  int fd;

  CLDB_UNUSED(env);
  if (result == NULL) {
    return cldb_status_invalid_argument("NewAppendableFile",
                                        "result is NULL");
  }
  *result = NULL;
  if (fname == NULL) {
    return cldb_status_invalid_argument("NewAppendableFile",
                                        "fname is NULL");
  }
  fd = open(fname, O_APPEND | O_WRONLY | O_CREAT | kOpenBaseFlags, 0644);
  if (fd < 0) {
    return posix_error(fname, errno);
  }
  return make_writable_file(fname, fd, result);
}

static bool posix_file_exists(cldb_env_t *env, const char *fname) {
  CLDB_UNUSED(env);
  return fname != NULL && access(fname, F_OK) == 0;
}

/* 对应上游 GetChildren；保留 "." 与 ".."，由上层 ParseFileName 过滤。 */
static cldb_status_t posix_get_children(cldb_env_t *env, const char *dirname,
                                        cldb_strvec_t *result) {
  DIR *dir;
  struct dirent *entry;

  CLDB_UNUSED(env);
  if (dirname == NULL || result == NULL) {
    return cldb_status_invalid_argument("GetChildren", "invalid argument");
  }
  dir = opendir(dirname);
  if (dir == NULL) {
    return posix_error(dirname, errno);
  }
  while ((entry = readdir(dir)) != NULL) {
    if (!cldb_strvec_push(result, entry->d_name)) {
      (void)closedir(dir);
      return cldb_env_out_of_memory("GetChildren");
    }
  }
  (void)closedir(dir);
  return cldb_status_ok();
}

static cldb_status_t posix_remove_file(cldb_env_t *env, const char *fname) {
  CLDB_UNUSED(env);
  if (fname == NULL) {
    return cldb_status_invalid_argument("RemoveFile", "invalid argument");
  }
  if (unlink(fname) != 0) {
    return posix_error(fname, errno);
  }
  return cldb_status_ok();
}

static cldb_status_t posix_create_dir(cldb_env_t *env, const char *dirname) {
  CLDB_UNUSED(env);
  if (dirname == NULL) {
    return cldb_status_invalid_argument("CreateDir", "invalid argument");
  }
  if (mkdir(dirname, 0755) != 0) {
    return posix_error(dirname, errno);
  }
  return cldb_status_ok();
}

static cldb_status_t posix_remove_dir(cldb_env_t *env, const char *dirname) {
  CLDB_UNUSED(env);
  if (dirname == NULL) {
    return cldb_status_invalid_argument("RemoveDir", "invalid argument");
  }
  if (rmdir(dirname) != 0) {
    return posix_error(dirname, errno);
  }
  return cldb_status_ok();
}

static cldb_status_t posix_get_file_size(cldb_env_t *env, const char *fname,
                                         uint64_t *size) {
  struct stat file_stat;

  CLDB_UNUSED(env);
  if (fname == NULL || size == NULL) {
    return cldb_status_invalid_argument("GetFileSize", "invalid argument");
  }
  *size = 0;
  if (stat(fname, &file_stat) != 0) {
    return posix_error(fname, errno);
  }
  *size = (uint64_t)file_stat.st_size;
  return cldb_status_ok();
}

static cldb_status_t posix_rename_file(cldb_env_t *env, const char *src,
                                       const char *target) {
  CLDB_UNUSED(env);
  if (src == NULL || target == NULL) {
    return cldb_status_invalid_argument("RenameFile", "invalid argument");
  }
  if (rename(src, target) != 0) {
    return posix_error(src, errno);
  }
  return cldb_status_ok();
}

static cldb_status_t posix_lock_file(cldb_env_t *env, const char *fname,
                                     cldb_file_lock_t **lock) {
  posix_env_t *self = (posix_env_t *)env;
  posix_file_lock_t *file_lock;
  lock_insert_result_t inserted;
  int fd;

  if (lock == NULL) {
    return cldb_status_invalid_argument("LockFile", "lock is NULL");
  }
  *lock = NULL;
  if (fname == NULL) {
    return cldb_status_invalid_argument("LockFile", "fname is NULL");
  }
  fd = open(fname, O_RDWR | O_CREAT | kOpenBaseFlags, 0644);
  if (fd < 0) {
    return posix_error(fname, errno);
  }
  inserted = lock_table_insert(&self->locks, fname);
  if (inserted == LOCK_INSERT_DUPLICATE) {
    (void)close(fd);
    return cldb_status_io("LockFile", "already held by process");
  }
  if (inserted == LOCK_INSERT_NO_MEMORY) {
    (void)close(fd);
    return cldb_env_out_of_memory("LockFile");
  }
  if (lock_or_unlock(fd, true) == -1) {
    int saved_errno = errno;

    lock_table_remove(&self->locks, fname);
    (void)close(fd);
    return posix_error(fname, saved_errno);
  }
  file_lock = malloc(sizeof(*file_lock));
  if (file_lock == NULL) {
    (void)lock_or_unlock(fd, false);
    lock_table_remove(&self->locks, fname);
    (void)close(fd);
    return cldb_env_out_of_memory("LockFile");
  }
  file_lock->fd = fd;
  file_lock->fname = cldb_env_strdup(fname);
  if (file_lock->fname == NULL) {
    (void)lock_or_unlock(fd, false);
    lock_table_remove(&self->locks, fname);
    (void)close(fd);
    free(file_lock);
    return cldb_env_out_of_memory("LockFile");
  }
  file_lock->base.state = file_lock;
  *lock = &file_lock->base;
  return cldb_status_ok();
}

static cldb_status_t posix_unlock_file(cldb_env_t *env,
                                       cldb_file_lock_t *lock) {
  posix_env_t *self = (posix_env_t *)env;
  posix_file_lock_t *file_lock;

  if (lock == NULL) {
    return cldb_status_invalid_argument("UnlockFile", "lock is NULL");
  }
  file_lock = (posix_file_lock_t *)lock;
  if (lock_or_unlock(file_lock->fd, false) == -1) {
    return posix_error(file_lock->fname, errno);
  }
  lock_table_remove(&self->locks, file_lock->fname);
  (void)close(file_lock->fd);
  free(file_lock->fname);
  free(file_lock);
  return cldb_status_ok();
}

static void posix_schedule(cldb_env_t *env, void (*fn)(void *), void *arg) {
  posix_env_t *self = (posix_env_t *)env;
  background_work_t *work;
  bool start_failed = false;

  if (fn == NULL) {
    return;
  }
  work = malloc(sizeof(*work));
  if (work == NULL) {
    fn(arg);
    return;
  }
  work->fn = fn;
  work->arg = arg;
  work->next = NULL;

  background_lock(self);
  if (!self->background_started) {
    if (start_detached_thread(background_thread_main, self)) {
      self->background_started = true;
    } else {
      start_failed = true;
    }
  }
  if (!start_failed) {
    if (self->background_tail == NULL) {
      self->background_head = work;
    } else {
      self->background_tail->next = work;
    }
    self->background_tail = work;
    background_signal(self);
  }
  background_unlock(self);
  if (start_failed) {
    free(work);
    fn(arg);
  }
}

static void posix_start_thread(cldb_env_t *env, void (*fn)(void *), void *arg) {
  start_thread_context_t *context;

  CLDB_UNUSED(env);
  if (fn == NULL) {
    return;
  }
  context = malloc(sizeof(*context));
  if (context == NULL) {
    fn(arg);
    return;
  }
  context->fn = fn;
  context->arg = arg;
  if (!start_detached_thread(start_thread_trampoline, context)) {
    free(context);
    fn(arg);
  }
}

static cldb_status_t posix_get_test_directory(cldb_env_t *env,
                                              cldb_buf_t *path) {
  const char *test_tmpdir;
  char default_path[100];

  CLDB_UNUSED(env);
  if (path == NULL) {
    return cldb_status_invalid_argument("GetTestDirectory", "path is NULL");
  }
  test_tmpdir = getenv("TEST_TMPDIR");
  if (test_tmpdir != NULL && test_tmpdir[0] != '\0') {
    if (!cldb_buf_assign(path, test_tmpdir, strlen(test_tmpdir))) {
      return cldb_env_out_of_memory("GetTestDirectory");
    }
  } else {
    (void)snprintf(default_path, sizeof(default_path),
                   "/tmp/leveldbtest-%lu", (unsigned long)geteuid());
    if (!cldb_buf_assign(path, default_path, strlen(default_path))) {
      return cldb_env_out_of_memory("GetTestDirectory");
    }
  }
  /* 目录可能已存在；对应上游忽略 CreateDir 的结果。 */
  (void)mkdir(cldb_buf_cstr(path), 0755);
  return cldb_status_ok();
}

static cldb_status_t posix_new_logger(cldb_env_t *env, const char *fname,
                                      cldb_logger_t **result) {
  posix_logger_t *logger;
  FILE *fp;
  int fd;

  CLDB_UNUSED(env);
  if (result == NULL) {
    return cldb_status_invalid_argument("NewLogger", "result is NULL");
  }
  *result = NULL;
  if (fname == NULL) {
    return cldb_status_invalid_argument("NewLogger", "fname is NULL");
  }
  fd = open(fname, O_APPEND | O_WRONLY | O_CREAT | kOpenBaseFlags, 0644);
  if (fd < 0) {
    return posix_error(fname, errno);
  }
  fp = fdopen(fd, "w");
  if (fp == NULL) {
    int saved_errno = errno;

    (void)close(fd);
    return posix_error(fname, saved_errno);
  }
  logger = malloc(sizeof(*logger));
  if (logger == NULL) {
    (void)fclose(fp);
    return cldb_env_out_of_memory("NewLogger");
  }
  logger->fp = fp;
  logger->base.vtbl = &kLoggerVtbl;
  *result = &logger->base;
  return cldb_status_ok();
}

static uint64_t posix_now_micros(cldb_env_t *env) {
  struct timeval now;

  CLDB_UNUSED(env);
  (void)gettimeofday(&now, NULL);
  return (uint64_t)now.tv_sec * UINT64_C(1000000) +
         (uint64_t)now.tv_usec;
}

static void posix_sleep_for_microseconds(cldb_env_t *env, int micros) {
  CLDB_UNUSED(env);
  if (micros > 0) {
    (void)cldb_sleep_for_micros((uint64_t)micros);
  }
}

static const cldb_env_vtbl_t kPosixEnvVtbl = {
    .new_sequential_file = posix_new_sequential_file,
    .new_random_access_file = posix_new_random_access_file,
    .new_writable_file = posix_new_writable_file,
    .new_appendable_file = posix_new_appendable_file,
    .file_exists = posix_file_exists,
    .get_children = posix_get_children,
    .remove_file = posix_remove_file,
    .delete_file = NULL,
    .create_dir = posix_create_dir,
    .remove_dir = posix_remove_dir,
    .delete_dir = NULL,
    .get_file_size = posix_get_file_size,
    .rename_file = posix_rename_file,
    .lock_file = posix_lock_file,
    .unlock_file = posix_unlock_file,
    .schedule = posix_schedule,
    .start_thread = posix_start_thread,
    .get_test_directory = posix_get_test_directory,
    .new_logger = posix_new_logger,
    .now_micros = posix_now_micros,
    .sleep_for_microseconds = posix_sleep_for_microseconds,
    .destroy = NULL,
};

/* ---------- 默认 Env 单例 ---------- */

static posix_env_t g_default_env;
static pthread_once_t g_default_env_once = PTHREAD_ONCE_INIT;

static void abort_init(const char *what, cldb_status_t status) {
  (void)fprintf(stderr, "cleveldb: default Env %s init failed: %s\n", what,
                cldb_status_message(&status));
  abort();
}

static void default_env_init(void) {
  cldb_status_t status;

  g_default_env.base.vtbl = &kPosixEnvVtbl;
  g_default_env.background_started = false;
  g_default_env.background_head = NULL;
  g_default_env.background_tail = NULL;

  status = cldb_mutex_init(&g_default_env.background_mutex);
  if (!cldb_status_is_ok(status)) {
    abort_init("background mutex", status);
  }
  status = cldb_condvar_init(&g_default_env.background_cv,
                             &g_default_env.background_mutex);
  if (!cldb_status_is_ok(status)) {
    abort_init("background condvar", status);
  }
  status = cldb_mutex_init(&g_default_env.locks.mutex);
  if (!cldb_status_is_ok(status)) {
    abort_init("lock table mutex", status);
  }
  g_default_env.locks.names = NULL;
  g_default_env.locks.size = 0;
  g_default_env.locks.capacity = 0;
}

cldb_env_t *cldb_env_default(void) {
  int error = pthread_once(&g_default_env_once, default_env_init);

  if (error != 0) {
    (void)fprintf(stderr, "cleveldb: pthread_once failed: %s\n",
                  strerror(error));
    abort();
  }
  return &g_default_env.base;
}
