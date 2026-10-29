#include "helpers/memenv/memenv.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "port/port.h"
#include "port/port_atomic.h"
#include "port/port_mutex.h"
#include "util/env_internal.h"

/* 全内存 Env；对应上游 helpers/memenv/memenv.cc。文件表按路径排序，
 * 并发访问由 mem_env_t::mutex 保护；文件内容另由每个 FileState 的
 * mutex 保护，使打开的文件对象可在 Env 锁之外读写。 */

/* 共享文件状态；对应上游 FileState。refs 初始为 1，由文件表持有；
 * 打开的文件对象各自再加 1，最后一个 unref 释放内容。
 * 引用规则：只有已持有引用，或在持 env->mutex 时从文件表取得表项，
 * 才能增加引用；因此 refs 不会从 0 复活，最后一个 unref 时不会还有
 * 使用者。state->mutex 只保护 buf 内容，不负责对象生命周期。 */
typedef struct mem_env_file_state {
  cldb_atomic_u32_t refs; /* 原子引用计数；初值 1 */
  cldb_mutex_t mutex;     /* 保护 buf */
  cldb_buf_t buf;         /* 由本状态释放；文件内容 */
} mem_env_file_state_t;

typedef struct mem_env_file_entry {
  char *path;                     /* 由文件表释放；排序键 */
  mem_env_file_state_t *state;    /* 文件表持有引用，移除时 unref */
} mem_env_file_entry_t;

typedef struct mem_env_file_lock {
  cldb_file_lock_t base; /* 必须为首成员 */
  char *fname;           /* 由 UnlockFile 释放 */
} mem_env_file_lock_t;

/* 内存 Env；对应上游 InMemoryEnv（基于 EnvWrapper）。base 用于转发
 * 调度、线程与时间。 */
typedef struct mem_env {
  cldb_env_wrapper_t wrapper; /* 必须为首成员 */
  cldb_mutex_t mutex;         /* 保护 files 与 locked_paths */
  mem_env_file_entry_t *files; /* 由本对象释放；按 path 升序 */
  size_t file_count;
  size_t file_capacity;
  char **locked_paths;         /* 由本对象释放；每项为已锁路径副本 */
  size_t lock_count;
  size_t lock_capacity;
  cldb_memenv_log_target_t log_target; /* 创建时固定 */
} mem_env_t;

/* 内部同步原语误用只可能是生命周期错误；release 下调用也不能省。 */
static void env_lock(mem_env_t *env) {
  cldb_status_t status = cldb_mutex_lock(&env->mutex);

  CLDB_UNUSED(status);
  CLDB_ASSERT(cldb_status_is_ok(status));
}

static void env_unlock(mem_env_t *env) {
  cldb_status_t status = cldb_mutex_unlock(&env->mutex);

  CLDB_UNUSED(status);
  CLDB_ASSERT(cldb_status_is_ok(status));
}

static void state_lock(mem_env_file_state_t *state) {
  cldb_status_t status = cldb_mutex_lock(&state->mutex);

  CLDB_UNUSED(status);
  CLDB_ASSERT(cldb_status_is_ok(status));
}

static void state_unlock(mem_env_file_state_t *state) {
  cldb_status_t status = cldb_mutex_unlock(&state->mutex);

  CLDB_UNUSED(status);
  CLDB_ASSERT(cldb_status_is_ok(status));
}

/* ---------- 文件状态 ---------- */

/* 新建状态并返回表引用；对应上游 FileState 构造（初值 1）。 */
static mem_env_file_state_t *file_state_create(void) {
  mem_env_file_state_t *state = calloc(1, sizeof(*state));

  if (state == NULL) {
    return NULL;
  }
  cldb_atomic_u32_init(&state->refs, 1);
  if (!cldb_status_is_ok(cldb_mutex_init(&state->mutex))) {
    free(state);
    return NULL;
  }
  cldb_buf_init(&state->buf);
  return state;
}

/* 增加引用；调用者必须已持有引用，或持有 env->mutex 且 state 仍在文件表
 * 中。relaxed 足够：发布路径由 env 锁或对象自身的生命周期同步。 */
static void file_state_ref(mem_env_file_state_t *state) {
  uint32_t old =
      cldb_atomic_u32_fetch_add(&state->refs, 1, memory_order_relaxed);

  CLDB_UNUSED(old);
  CLDB_ASSERT(old > 0);
}

/* 减少引用；调用者必须持有一个引用（表项引用或对象引用）。old == 1 时
 * 释放 buffer、mutex 和 state。acq_rel 保证释放前看到其他引用者的写入；
 * 多个线程并发 unref 时只有一个执行释放。 */
static void file_state_unref(mem_env_file_state_t *state) {
  uint32_t old =
      atomic_fetch_sub_explicit(&state->refs, 1, memory_order_acq_rel);

  if (old == 1) {
    cldb_buf_free(&state->buf);
    (void)cldb_mutex_destroy(&state->mutex);
    free(state);
    return;
  }
  CLDB_ASSERT(old > 0);
}

/* 追加数据；对应上游 FileState::Append。 */
static cldb_status_t file_state_append(mem_env_file_state_t *state,
                                       const char *data, size_t size) {
  bool appended;

  if (size == 0) {
    return cldb_status_ok();
  }
  state_lock(state);
  appended = cldb_buf_append(&state->buf, data, size);
  state_unlock(state);
  if (!appended) {
    return cldb_env_out_of_memory("Append");
  }
  return cldb_status_ok();
}

/* 读取 [offset, offset+n)；对应上游 FileState::Read。offset 超过文件尾
 * 返回 IOError（上游 memenv 语义，与 POSIX pread 返回空 Slice 不同）。 */
static cldb_status_t file_state_read(mem_env_file_state_t *state,
                                     uint64_t offset, size_t n,
                                     cldb_slice_t *result, char *scratch) {
  size_t available;
  size_t copy_size;

  if (n > 0 && scratch == NULL) {
    *result = cldb_slice_empty();
    return cldb_status_invalid_argument("Read", "scratch is NULL");
  }
  state_lock(state);
  if (offset > (uint64_t)state->buf.size) {
    state_unlock(state);
    *result = cldb_slice_empty();
    return cldb_status_io("Read", "Offset greater than file size.");
  }
  available = state->buf.size - (size_t)offset;
  copy_size = n < available ? n : available;
  if (copy_size == 0) {
    *result = cldb_slice_empty();
  } else {
    memcpy(scratch, state->buf.data + (size_t)offset, copy_size);
    *result = cldb_slice_make(scratch, copy_size);
  }
  state_unlock(state);
  return cldb_status_ok();
}

/* 清空内容；对应上游 FileState::Truncate。 */
static void file_state_truncate(mem_env_file_state_t *state) {
  state_lock(state);
  cldb_buf_clear(&state->buf);
  state_unlock(state);
}

static uint64_t file_state_size(mem_env_file_state_t *state) {
  uint64_t size;

  state_lock(state);
  size = (uint64_t)state->buf.size;
  state_unlock(state);
  return size;
}

/* ---------- 文件表 ---------- */

/* 二分查找路径；返回插入位置，found 表示已有同名项。调用者持 env 锁。 */
static size_t file_lower_bound(const mem_env_t *env, const char *path,
                               bool *found) {
  size_t low = 0;
  size_t high = env->file_count;

  while (low < high) {
    size_t middle = low + (high - low) / 2;
    int compare = strcmp(env->files[middle].path, path);

    if (compare < 0) {
      low = middle + 1;
    } else {
      high = middle;
    }
  }
  *found = low < env->file_count &&
           strcmp(env->files[low].path, path) == 0;
  return low;
}

/* 查找并取得调用者引用；不存在返回 NULL。调用者持 env 锁。
 * 表项引用保证 state 在 file_state_ref 前不会被释放。 */
static mem_env_file_state_t *lookup_file_locked(mem_env_t *env,
                                                const char *path) {
  bool found;
  size_t index = file_lower_bound(env, path, &found);

  if (!found) {
    return NULL;
  }
  file_state_ref(env->files[index].state);
  return env->files[index].state;
}

static bool ensure_file_capacity(mem_env_t *env) {
  size_t new_capacity;
  mem_env_file_entry_t *new_files;

  if (env->file_count < env->file_capacity) {
    return true;
  }
  if (env->file_capacity == 0) {
    new_capacity = 8;
  } else {
    if (env->file_capacity > SIZE_MAX / 2) {
      return false;
    }
    new_capacity = env->file_capacity * 2;
  }
  if (new_capacity > SIZE_MAX / sizeof(*new_files)) {
    return false;
  }
  new_files = realloc(env->files, new_capacity * sizeof(*new_files));
  if (new_files == NULL) {
    return false;
  }
  env->files = new_files;
  env->file_capacity = new_capacity;
  return true;
}

/* 在 index 处插入；调用者已保证容量，插入后由文件表负责释放。 */
static void insert_file_locked(mem_env_t *env, size_t index, char *path,
                               mem_env_file_state_t *state) {
  size_t tail = env->file_count - index;

  if (tail > 0) {
    memmove(&env->files[index + 1], &env->files[index],
            tail * sizeof(env->files[0]));
  }
  env->files[index].path = path;
  env->files[index].state = state;
  env->file_count++;
}

/* 移除表项并释放路径；不处理 state 引用，由调用者决定。 */
static void remove_file_locked(mem_env_t *env, size_t index) {
  size_t tail = env->file_count - index - 1;

  free(env->files[index].path);
  if (tail > 0) {
    memmove(&env->files[index], &env->files[index + 1],
            tail * sizeof(env->files[0]));
  }
  env->file_count--;
}

/* 新建空文件并返回表引用（不额外加调用者引用）。调用者持 env 锁。 */
static mem_env_file_state_t *create_file_locked(mem_env_t *env,
                                                const char *path) {
  mem_env_file_state_t *state;
  char *path_copy;
  bool found;
  size_t index;

  state = file_state_create();
  if (state == NULL) {
    return NULL;
  }
  path_copy = cldb_env_strdup(path);
  if (path_copy == NULL) {
    file_state_unref(state);
    return NULL;
  }
  if (!ensure_file_capacity(env)) {
    free(path_copy);
    file_state_unref(state);
    return NULL;
  }
  index = file_lower_bound(env, path, &found);
  CLDB_ASSERT(!found);
  insert_file_locked(env, index, path_copy, state);
  return state;
}

/* 打开文件状态；create 时不存在则新建，truncate 时清空内容。成功返回
 * 调用者引用，失败返回 NULL。 */
static mem_env_file_state_t *open_file_state(mem_env_t *env, const char *path,
                                             bool create, bool truncate) {
  mem_env_file_state_t *state;

  env_lock(env);
  state = lookup_file_locked(env, path);
  if (state == NULL && create) {
    state = create_file_locked(env, path);
    if (state != NULL) {
      file_state_ref(state);
    }
  }
  if (state != NULL && truncate) {
    file_state_truncate(state);
  }
  env_unlock(env);
  return state;
}

/* ---------- 顺序读文件 ---------- */

typedef struct mem_env_sequential_file {
  cldb_sequential_file_t base; /* 必须为首成员 */
  mem_env_file_state_t *state; /* destroy 时 unref */
  uint64_t pos;                /* 当前读取位置 */
} mem_env_sequential_file_t;

static cldb_status_t sequential_file_read(cldb_sequential_file_t *file,
                                          size_t n, cldb_slice_t *result,
                                          char *scratch) {
  mem_env_sequential_file_t *self = (mem_env_sequential_file_t *)file;
  cldb_status_t status =
      file_state_read(self->state, self->pos, n, result, scratch);

  if (cldb_status_is_ok(status)) {
    self->pos += (uint64_t)result->size;
  }
  return status;
}

/* 对应上游 SequentialFileImpl::Skip；先锁状态再检查，避免与写入竞态。 */
static cldb_status_t sequential_file_skip(cldb_sequential_file_t *file,
                                          uint64_t n) {
  mem_env_sequential_file_t *self = (mem_env_sequential_file_t *)file;
  uint64_t available;

  state_lock(self->state);
  if (self->pos > (uint64_t)self->state->buf.size) {
    state_unlock(self->state);
    return cldb_status_io("Skip", "pos > file size");
  }
  available = (uint64_t)self->state->buf.size - self->pos;
  if (n > available) {
    n = available;
  }
  self->pos += n;
  state_unlock(self->state);
  return cldb_status_ok();
}

static void sequential_file_destroy(cldb_sequential_file_t *file) {
  mem_env_sequential_file_t *self = (mem_env_sequential_file_t *)file;

  file_state_unref(self->state);
  free(self);
}

static const cldb_sequential_file_vtbl_t kSequentialFileVtbl = {
    .read = sequential_file_read,
    .skip = sequential_file_skip,
    .destroy = sequential_file_destroy,
};

/* ---------- 随机读文件 ---------- */

typedef struct mem_env_random_access_file {
  cldb_random_access_file_t base; /* 必须为首成员 */
  mem_env_file_state_t *state;    /* destroy 时 unref */
} mem_env_random_access_file_t;

static cldb_status_t random_access_file_read(cldb_random_access_file_t *file,
                                             uint64_t offset, size_t n,
                                             cldb_slice_t *result,
                                             char *scratch) {
  mem_env_random_access_file_t *self =
      (mem_env_random_access_file_t *)file;

  return file_state_read(self->state, offset, n, result, scratch);
}

static void random_access_file_destroy(cldb_random_access_file_t *file) {
  mem_env_random_access_file_t *self =
      (mem_env_random_access_file_t *)file;

  file_state_unref(self->state);
  free(self);
}

static const cldb_random_access_file_vtbl_t kRandomAccessFileVtbl = {
    .read = random_access_file_read,
    .destroy = random_access_file_destroy,
};

/* ---------- 可写 / 追加文件 ---------- */

typedef struct mem_env_writable_file {
  cldb_writable_file_t base;   /* 必须为首成员 */
  mem_env_file_state_t *state; /* destroy 时 unref */
} mem_env_writable_file_t;

static cldb_status_t writable_file_append(cldb_writable_file_t *file,
                                          cldb_slice_t data) {
  mem_env_writable_file_t *self = (mem_env_writable_file_t *)file;

  if (data.size > 0 && data.data == NULL) {
    return cldb_status_invalid_argument("Append", "data is NULL");
  }
  return file_state_append(self->state, data.data, data.size);
}

static cldb_status_t writable_file_close(cldb_writable_file_t *file) {
  CLDB_UNUSED(file);
  return cldb_status_ok();
}

static cldb_status_t writable_file_flush(cldb_writable_file_t *file) {
  CLDB_UNUSED(file);
  return cldb_status_ok();
}

static cldb_status_t writable_file_sync(cldb_writable_file_t *file) {
  CLDB_UNUSED(file);
  return cldb_status_ok();
}

static void writable_file_destroy(cldb_writable_file_t *file) {
  mem_env_writable_file_t *self = (mem_env_writable_file_t *)file;

  file_state_unref(self->state);
  free(self);
}

static const cldb_writable_file_vtbl_t kWritableFileVtbl = {
    .append = writable_file_append,
    .close = writable_file_close,
    .flush = writable_file_flush,
    .sync = writable_file_sync,
    .destroy = writable_file_destroy,
};

/* ---------- 日志 ---------- */

typedef struct mem_env_logger {
  cldb_logger_t base;          /* 必须为首成员 */
  mem_env_file_state_t *state; /* destroy 时 unref；NULL 表示 stderr */
} mem_env_logger_t;

static void logger_write(mem_env_logger_t *logger, const char *line,
                         size_t size) {
  if (logger->state != NULL) {
    (void)file_state_append(logger->state, line, size);
    return;
  }
  flockfile(stderr);
  (void)fwrite(line, 1, size, stderr);
  (void)fflush(stderr);
  funlockfile(stderr);
}

/* 时间戳格式与 PosixLogger 一致；内存模式追加到 NewLogger 的文件。 */
static void logger_logv(cldb_logger_t *logger, const char *format,
                        va_list ap) {
  enum { kHeaderSize = 64 };
  mem_env_logger_t *self = (mem_env_logger_t *)logger;
  struct timeval now;
  struct tm components;
  time_t seconds;
  char header[kHeaderSize];
  char *line;
  va_list copy;
  int header_length;
  int message_length;
  size_t total;

  if (format == NULL) {
    return;
  }
  (void)gettimeofday(&now, NULL);
  seconds = now.tv_sec;
  if (localtime_r(&seconds, &components) == NULL) {
    memset(&components, 0, sizeof(components));
  }
  header_length = snprintf(
      header, sizeof(header), "%04d/%02d/%02d-%02d:%02d:%02d.%06d %lu ",
      components.tm_year + 1900, components.tm_mon + 1, components.tm_mday,
      components.tm_hour, components.tm_min, components.tm_sec,
      (int)now.tv_usec, (unsigned long)pthread_self());
  if (header_length < 0 || (size_t)header_length >= sizeof(header)) {
    return;
  }
  va_copy(copy, ap);
  message_length = vsnprintf(NULL, 0, format, copy);
  va_end(copy);
  if (message_length < 0 ||
      (size_t)message_length > SIZE_MAX - (size_t)header_length - 2) {
    return;
  }
  total = (size_t)header_length + (size_t)message_length;
  line = malloc(total + 2);
  if (line == NULL) {
    return;
  }
  memcpy(line, header, (size_t)header_length);
  va_copy(copy, ap);
  (void)vsnprintf(line + (size_t)header_length, (size_t)message_length + 1,
                  format, copy);
  va_end(copy);
  if (line[total - 1] != '\n') {
    line[total] = '\n';
    total++;
  }
  logger_write(self, line, total);
  free(line);
}

static void logger_destroy(cldb_logger_t *logger) {
  mem_env_logger_t *self = (mem_env_logger_t *)logger;

  if (self->state != NULL) {
    file_state_unref(self->state);
  }
  free(self);
}

static const cldb_logger_vtbl_t kLoggerVtbl = {
    .logv = logger_logv,
    .destroy = logger_destroy,
};

/* ---------- Env 方法 ---------- */

static cldb_status_t mem_env_new_sequential_file(
    cldb_env_t *env, const char *fname, cldb_sequential_file_t **result) {
  mem_env_t *self = (mem_env_t *)env;
  mem_env_file_state_t *state;
  mem_env_sequential_file_t *file;

  if (result == NULL) {
    return cldb_status_invalid_argument("NewSequentialFile",
                                        "result is NULL");
  }
  *result = NULL;
  if (fname == NULL) {
    return cldb_status_invalid_argument("NewSequentialFile",
                                        "fname is NULL");
  }
  state = open_file_state(self, fname, false, false);
  if (state == NULL) {
    return cldb_status_not_found("NewSequentialFile", fname);
  }
  file = calloc(1, sizeof(*file));
  if (file == NULL) {
    file_state_unref(state);
    return cldb_env_out_of_memory("NewSequentialFile");
  }
  file->base.vtbl = &kSequentialFileVtbl;
  file->state = state;
  *result = &file->base;
  return cldb_status_ok();
}

static cldb_status_t mem_env_new_random_access_file(
    cldb_env_t *env, const char *fname, cldb_random_access_file_t **result) {
  mem_env_t *self = (mem_env_t *)env;
  mem_env_file_state_t *state;
  mem_env_random_access_file_t *file;

  if (result == NULL) {
    return cldb_status_invalid_argument("NewRandomAccessFile",
                                        "result is NULL");
  }
  *result = NULL;
  if (fname == NULL) {
    return cldb_status_invalid_argument("NewRandomAccessFile",
                                        "fname is NULL");
  }
  state = open_file_state(self, fname, false, false);
  if (state == NULL) {
    return cldb_status_not_found("NewRandomAccessFile", fname);
  }
  file = calloc(1, sizeof(*file));
  if (file == NULL) {
    file_state_unref(state);
    return cldb_env_out_of_memory("NewRandomAccessFile");
  }
  file->base.vtbl = &kRandomAccessFileVtbl;
  file->state = state;
  *result = &file->base;
  return cldb_status_ok();
}

static cldb_status_t mem_env_new_writable_file(
    cldb_env_t *env, const char *fname, cldb_writable_file_t **result) {
  mem_env_t *self = (mem_env_t *)env;
  mem_env_file_state_t *state;
  mem_env_writable_file_t *file;

  if (result == NULL) {
    return cldb_status_invalid_argument("NewWritableFile",
                                        "result is NULL");
  }
  *result = NULL;
  if (fname == NULL) {
    return cldb_status_invalid_argument("NewWritableFile", "fname is NULL");
  }
  state = open_file_state(self, fname, true, true);
  if (state == NULL) {
    return cldb_env_out_of_memory("NewWritableFile");
  }
  file = calloc(1, sizeof(*file));
  if (file == NULL) {
    file_state_unref(state);
    return cldb_env_out_of_memory("NewWritableFile");
  }
  file->base.vtbl = &kWritableFileVtbl;
  file->state = state;
  *result = &file->base;
  return cldb_status_ok();
}

static cldb_status_t mem_env_new_appendable_file(
    cldb_env_t *env, const char *fname, cldb_writable_file_t **result) {
  mem_env_t *self = (mem_env_t *)env;
  mem_env_file_state_t *state;
  mem_env_writable_file_t *file;

  if (result == NULL) {
    return cldb_status_invalid_argument("NewAppendableFile",
                                        "result is NULL");
  }
  *result = NULL;
  if (fname == NULL) {
    return cldb_status_invalid_argument("NewAppendableFile",
                                        "fname is NULL");
  }
  state = open_file_state(self, fname, true, false);
  if (state == NULL) {
    return cldb_env_out_of_memory("NewAppendableFile");
  }
  file = calloc(1, sizeof(*file));
  if (file == NULL) {
    file_state_unref(state);
    return cldb_env_out_of_memory("NewAppendableFile");
  }
  file->base.vtbl = &kWritableFileVtbl;
  file->state = state;
  *result = &file->base;
  return cldb_status_ok();
}

static bool mem_env_file_exists(cldb_env_t *env, const char *fname) {
  mem_env_t *self = (mem_env_t *)env;
  bool found;
  size_t index;

  if (fname == NULL) {
    return false;
  }
  env_lock(self);
  index = file_lower_bound(self, fname, &found);
  CLDB_UNUSED(index);
  env_unlock(self);
  return found;
}

/* 对应上游 InMemoryEnv::GetChildren：返回 dir/ 前缀条目的相对路径；
 * 目录不是独立表项，因此后缀可能含 '/'，且没有 "."/".."。 */
static cldb_status_t mem_env_get_children(cldb_env_t *env, const char *dir,
                                          cldb_strvec_t *result) {
  mem_env_t *self = (mem_env_t *)env;
  size_t dir_length;
  size_t i;

  if (dir == NULL || result == NULL) {
    return cldb_status_invalid_argument("GetChildren", "invalid argument");
  }
  dir_length = strlen(dir);
  env_lock(self);
  for (i = 0; i < self->file_count; ++i) {
    const char *path = self->files[i].path;
    size_t path_length = strlen(path);

    if (path_length <= dir_length ||
        path[dir_length] != '/' ||
        strncmp(path, dir, dir_length) != 0) {
      continue;
    }
    if (!cldb_strvec_push(result, path + dir_length + 1)) {
      env_unlock(self);
      return cldb_env_out_of_memory("GetChildren");
    }
  }
  env_unlock(self);
  return cldb_status_ok();
}

static cldb_status_t mem_env_remove_file(cldb_env_t *env,
                                         const char *fname) {
  mem_env_t *self = (mem_env_t *)env;
  mem_env_file_state_t *state;
  bool found;
  size_t index;

  if (fname == NULL) {
    return cldb_status_invalid_argument("RemoveFile", "fname is NULL");
  }
  env_lock(self);
  index = file_lower_bound(self, fname, &found);
  if (!found) {
    env_unlock(self);
    return cldb_status_not_found("RemoveFile", fname);
  }
  state = self->files[index].state;
  remove_file_locked(self, index);
  env_unlock(self);
  /* 表项已不可查，新查找无法再取得该 state；已有打开对象各持引用。 */
  file_state_unref(state);
  return cldb_status_ok();
}

/* 目录没有独立表项；对应上游 no-op CreateDir。 */
static cldb_status_t mem_env_create_dir(cldb_env_t *env,
                                        const char *dirname) {
  CLDB_UNUSED(env);
  if (dirname == NULL) {
    return cldb_status_invalid_argument("CreateDir", "dirname is NULL");
  }
  return cldb_status_ok();
}

/* 目录没有独立表项；对应上游 no-op RemoveDir。 */
static cldb_status_t mem_env_remove_dir(cldb_env_t *env,
                                        const char *dirname) {
  CLDB_UNUSED(env);
  if (dirname == NULL) {
    return cldb_status_invalid_argument("RemoveDir", "dirname is NULL");
  }
  return cldb_status_ok();
}

static cldb_status_t mem_env_get_file_size(cldb_env_t *env,
                                           const char *fname,
                                           uint64_t *size) {
  mem_env_t *self = (mem_env_t *)env;
  mem_env_file_state_t *state;

  if (fname == NULL || size == NULL) {
    return cldb_status_invalid_argument("GetFileSize", "invalid argument");
  }
  *size = 0;
  env_lock(self);
  state = lookup_file_locked(self, fname);
  env_unlock(self);
  if (state == NULL) {
    return cldb_status_not_found("GetFileSize", fname);
  }
  *size = file_state_size(state);
  file_state_unref(state);
  return cldb_status_ok();
}

/* 两处查找与移动都在同一把锁内完成；target 已存在时旧状态延后 unref，
 * 已打开的文件对象仍可读到旧内容（对应上游行为）。 */
static cldb_status_t mem_env_rename_file(cldb_env_t *env, const char *src,
                                         const char *target) {
  mem_env_t *self = (mem_env_t *)env;
  mem_env_file_state_t *state;
  mem_env_file_state_t *replaced = NULL;
  char *target_copy;
  bool src_found;
  bool target_found;
  bool unused_found;
  size_t src_index;
  size_t target_index;
  size_t insert_index;

  if (src == NULL || target == NULL) {
    return cldb_status_invalid_argument("RenameFile", "invalid argument");
  }
  if (strcmp(src, target) == 0) {
    env_lock(self);
    (void)file_lower_bound(self, src, &src_found);
    env_unlock(self);
    return src_found ? cldb_status_ok()
                     : cldb_status_not_found("RenameFile", src);
  }
  target_copy = cldb_env_strdup(target);
  if (target_copy == NULL) {
    return cldb_env_out_of_memory("RenameFile");
  }
  env_lock(self);
  src_index = file_lower_bound(self, src, &src_found);
  if (!src_found) {
    env_unlock(self);
    free(target_copy);
    return cldb_status_not_found("RenameFile", src);
  }
  target_index = file_lower_bound(self, target, &target_found);
  state = self->files[src_index].state;
  if (target_found) {
    replaced = self->files[target_index].state;
    remove_file_locked(self, target_index);
    if (target_index < src_index) {
      src_index--;
    }
  }
  remove_file_locked(self, src_index);
  insert_index = file_lower_bound(self, target_copy, &unused_found);
  CLDB_ASSERT(!unused_found);
  insert_file_locked(self, insert_index, target_copy, state);
  env_unlock(self);
  /* replaced 已从表中移除；并发查找要么在移除前已加引用，要么看不到它。 */
  if (replaced != NULL) {
    file_state_unref(replaced);
  }
  return cldb_status_ok();
}

static bool ensure_lock_capacity(mem_env_t *env) {
  size_t new_capacity;
  char **new_paths;

  if (env->lock_count < env->lock_capacity) {
    return true;
  }
  if (env->lock_capacity == 0) {
    new_capacity = 8;
  } else {
    if (env->lock_capacity > SIZE_MAX / 2) {
      return false;
    }
    new_capacity = env->lock_capacity * 2;
  }
  if (new_capacity > SIZE_MAX / sizeof(*new_paths)) {
    return false;
  }
  new_paths = realloc(env->locked_paths, new_capacity * sizeof(*new_paths));
  if (new_paths == NULL) {
    return false;
  }
  env->locked_paths = new_paths;
  env->lock_capacity = new_capacity;
  return true;
}

static cldb_status_t mem_env_lock_file(cldb_env_t *env, const char *fname,
                                       cldb_file_lock_t **lock) {
  mem_env_t *self = (mem_env_t *)env;
  mem_env_file_lock_t *file_lock;
  char *table_copy;
  size_t i;

  if (lock == NULL) {
    return cldb_status_invalid_argument("LockFile", "lock is NULL");
  }
  *lock = NULL;
  if (fname == NULL) {
    return cldb_status_invalid_argument("LockFile", "fname is NULL");
  }
  file_lock = calloc(1, sizeof(*file_lock));
  if (file_lock == NULL) {
    return cldb_env_out_of_memory("LockFile");
  }
  file_lock->fname = cldb_env_strdup(fname);
  if (file_lock->fname == NULL) {
    free(file_lock);
    return cldb_env_out_of_memory("LockFile");
  }
  env_lock(self);
  for (i = 0; i < self->lock_count; ++i) {
    if (strcmp(self->locked_paths[i], fname) == 0) {
      env_unlock(self);
      free(file_lock->fname);
      free(file_lock);
      return cldb_status_io("LockFile", "already held by process");
    }
  }
  if (!ensure_lock_capacity(self)) {
    env_unlock(self);
    free(file_lock->fname);
    free(file_lock);
    return cldb_env_out_of_memory("LockFile");
  }
  table_copy = cldb_env_strdup(fname);
  if (table_copy == NULL) {
    env_unlock(self);
    free(file_lock->fname);
    free(file_lock);
    return cldb_env_out_of_memory("LockFile");
  }
  self->locked_paths[self->lock_count] = table_copy;
  self->lock_count++;
  env_unlock(self);
  file_lock->base.state = file_lock;
  *lock = &file_lock->base;
  return cldb_status_ok();
}

static cldb_status_t mem_env_unlock_file(cldb_env_t *env,
                                         cldb_file_lock_t *lock) {
  mem_env_t *self = (mem_env_t *)env;
  mem_env_file_lock_t *file_lock;
  size_t i;

  if (lock == NULL) {
    return cldb_status_invalid_argument("UnlockFile", "lock is NULL");
  }
  file_lock = (mem_env_file_lock_t *)lock;
  env_lock(self);
  for (i = 0; i < self->lock_count; ++i) {
    if (strcmp(self->locked_paths[i], file_lock->fname) == 0) {
      free(self->locked_paths[i]);
      self->locked_paths[i] = self->locked_paths[self->lock_count - 1];
      self->lock_count--;
      break;
    }
  }
  env_unlock(self);
  free(file_lock->fname);
  free(file_lock);
  return cldb_status_ok();
}

static void mem_env_schedule(cldb_env_t *env, void (*fn)(void *), void *arg) {
  mem_env_t *self = (mem_env_t *)env;

  cldb_env_schedule(self->wrapper.target, fn, arg);
}

static void mem_env_start_thread(cldb_env_t *env, void (*fn)(void *),
                                 void *arg) {
  mem_env_t *self = (mem_env_t *)env;

  cldb_env_start_thread(self->wrapper.target, fn, arg);
}

static cldb_status_t mem_env_get_test_directory(cldb_env_t *env,
                                                cldb_buf_t *path) {
  static const char kTestDirectory[] = "/test";

  CLDB_UNUSED(env);
  if (path == NULL) {
    return cldb_status_invalid_argument("GetTestDirectory", "path is NULL");
  }
  if (!cldb_buf_assign(path, kTestDirectory, sizeof(kTestDirectory) - 1)) {
    return cldb_env_out_of_memory("GetTestDirectory");
  }
  return cldb_status_ok();
}

static cldb_status_t mem_env_new_logger(cldb_env_t *env, const char *fname,
                                        cldb_logger_t **result) {
  mem_env_t *self = (mem_env_t *)env;
  mem_env_logger_t *logger;
  mem_env_file_state_t *state = NULL;

  if (result == NULL) {
    return cldb_status_invalid_argument("NewLogger", "result is NULL");
  }
  *result = NULL;
  if (fname == NULL) {
    return cldb_status_invalid_argument("NewLogger", "fname is NULL");
  }
  if (self->log_target == CLDB_MEMENV_LOG_MEMORY) {
    state = open_file_state(self, fname, true, false);
    if (state == NULL) {
      return cldb_env_out_of_memory("NewLogger");
    }
  }
  logger = calloc(1, sizeof(*logger));
  if (logger == NULL) {
    if (state != NULL) {
      file_state_unref(state);
    }
    return cldb_env_out_of_memory("NewLogger");
  }
  logger->base.vtbl = &kLoggerVtbl;
  logger->state = state;
  *result = &logger->base;
  return cldb_status_ok();
}

static uint64_t mem_env_now_micros(cldb_env_t *env) {
  mem_env_t *self = (mem_env_t *)env;

  return cldb_env_now_micros(self->wrapper.target);
}

static void mem_env_sleep_for_microseconds(cldb_env_t *env, int micros) {
  mem_env_t *self = (mem_env_t *)env;

  cldb_env_sleep_for_microseconds(self->wrapper.target, micros);
}

/* 终止操作：调用者保证没有并发方法或存活对象；无需再持 env->mutex。 */
static void mem_env_destroy(cldb_env_t *env) {
  mem_env_t *self = (mem_env_t *)env;
  size_t i;

  for (i = 0; i < self->file_count; ++i) {
    free(self->files[i].path);
    file_state_unref(self->files[i].state);
  }
  free(self->files);
  for (i = 0; i < self->lock_count; ++i) {
    free(self->locked_paths[i]);
  }
  free(self->locked_paths);
  (void)cldb_mutex_destroy(&self->mutex);
  free(self);
}

static const cldb_env_vtbl_t kMemEnvVtbl = {
    .new_sequential_file = mem_env_new_sequential_file,
    .new_random_access_file = mem_env_new_random_access_file,
    .new_writable_file = mem_env_new_writable_file,
    .new_appendable_file = mem_env_new_appendable_file,
    .file_exists = mem_env_file_exists,
    .get_children = mem_env_get_children,
    .remove_file = mem_env_remove_file,
    .delete_file = NULL,
    .create_dir = mem_env_create_dir,
    .remove_dir = mem_env_remove_dir,
    .delete_dir = NULL,
    .get_file_size = mem_env_get_file_size,
    .rename_file = mem_env_rename_file,
    .lock_file = mem_env_lock_file,
    .unlock_file = mem_env_unlock_file,
    .schedule = mem_env_schedule,
    .start_thread = mem_env_start_thread,
    .get_test_directory = mem_env_get_test_directory,
    .new_logger = mem_env_new_logger,
    .now_micros = mem_env_now_micros,
    .sleep_for_microseconds = mem_env_sleep_for_microseconds,
    .destroy = mem_env_destroy,
};

void cldb_memenv_options_init(cldb_memenv_options_t *options) {
  if (options == NULL) {
    return;
  }
  options->log_target = CLDB_MEMENV_LOG_MEMORY;
}

cldb_env_t *cldb_memenv_create_with_options(
    cldb_env_t *base_env, const cldb_memenv_options_t *options) {
  mem_env_t *env;
  cldb_memenv_log_target_t log_target = CLDB_MEMENV_LOG_MEMORY;

  if (options != NULL) {
    if (options->log_target != CLDB_MEMENV_LOG_MEMORY &&
        options->log_target != CLDB_MEMENV_LOG_STDERR) {
      return NULL;
    }
    log_target = options->log_target;
  }
  if (base_env == NULL) {
    base_env = cldb_env_default();
  }
  env = calloc(1, sizeof(*env));
  if (env == NULL) {
    return NULL;
  }
  cldb_env_wrapper_init(&env->wrapper, base_env);
  env->wrapper.base.vtbl = &kMemEnvVtbl;
  env->log_target = log_target;
  if (!cldb_status_is_ok(cldb_mutex_init(&env->mutex))) {
    free(env);
    return NULL;
  }
  return &env->wrapper.base;
}

cldb_env_t *cldb_memenv_create(cldb_env_t *base_env) {
  return cldb_memenv_create_with_options(base_env, NULL);
}
