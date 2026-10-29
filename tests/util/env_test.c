#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cleveldb/env.h"
#include "harness.h"
#include "status_test_util.h"

/* mock Env 记录的调用类别；用于逐项确认 EnvWrapper 的转发。 */
typedef enum mock_call {
  MOCK_NEW_SEQ,
  MOCK_NEW_RAND,
  MOCK_NEW_WRITABLE,
  MOCK_NEW_APPENDABLE,
  MOCK_FILE_EXISTS,
  MOCK_GET_CHILDREN,
  MOCK_REMOVE_FILE,
  MOCK_DELETE_FILE,
  MOCK_CREATE_DIR,
  MOCK_REMOVE_DIR,
  MOCK_DELETE_DIR,
  MOCK_GET_FILE_SIZE,
  MOCK_RENAME_FILE,
  MOCK_LOCK_FILE,
  MOCK_UNLOCK_FILE,
  MOCK_SCHEDULE,
  MOCK_START_THREAD,
  MOCK_GET_TEST_DIR,
  MOCK_NEW_LOGGER,
  MOCK_NOW_MICROS,
  MOCK_SLEEP,
  MOCK_DESTROY,
  MOCK_CALL_COUNT
} mock_call_t;

typedef struct mock_env {
  cldb_env_t base; /* 必须为首成员 */
  unsigned calls[MOCK_CALL_COUNT];
  cldb_status_t new_sequential_status;
  cldb_status_t new_random_status;
  cldb_status_t new_writable_status;
  cldb_status_t new_appendable_status;
  cldb_status_t get_children_status;
  cldb_status_t remove_file_status;
  cldb_status_t delete_file_status;
  cldb_status_t create_dir_status;
  cldb_status_t remove_dir_status;
  cldb_status_t delete_dir_status;
  cldb_status_t get_file_size_status;
  cldb_status_t rename_status;
  cldb_status_t lock_status;
  cldb_status_t unlock_status;
  cldb_status_t get_test_directory_status;
  cldb_status_t new_logger_status;
  cldb_status_t seq_read_status;
  cldb_status_t seq_skip_status;
  cldb_status_t rand_read_status;
  cldb_status_t append_status;
  cldb_status_t close_status;
  cldb_status_t flush_status;
  cldb_status_t sync_status;
  bool file_exists_result;
  bool saw_stale_children;
  uint64_t file_size_value;
  uint64_t now_micros_value;
  const char *test_directory;
  const char *seq_data;
  size_t seq_size;
  size_t seq_chunk;
  size_t seq_reads_before_error; /* 成功读次数上限；SIZE_MAX 表示不注入错误 */
  size_t seq_read_calls;
  size_t seq_skip_calls;
  size_t rand_read_calls;
  uint64_t last_rand_offset;
  size_t last_rand_size;
  const char *children[8];
  size_t child_count;
  char last_name[128];
  char last_target[128];
  int sleep_micros;
  cldb_buf_t written;
  unsigned seq_destroys;
  unsigned rand_destroys;
  unsigned writable_destroys;
  unsigned logger_destroys;
  unsigned append_calls;
  unsigned close_calls;
  unsigned flush_calls;
  unsigned sync_calls;
  unsigned logv_calls;
  char log_text[512];
  void (*scheduled_fn)(void *);
  void *scheduled_arg;
} mock_env_t;

typedef struct mock_seq_file {
  cldb_sequential_file_t base;
  mock_env_t *env;
  size_t pos;
} mock_seq_file_t;

typedef struct mock_rand_file {
  cldb_random_access_file_t base;
  mock_env_t *env;
} mock_rand_file_t;

typedef struct mock_writable_file {
  cldb_writable_file_t base;
  mock_env_t *env;
} mock_writable_file_t;

typedef struct mock_logger {
  cldb_logger_t base;
  mock_env_t *env;
} mock_logger_t;

typedef struct mock_lock {
  cldb_file_lock_t base; /* 必须为首成员 */
  mock_env_t *env;
} mock_lock_t;

/* GCC 16 在 -O2 下会把经 EnvWrapper 内联的基类指针误判为 wrapper 对象；
 * 用 noinline 隔离转换以避免 -Warray-bounds 误报。 */
static __attribute__((noinline)) mock_env_t *mock_env_from_base(
    cldb_env_t *base) {
  return (mock_env_t *)base;
}

static void copy_arg(char *dst, size_t capacity, const char *src) {
  if (capacity == 0) {
    return;
  }
  if (src == NULL) {
    dst[0] = '\0';
    return;
  }
  (void)snprintf(dst, capacity, "%s", src);
}

static cldb_status_t mock_seq_read(cldb_sequential_file_t *file, size_t n,
                                   cldb_slice_t *result, char *scratch) {
  mock_seq_file_t *self = (mock_seq_file_t *)file;
  mock_env_t *env = self->env;
  size_t remaining;
  size_t amount;

  env->seq_read_calls++;
  if (env->seq_reads_before_error != SIZE_MAX &&
      env->seq_read_calls > env->seq_reads_before_error) {
    *result = cldb_slice_empty();
    return env->seq_read_status;
  }
  if (self->pos >= env->seq_size) {
    *result = cldb_slice_empty();
    return cldb_status_ok();
  }
  remaining = env->seq_size - self->pos;
  amount = n < remaining ? n : remaining;
  if (env->seq_chunk != 0 && amount > env->seq_chunk) {
    amount = env->seq_chunk;
  }
  if (amount == 0) {
    *result = cldb_slice_empty();
    return cldb_status_ok();
  }
  memcpy(scratch, env->seq_data + self->pos, amount);
  *result = cldb_slice_make(scratch, amount);
  self->pos += amount;
  return cldb_status_ok();
}

static cldb_status_t mock_seq_skip(cldb_sequential_file_t *file, uint64_t n) {
  mock_seq_file_t *self = (mock_seq_file_t *)file;
  mock_env_t *env = self->env;
  uint64_t remaining;

  env->seq_skip_calls++;
  if (!cldb_status_is_ok(env->seq_skip_status)) {
    return env->seq_skip_status;
  }
  remaining = (uint64_t)(env->seq_size - self->pos);
  if (n >= remaining) {
    self->pos = env->seq_size;
  } else {
    self->pos += (size_t)n;
  }
  return cldb_status_ok();
}

static void mock_seq_destroy(cldb_sequential_file_t *file) {
  mock_seq_file_t *self = (mock_seq_file_t *)file;

  self->env->seq_destroys++;
  free(self);
}

static const cldb_sequential_file_vtbl_t kMockSequentialVtbl = {
    .read = mock_seq_read,
    .skip = mock_seq_skip,
    .destroy = mock_seq_destroy,
};

static cldb_status_t mock_rand_read(cldb_random_access_file_t *file,
                                    uint64_t offset, size_t n,
                                    cldb_slice_t *result, char *scratch) {
  mock_rand_file_t *self = (mock_rand_file_t *)file;
  mock_env_t *env = self->env;
  size_t index;
  size_t remaining;
  size_t amount;

  env->rand_read_calls++;
  env->last_rand_offset = offset;
  env->last_rand_size = n;
  if (!cldb_status_is_ok(env->rand_read_status)) {
    *result = cldb_slice_empty();
    return env->rand_read_status;
  }
  if (offset >= (uint64_t)env->seq_size || n == 0) {
    *result = cldb_slice_empty();
    return cldb_status_ok();
  }
  index = (size_t)offset;
  remaining = env->seq_size - index;
  amount = n < remaining ? n : remaining;
  memcpy(scratch, env->seq_data + index, amount);
  *result = cldb_slice_make(scratch, amount);
  return cldb_status_ok();
}

static void mock_rand_destroy(cldb_random_access_file_t *file) {
  mock_rand_file_t *self = (mock_rand_file_t *)file;

  self->env->rand_destroys++;
  free(self);
}

static const cldb_random_access_file_vtbl_t kMockRandomVtbl = {
    .read = mock_rand_read,
    .destroy = mock_rand_destroy,
};

static cldb_status_t mock_writable_append(cldb_writable_file_t *file,
                                          cldb_slice_t data) {
  mock_writable_file_t *self = (mock_writable_file_t *)file;
  mock_env_t *env = self->env;

  env->append_calls++;
  if (!cldb_status_is_ok(env->append_status)) {
    return env->append_status;
  }
  if (!cldb_buf_append(&env->written, data.data, data.size)) {
    return cldb_status_io("mock.append", "out of memory");
  }
  return cldb_status_ok();
}

static cldb_status_t mock_writable_close(cldb_writable_file_t *file) {
  mock_writable_file_t *self = (mock_writable_file_t *)file;

  self->env->close_calls++;
  return self->env->close_status;
}

static cldb_status_t mock_writable_flush(cldb_writable_file_t *file) {
  mock_writable_file_t *self = (mock_writable_file_t *)file;

  self->env->flush_calls++;
  return self->env->flush_status;
}

static cldb_status_t mock_writable_sync(cldb_writable_file_t *file) {
  mock_writable_file_t *self = (mock_writable_file_t *)file;

  self->env->sync_calls++;
  return self->env->sync_status;
}

static void mock_writable_destroy(cldb_writable_file_t *file) {
  mock_writable_file_t *self = (mock_writable_file_t *)file;

  self->env->writable_destroys++;
  free(self);
}

static const cldb_writable_file_vtbl_t kMockWritableVtbl = {
    .append = mock_writable_append,
    .close = mock_writable_close,
    .flush = mock_writable_flush,
    .sync = mock_writable_sync,
    .destroy = mock_writable_destroy,
};

static void mock_logger_logv(cldb_logger_t *logger, const char *format,
                             va_list ap) {
  mock_logger_t *self = (mock_logger_t *)logger;

  self->env->logv_calls++;
  (void)vsnprintf(self->env->log_text, sizeof(self->env->log_text), format, ap);
}

static void mock_logger_destroy(cldb_logger_t *logger) {
  mock_logger_t *self = (mock_logger_t *)logger;

  self->env->logger_destroys++;
  free(self);
}

static const cldb_logger_vtbl_t kMockLoggerVtbl = {
    .logv = mock_logger_logv,
    .destroy = mock_logger_destroy,
};

static cldb_status_t mock_new_sequential_file(
    cldb_env_t *base, const char *fname, cldb_sequential_file_t **result) {
  mock_env_t *env = mock_env_from_base(base);
  mock_seq_file_t *file;

  env->calls[MOCK_NEW_SEQ]++;
  copy_arg(env->last_name, sizeof(env->last_name), fname);
  if (!cldb_status_is_ok(env->new_sequential_status)) {
    *result = NULL;
    return env->new_sequential_status;
  }
  file = calloc(1, sizeof(*file));
  if (file == NULL) {
    *result = NULL;
    return cldb_status_io("mock.new_sequential_file", "out of memory");
  }
  file->base.vtbl = &kMockSequentialVtbl;
  file->env = env;
  *result = &file->base;
  return cldb_status_ok();
}

static cldb_status_t mock_new_random_access_file(
    cldb_env_t *base, const char *fname, cldb_random_access_file_t **result) {
  mock_env_t *env = mock_env_from_base(base);
  mock_rand_file_t *file;

  env->calls[MOCK_NEW_RAND]++;
  copy_arg(env->last_name, sizeof(env->last_name), fname);
  if (!cldb_status_is_ok(env->new_random_status)) {
    *result = NULL;
    return env->new_random_status;
  }
  file = calloc(1, sizeof(*file));
  if (file == NULL) {
    *result = NULL;
    return cldb_status_io("mock.new_random_access_file", "out of memory");
  }
  file->base.vtbl = &kMockRandomVtbl;
  file->env = env;
  *result = &file->base;
  return cldb_status_ok();
}

static cldb_status_t mock_new_writable_file(cldb_env_t *base,
                                            const char *fname,
                                            cldb_writable_file_t **result) {
  mock_env_t *env = mock_env_from_base(base);
  mock_writable_file_t *file;

  env->calls[MOCK_NEW_WRITABLE]++;
  copy_arg(env->last_name, sizeof(env->last_name), fname);
  if (!cldb_status_is_ok(env->new_writable_status)) {
    *result = NULL;
    return env->new_writable_status;
  }
  file = calloc(1, sizeof(*file));
  if (file == NULL) {
    *result = NULL;
    return cldb_status_io("mock.new_writable_file", "out of memory");
  }
  file->base.vtbl = &kMockWritableVtbl;
  file->env = env;
  *result = &file->base;
  return cldb_status_ok();
}

static cldb_status_t mock_new_appendable_file(
    cldb_env_t *base, const char *fname, cldb_writable_file_t **result) {
  mock_env_t *env = mock_env_from_base(base);
  mock_writable_file_t *file;

  env->calls[MOCK_NEW_APPENDABLE]++;
  copy_arg(env->last_name, sizeof(env->last_name), fname);
  if (!cldb_status_is_ok(env->new_appendable_status)) {
    *result = NULL;
    return env->new_appendable_status;
  }
  file = calloc(1, sizeof(*file));
  if (file == NULL) {
    *result = NULL;
    return cldb_status_io("mock.new_appendable_file", "out of memory");
  }
  file->base.vtbl = &kMockWritableVtbl;
  file->env = env;
  *result = &file->base;
  return cldb_status_ok();
}

static bool mock_file_exists(cldb_env_t *base, const char *fname) {
  mock_env_t *env = mock_env_from_base(base);

  env->calls[MOCK_FILE_EXISTS]++;
  copy_arg(env->last_name, sizeof(env->last_name), fname);
  return env->file_exists_result;
}

static cldb_status_t mock_get_children(cldb_env_t *base, const char *dir,
                                       cldb_strvec_t *result) {
  mock_env_t *env = mock_env_from_base(base);
  size_t i;

  env->calls[MOCK_GET_CHILDREN]++;
  copy_arg(env->last_name, sizeof(env->last_name), dir);
  if (cldb_strvec_size(result) != 0) {
    env->saw_stale_children = true;
  }
  if (!cldb_status_is_ok(env->get_children_status)) {
    return env->get_children_status;
  }
  for (i = 0; i < env->child_count; ++i) {
    if (!cldb_strvec_push(result, env->children[i])) {
      return cldb_status_io("mock.get_children", "out of memory");
    }
  }
  return cldb_status_ok();
}

static cldb_status_t mock_remove_file(cldb_env_t *base, const char *fname) {
  mock_env_t *env = mock_env_from_base(base);

  env->calls[MOCK_REMOVE_FILE]++;
  copy_arg(env->last_name, sizeof(env->last_name), fname);
  return env->remove_file_status;
}

static cldb_status_t mock_delete_file(cldb_env_t *base, const char *fname) {
  mock_env_t *env = mock_env_from_base(base);

  env->calls[MOCK_DELETE_FILE]++;
  copy_arg(env->last_name, sizeof(env->last_name), fname);
  return env->delete_file_status;
}

static cldb_status_t mock_create_dir(cldb_env_t *base, const char *dirname) {
  mock_env_t *env = mock_env_from_base(base);

  env->calls[MOCK_CREATE_DIR]++;
  copy_arg(env->last_name, sizeof(env->last_name), dirname);
  return env->create_dir_status;
}

static cldb_status_t mock_remove_dir(cldb_env_t *base, const char *dirname) {
  mock_env_t *env = mock_env_from_base(base);

  env->calls[MOCK_REMOVE_DIR]++;
  copy_arg(env->last_name, sizeof(env->last_name), dirname);
  return env->remove_dir_status;
}

static cldb_status_t mock_delete_dir(cldb_env_t *base, const char *dirname) {
  mock_env_t *env = mock_env_from_base(base);

  env->calls[MOCK_DELETE_DIR]++;
  copy_arg(env->last_name, sizeof(env->last_name), dirname);
  return env->delete_dir_status;
}

static cldb_status_t mock_get_file_size(cldb_env_t *base, const char *fname,
                                        uint64_t *size) {
  mock_env_t *env = mock_env_from_base(base);

  env->calls[MOCK_GET_FILE_SIZE]++;
  copy_arg(env->last_name, sizeof(env->last_name), fname);
  if (!cldb_status_is_ok(env->get_file_size_status)) {
    *size = 0;
    return env->get_file_size_status;
  }
  *size = env->file_size_value;
  return cldb_status_ok();
}

static cldb_status_t mock_rename_file(cldb_env_t *base, const char *src,
                                      const char *target) {
  mock_env_t *env = mock_env_from_base(base);

  env->calls[MOCK_RENAME_FILE]++;
  copy_arg(env->last_name, sizeof(env->last_name), src);
  copy_arg(env->last_target, sizeof(env->last_target), target);
  return env->rename_status;
}

static cldb_status_t mock_lock_file(cldb_env_t *base, const char *fname,
                                    cldb_file_lock_t **lock) {
  mock_env_t *env = mock_env_from_base(base);
  mock_lock_t *self;

  env->calls[MOCK_LOCK_FILE]++;
  copy_arg(env->last_name, sizeof(env->last_name), fname);
  if (!cldb_status_is_ok(env->lock_status)) {
    *lock = NULL;
    return env->lock_status;
  }
  self = calloc(1, sizeof(*self));
  if (self == NULL) {
    *lock = NULL;
    return cldb_status_io("mock.lock_file", "out of memory");
  }
  self->base.state = self;
  self->env = env;
  *lock = &self->base;
  return cldb_status_ok();
}

static cldb_status_t mock_unlock_file(cldb_env_t *base,
                                      cldb_file_lock_t *lock) {
  mock_env_t *env = mock_env_from_base(base);
  mock_lock_t *self = (mock_lock_t *)lock;

  env->calls[MOCK_UNLOCK_FILE]++;
  free(self);
  return env->unlock_status;
}

static void mock_schedule(cldb_env_t *base, void (*fn)(void *), void *arg) {
  mock_env_t *env = mock_env_from_base(base);

  env->calls[MOCK_SCHEDULE]++;
  env->scheduled_fn = fn;
  env->scheduled_arg = arg;
  if (fn != NULL) {
    fn(arg);
  }
}

static void mock_start_thread(cldb_env_t *base, void (*fn)(void *), void *arg) {
  mock_env_t *env = mock_env_from_base(base);

  env->calls[MOCK_START_THREAD]++;
  env->scheduled_fn = fn;
  env->scheduled_arg = arg;
  if (fn != NULL) {
    fn(arg);
  }
}

static cldb_status_t mock_get_test_directory(cldb_env_t *base,
                                             cldb_buf_t *path) {
  mock_env_t *env = mock_env_from_base(base);

  env->calls[MOCK_GET_TEST_DIR]++;
  if (!cldb_status_is_ok(env->get_test_directory_status)) {
    return env->get_test_directory_status;
  }
  if (env->test_directory == NULL) {
    return cldb_status_invalid_argument("mock.get_test_directory",
                                        "test_directory is NULL");
  }
  if (!cldb_buf_assign(path, env->test_directory,
                       strlen(env->test_directory))) {
    return cldb_status_io("mock.get_test_directory", "out of memory");
  }
  return cldb_status_ok();
}

static cldb_status_t mock_new_logger(cldb_env_t *base, const char *fname,
                                     cldb_logger_t **result) {
  mock_env_t *env = mock_env_from_base(base);
  mock_logger_t *logger;

  env->calls[MOCK_NEW_LOGGER]++;
  copy_arg(env->last_name, sizeof(env->last_name), fname);
  if (!cldb_status_is_ok(env->new_logger_status)) {
    *result = NULL;
    return env->new_logger_status;
  }
  logger = calloc(1, sizeof(*logger));
  if (logger == NULL) {
    *result = NULL;
    return cldb_status_io("mock.new_logger", "out of memory");
  }
  logger->base.vtbl = &kMockLoggerVtbl;
  logger->env = env;
  *result = &logger->base;
  return cldb_status_ok();
}

static uint64_t mock_now_micros(cldb_env_t *base) {
  mock_env_t *env = mock_env_from_base(base);

  env->calls[MOCK_NOW_MICROS]++;
  return env->now_micros_value;
}

static void mock_sleep_for_microseconds(cldb_env_t *base, int micros) {
  mock_env_t *env = mock_env_from_base(base);

  env->calls[MOCK_SLEEP]++;
  env->sleep_micros = micros;
}

static void mock_destroy(cldb_env_t *base) {
  mock_env_t *env = mock_env_from_base(base);

  env->calls[MOCK_DESTROY]++;
}

static const cldb_env_vtbl_t kMockVtbl = {
    .new_sequential_file = mock_new_sequential_file,
    .new_random_access_file = mock_new_random_access_file,
    .new_writable_file = mock_new_writable_file,
    .new_appendable_file = mock_new_appendable_file,
    .file_exists = mock_file_exists,
    .get_children = mock_get_children,
    .remove_file = mock_remove_file,
    .delete_file = mock_delete_file,
    .create_dir = mock_create_dir,
    .remove_dir = mock_remove_dir,
    .delete_dir = mock_delete_dir,
    .get_file_size = mock_get_file_size,
    .rename_file = mock_rename_file,
    .lock_file = mock_lock_file,
    .unlock_file = mock_unlock_file,
    .schedule = mock_schedule,
    .start_thread = mock_start_thread,
    .get_test_directory = mock_get_test_directory,
    .new_logger = mock_new_logger,
    .now_micros = mock_now_micros,
    .sleep_for_microseconds = mock_sleep_for_microseconds,
    .destroy = mock_destroy,
};

static void mock_env_init(mock_env_t *env) {
  memset(env, 0, sizeof(*env));
  env->base.vtbl = &kMockVtbl;
  env->file_exists_result = true;
  env->file_size_value = 1234;
  env->now_micros_value = UINT64_C(123456789);
  env->test_directory = "/tmp/mock";
  env->seq_reads_before_error = SIZE_MAX;
  cldb_buf_init(&env->written);
}

static void mock_env_cleanup(mock_env_t *env) { cldb_buf_free(&env->written); }

static void mark_called(void *arg) {
  bool *called = arg;

  *called = true;
}

CLDB_TEST(env_wrapper_forwards_all) {
  mock_env_t target;
  cldb_env_wrapper_t wrapper;
  cldb_sequential_file_t *seq = NULL;
  cldb_random_access_file_t *random = NULL;
  cldb_writable_file_t *writable = NULL;
  cldb_writable_file_t *appendable = NULL;
  cldb_file_lock_t *lock = NULL;
  cldb_logger_t *logger = NULL;
  cldb_strvec_t children;
  cldb_buf_t path;
  cldb_slice_t read_result;
  char scratch[16];
  uint64_t file_size = 0;
  bool callback_called = false;

  mock_env_init(&target);
  cldb_env_wrapper_init(&wrapper, &target.base);
  CLDB_EXPECT_EQ_PTR(&target.base, cldb_env_wrapper_target(&wrapper));
  CLDB_EXPECT_EQ_PTR(NULL, cldb_env_wrapper_target(NULL));

  /* 顺序文件：New、Read、Skip、destroy 全部转发。 */
  CLDB_EXPECT_OK(cldb_env_new_sequential_file(&wrapper.base, "seq", &seq));
  CLDB_EXPECT_EQ_UINT(1, target.calls[MOCK_NEW_SEQ]);
  CLDB_EXPECT_EQ_STR("seq", target.last_name);
  CLDB_ASSERT_TRUE(seq != NULL);
  target.seq_data = "abcdef";
  target.seq_size = 6;
  CLDB_EXPECT_OK(cldb_sequential_file_read(seq, 3, &read_result, scratch));
  CLDB_EXPECT_EQ_UINT(3, read_result.size);
  CLDB_EXPECT_EQ_INT(0, memcmp(read_result.data, "abc", 3));
  CLDB_EXPECT_OK(cldb_sequential_file_read(seq, 10, &read_result, scratch));
  CLDB_EXPECT_EQ_UINT(3, read_result.size);
  CLDB_EXPECT_EQ_INT(0, memcmp(read_result.data, "def", 3));
  CLDB_EXPECT_OK(cldb_sequential_file_skip(seq, 100));
  CLDB_EXPECT_EQ_UINT(1, target.seq_skip_calls);
  CLDB_EXPECT_OK(cldb_sequential_file_read(seq, 1, &read_result, scratch));
  CLDB_EXPECT_EQ_UINT(0, read_result.size);
  cldb_sequential_file_destroy(seq);
  CLDB_EXPECT_EQ_UINT(1, target.seq_destroys);

  /* 随机读：offset 和长度原样传给实现。 */
  CLDB_EXPECT_OK(
      cldb_env_new_random_access_file(&wrapper.base, "rand", &random));
  CLDB_EXPECT_EQ_UINT(1, target.calls[MOCK_NEW_RAND]);
  CLDB_ASSERT_TRUE(random != NULL);
  CLDB_EXPECT_OK(cldb_random_access_file_read(random, 2, 3, &read_result,
                                              scratch));
  CLDB_EXPECT_EQ_UINT(3, read_result.size);
  CLDB_EXPECT_EQ_INT(0, memcmp(read_result.data, "cde", 3));
  CLDB_EXPECT_EQ_UINT(2, target.last_rand_offset);
  CLDB_EXPECT_EQ_UINT(3, target.last_rand_size);
  CLDB_EXPECT_OK(cldb_random_access_file_read(random, 100, 3, &read_result,
                                              scratch));
  CLDB_EXPECT_EQ_UINT(0, read_result.size);
  cldb_random_access_file_destroy(random);
  CLDB_EXPECT_EQ_UINT(1, target.rand_destroys);

  /* 写文件：Append、Flush、Sync、Close、destroy。 */
  CLDB_EXPECT_OK(cldb_env_new_writable_file(&wrapper.base, "w", &writable));
  CLDB_EXPECT_EQ_UINT(1, target.calls[MOCK_NEW_WRITABLE]);
  CLDB_ASSERT_TRUE(writable != NULL);
  CLDB_EXPECT_OK(cldb_writable_file_append(writable,
                                           cldb_slice_make("ab", 2)));
  CLDB_EXPECT_OK(cldb_writable_file_flush(writable));
  CLDB_EXPECT_OK(cldb_writable_file_sync(writable));
  CLDB_EXPECT_OK(cldb_writable_file_close(writable));
  cldb_writable_file_destroy(writable);
  CLDB_EXPECT_EQ_UINT(1, target.append_calls);
  CLDB_EXPECT_EQ_UINT(1, target.flush_calls);
  CLDB_EXPECT_EQ_UINT(1, target.sync_calls);
  CLDB_EXPECT_EQ_UINT(1, target.close_calls);
  CLDB_EXPECT_EQ_UINT(1, target.writable_destroys);
  CLDB_EXPECT_EQ_STR("ab", cldb_buf_cstr(&target.written));

  CLDB_EXPECT_OK(
      cldb_env_new_appendable_file(&wrapper.base, "append", &appendable));
  CLDB_EXPECT_EQ_UINT(1, target.calls[MOCK_NEW_APPENDABLE]);
  CLDB_ASSERT_TRUE(appendable != NULL);
  cldb_writable_file_destroy(appendable);
  CLDB_EXPECT_EQ_UINT(2, target.writable_destroys);

  CLDB_EXPECT_TRUE(cldb_env_file_exists(&wrapper.base, "exists"));
  CLDB_EXPECT_EQ_UINT(1, target.calls[MOCK_FILE_EXISTS]);
  CLDB_EXPECT_EQ_STR("exists", target.last_name);

  /* GetChildren 前应清空调用者数组，旧元素不能混入结果。 */
  target.children[0] = "one";
  target.children[1] = "two";
  target.child_count = 2;
  cldb_strvec_init(&children);
  CLDB_ASSERT_TRUE(cldb_strvec_push(&children, "stale"));
  CLDB_EXPECT_OK(cldb_env_get_children(&wrapper.base, "dir", &children));
  CLDB_EXPECT_EQ_UINT(1, target.calls[MOCK_GET_CHILDREN]);
  CLDB_EXPECT_FALSE(target.saw_stale_children);
  CLDB_EXPECT_EQ_UINT(2, cldb_strvec_size(&children));
  CLDB_EXPECT_EQ_STR("one", cldb_strvec_get(&children, 0));
  CLDB_EXPECT_EQ_STR("two", cldb_strvec_get(&children, 1));
  cldb_strvec_free(&children);

  CLDB_EXPECT_OK(cldb_env_remove_file(&wrapper.base, "rm"));
  CLDB_EXPECT_EQ_UINT(1, target.calls[MOCK_REMOVE_FILE]);
  CLDB_EXPECT_OK(cldb_env_delete_file(&wrapper.base, "del"));
  CLDB_EXPECT_EQ_UINT(1, target.calls[MOCK_DELETE_FILE]);
  CLDB_EXPECT_OK(cldb_env_create_dir(&wrapper.base, "mkdir"));
  CLDB_EXPECT_EQ_UINT(1, target.calls[MOCK_CREATE_DIR]);
  CLDB_EXPECT_OK(cldb_env_remove_dir(&wrapper.base, "rmdir"));
  CLDB_EXPECT_EQ_UINT(1, target.calls[MOCK_REMOVE_DIR]);
  CLDB_EXPECT_OK(cldb_env_delete_dir(&wrapper.base, "deldir"));
  CLDB_EXPECT_EQ_UINT(1, target.calls[MOCK_DELETE_DIR]);

  CLDB_EXPECT_OK(cldb_env_get_file_size(&wrapper.base, "size", &file_size));
  CLDB_EXPECT_EQ_UINT(1234, file_size);
  CLDB_EXPECT_EQ_UINT(1, target.calls[MOCK_GET_FILE_SIZE]);
  CLDB_EXPECT_OK(cldb_env_rename_file(&wrapper.base, "src", "dst"));
  CLDB_EXPECT_EQ_UINT(1, target.calls[MOCK_RENAME_FILE]);
  CLDB_EXPECT_EQ_STR("src", target.last_name);
  CLDB_EXPECT_EQ_STR("dst", target.last_target);

  CLDB_EXPECT_OK(cldb_env_lock_file(&wrapper.base, "lock", &lock));
  CLDB_EXPECT_EQ_UINT(1, target.calls[MOCK_LOCK_FILE]);
  CLDB_ASSERT_TRUE(lock != NULL);
  CLDB_EXPECT_OK(cldb_env_unlock_file(&wrapper.base, lock));
  CLDB_EXPECT_EQ_UINT(1, target.calls[MOCK_UNLOCK_FILE]);

  cldb_env_schedule(&wrapper.base, mark_called, &callback_called);
  CLDB_EXPECT_EQ_UINT(1, target.calls[MOCK_SCHEDULE]);
  CLDB_EXPECT_TRUE(callback_called);
  callback_called = false;
  cldb_env_start_thread(&wrapper.base, mark_called, &callback_called);
  CLDB_EXPECT_EQ_UINT(1, target.calls[MOCK_START_THREAD]);
  CLDB_EXPECT_TRUE(callback_called);

  cldb_buf_init(&path);
  CLDB_EXPECT_OK(cldb_env_get_test_directory(&wrapper.base, &path));
  CLDB_EXPECT_EQ_UINT(1, target.calls[MOCK_GET_TEST_DIR]);
  CLDB_EXPECT_EQ_STR("/tmp/mock", cldb_buf_cstr(&path));
  cldb_buf_free(&path);

  CLDB_EXPECT_OK(cldb_env_new_logger(&wrapper.base, "log", &logger));
  CLDB_EXPECT_EQ_UINT(1, target.calls[MOCK_NEW_LOGGER]);
  CLDB_ASSERT_TRUE(logger != NULL);
  cldb_log(logger, "%s=%d", "x", 7);
  CLDB_EXPECT_EQ_UINT(1, target.logv_calls);
  CLDB_EXPECT_EQ_STR("x=7", target.log_text);
  cldb_logger_destroy(logger);
  CLDB_EXPECT_EQ_UINT(1, target.logger_destroys);

  CLDB_EXPECT_EQ_UINT(123456789, cldb_env_now_micros(&wrapper.base));
  CLDB_EXPECT_EQ_UINT(1, target.calls[MOCK_NOW_MICROS]);
  cldb_env_sleep_for_microseconds(&wrapper.base, 42);
  CLDB_EXPECT_EQ_UINT(1, target.calls[MOCK_SLEEP]);
  CLDB_EXPECT_EQ_INT(42, target.sleep_micros);

  /* wrapper 仅引用 target，销毁 wrapper 不销毁 target。 */
  cldb_env_destroy(&wrapper.base);
  CLDB_EXPECT_EQ_UINT(0, target.calls[MOCK_DESTROY]);
  cldb_env_destroy(&target.base);
  CLDB_EXPECT_EQ_UINT(1, target.calls[MOCK_DESTROY]);
  mock_env_cleanup(&target);
}

CLDB_TEST(env_optional_and_alias_methods) {
  mock_env_t env;
  cldb_env_vtbl_t vtbl;
  cldb_env_wrapper_t wrapper;
  cldb_writable_file_t *file;

  /* vtable 未提供 NewAppendableFile 时返回 NotSupported 并清空输出。 */
  mock_env_init(&env);
  vtbl = kMockVtbl;
  vtbl.new_appendable_file = NULL;
  env.base.vtbl = &vtbl;
  file = NULL;
  CLDB_EXPECT_STATUS(CLDB_STATUS_NOT_SUPPORTED,
                     cldb_env_new_appendable_file(&env.base, "f", &file));
  CLDB_EXPECT_EQ_PTR(NULL, file);
  CLDB_EXPECT_EQ_UINT(0, env.calls[MOCK_NEW_APPENDABLE]);
  cldb_env_wrapper_init(&wrapper, &env.base);
  file = NULL;
  CLDB_EXPECT_STATUS(
      CLDB_STATUS_NOT_SUPPORTED,
      cldb_env_new_appendable_file(&wrapper.base, "f", &file));
  CLDB_EXPECT_EQ_PTR(NULL, file);
  CLDB_EXPECT_EQ_UINT(0, env.calls[MOCK_NEW_APPENDABLE]);

  /* vtable 提供 NewAppendableFile 时，错误状态和 NULL 输出照常返回。 */
  mock_env_init(&env);
  env.new_appendable_status = cldb_status_io("mock.appendable", "fail");
  file = NULL;
  CLDB_EXPECT_STATUS(CLDB_STATUS_IO_ERROR,
                     cldb_env_new_appendable_file(&env.base, "f", &file));
  CLDB_EXPECT_EQ_PTR(NULL, file);
  CLDB_EXPECT_EQ_UINT(1, env.calls[MOCK_NEW_APPENDABLE]);

  /* RemoveFile 缺失时回退 DeleteFile，反向同理，且状态原样返回。 */
  mock_env_init(&env);
  vtbl = kMockVtbl;
  vtbl.remove_file = NULL;
  env.base.vtbl = &vtbl;
  env.remove_file_status = cldb_status_io("mock.remove", "must not run");
  env.delete_file_status = cldb_status_ok();
  CLDB_EXPECT_OK(cldb_env_remove_file(&env.base, "r"));
  CLDB_EXPECT_EQ_UINT(0, env.calls[MOCK_REMOVE_FILE]);
  CLDB_EXPECT_EQ_UINT(1, env.calls[MOCK_DELETE_FILE]);
  CLDB_EXPECT_OK(cldb_env_delete_file(&env.base, "d"));
  CLDB_EXPECT_EQ_UINT(2, env.calls[MOCK_DELETE_FILE]);
  env.delete_file_status = cldb_status_io("mock.delete", "fail");
  CLDB_EXPECT_STATUS(CLDB_STATUS_IO_ERROR,
                     cldb_env_remove_file(&env.base, "r"));

  mock_env_init(&env);
  vtbl = kMockVtbl;
  vtbl.delete_file = NULL;
  env.base.vtbl = &vtbl;
  env.delete_file_status = cldb_status_io("mock.delete", "must not run");
  env.remove_file_status = cldb_status_ok();
  CLDB_EXPECT_OK(cldb_env_delete_file(&env.base, "d"));
  CLDB_EXPECT_EQ_UINT(0, env.calls[MOCK_DELETE_FILE]);
  CLDB_EXPECT_EQ_UINT(1, env.calls[MOCK_REMOVE_FILE]);
  CLDB_EXPECT_OK(cldb_env_remove_file(&env.base, "r"));
  CLDB_EXPECT_EQ_UINT(2, env.calls[MOCK_REMOVE_FILE]);

  /* wrapper 的 DeleteFile 也要先尝试 target 的 DeleteFile，再回退 Remove。 */
  cldb_env_wrapper_init(&wrapper, &env.base);
  CLDB_EXPECT_OK(cldb_env_delete_file(&wrapper.base, "d"));
  CLDB_EXPECT_EQ_UINT(3, env.calls[MOCK_REMOVE_FILE]);
  CLDB_EXPECT_EQ_UINT(0, env.calls[MOCK_DELETE_FILE]);

  mock_env_init(&env);
  vtbl = kMockVtbl;
  vtbl.remove_file = NULL;
  vtbl.delete_file = NULL;
  env.base.vtbl = &vtbl;
  CLDB_EXPECT_STATUS(CLDB_STATUS_NOT_SUPPORTED,
                     cldb_env_remove_file(&env.base, "r"));
  CLDB_EXPECT_STATUS(CLDB_STATUS_NOT_SUPPORTED,
                     cldb_env_delete_file(&env.base, "d"));

  /* 目录的别名回退规则与文件一致。 */
  mock_env_init(&env);
  vtbl = kMockVtbl;
  vtbl.remove_dir = NULL;
  env.base.vtbl = &vtbl;
  env.remove_dir_status = cldb_status_io("mock.remove_dir", "must not run");
  CLDB_EXPECT_OK(cldb_env_remove_dir(&env.base, "r"));
  CLDB_EXPECT_EQ_UINT(0, env.calls[MOCK_REMOVE_DIR]);
  CLDB_EXPECT_EQ_UINT(1, env.calls[MOCK_DELETE_DIR]);

  mock_env_init(&env);
  vtbl = kMockVtbl;
  vtbl.delete_dir = NULL;
  env.base.vtbl = &vtbl;
  env.delete_dir_status = cldb_status_io("mock.delete_dir", "must not run");
  CLDB_EXPECT_OK(cldb_env_delete_dir(&env.base, "d"));
  CLDB_EXPECT_EQ_UINT(0, env.calls[MOCK_DELETE_DIR]);
  CLDB_EXPECT_EQ_UINT(1, env.calls[MOCK_REMOVE_DIR]);

  mock_env_init(&env);
  vtbl = kMockVtbl;
  vtbl.remove_dir = NULL;
  vtbl.delete_dir = NULL;
  env.base.vtbl = &vtbl;
  CLDB_EXPECT_STATUS(CLDB_STATUS_NOT_SUPPORTED,
                     cldb_env_remove_dir(&env.base, "r"));
  CLDB_EXPECT_STATUS(CLDB_STATUS_NOT_SUPPORTED,
                     cldb_env_delete_dir(&env.base, "d"));
}

typedef enum write_failure {
  WRITE_FAIL_NONE,
  WRITE_FAIL_CREATE,
  WRITE_FAIL_APPEND,
  WRITE_FAIL_SYNC,
  WRITE_FAIL_CLOSE
} write_failure_t;

static void init_write_failure(mock_env_t *env, write_failure_t failure) {
  mock_env_init(env);
  switch (failure) {
    case WRITE_FAIL_CREATE:
      env->new_writable_status = cldb_status_io("mock.create", "fail");
      break;
    case WRITE_FAIL_APPEND:
      env->append_status = cldb_status_io("mock.append", "fail");
      break;
    case WRITE_FAIL_SYNC:
      env->sync_status = cldb_status_io("mock.sync", "fail");
      break;
    case WRITE_FAIL_CLOSE:
      env->close_status = cldb_status_io("mock.close", "fail");
      break;
    case WRITE_FAIL_NONE:
      break;
  }
}

CLDB_TEST(env_write_string_to_file_success) {
  mock_env_t env;

  mock_env_init(&env);
  CLDB_EXPECT_OK(cldb_write_string_to_file(
      &env.base, cldb_slice_make("abc", 3), "file"));
  CLDB_EXPECT_EQ_UINT(1, env.calls[MOCK_NEW_WRITABLE]);
  CLDB_EXPECT_EQ_STR("file", env.last_name);
  CLDB_EXPECT_EQ_UINT(1, env.append_calls);
  CLDB_EXPECT_EQ_UINT(0, env.sync_calls);
  CLDB_EXPECT_EQ_UINT(1, env.close_calls);
  CLDB_EXPECT_EQ_UINT(1, env.writable_destroys);
  CLDB_EXPECT_EQ_UINT(0, env.calls[MOCK_REMOVE_FILE]);
  CLDB_EXPECT_EQ_STR("abc", cldb_buf_cstr(&env.written));

  cldb_buf_clear(&env.written);
  CLDB_EXPECT_OK(cldb_write_string_to_file_sync(
      &env.base, cldb_slice_make("def", 3), "file"));
  CLDB_EXPECT_EQ_UINT(2, env.append_calls);
  CLDB_EXPECT_EQ_UINT(1, env.sync_calls);
  CLDB_EXPECT_EQ_UINT(2, env.close_calls);
  CLDB_EXPECT_EQ_UINT(2, env.writable_destroys);
  CLDB_EXPECT_EQ_UINT(0, env.calls[MOCK_REMOVE_FILE]);
  CLDB_EXPECT_EQ_STR("def", cldb_buf_cstr(&env.written));

  cldb_buf_clear(&env.written);
  CLDB_EXPECT_OK(cldb_write_string_to_file(&env.base, cldb_slice_empty(),
                                           "empty"));
  CLDB_EXPECT_EQ_UINT(3, env.append_calls);
  CLDB_EXPECT_EQ_UINT(0, env.written.size);
  mock_env_cleanup(&env);
}

CLDB_TEST(env_write_string_to_file_failures) {
  mock_env_t env;
  cldb_status_t status;

  /* 创建失败：没有文件对象可销毁，也不应尝试删除。 */
  init_write_failure(&env, WRITE_FAIL_CREATE);
  status = cldb_write_string_to_file(&env.base, cldb_slice_make("x", 1),
                                     "file");
  CLDB_EXPECT_STATUS(CLDB_STATUS_IO_ERROR, status);
  CLDB_EXPECT_EQ_UINT(1, env.calls[MOCK_NEW_WRITABLE]);
  CLDB_EXPECT_EQ_UINT(0, env.append_calls);
  CLDB_EXPECT_EQ_UINT(0, env.close_calls);
  CLDB_EXPECT_EQ_UINT(0, env.writable_destroys);
  CLDB_EXPECT_EQ_UINT(0, env.calls[MOCK_REMOVE_FILE]);
  mock_env_cleanup(&env);

  /* Append 失败：销毁文件并删除残留文件，不调用 Close。 */
  init_write_failure(&env, WRITE_FAIL_APPEND);
  status = cldb_write_string_to_file(&env.base, cldb_slice_make("x", 1),
                                     "file");
  CLDB_EXPECT_STATUS(CLDB_STATUS_IO_ERROR, status);
  CLDB_EXPECT_EQ_UINT(1, env.append_calls);
  CLDB_EXPECT_EQ_UINT(0, env.close_calls);
  CLDB_EXPECT_EQ_UINT(1, env.writable_destroys);
  CLDB_EXPECT_EQ_UINT(1, env.calls[MOCK_REMOVE_FILE]);
  CLDB_EXPECT_EQ_UINT(0, env.written.size);
  mock_env_cleanup(&env);

  /* Sync 失败：不调用 Close，仍删除残留文件。 */
  init_write_failure(&env, WRITE_FAIL_SYNC);
  status = cldb_write_string_to_file_sync(&env.base, cldb_slice_make("x", 1),
                                          "file");
  CLDB_EXPECT_STATUS(CLDB_STATUS_IO_ERROR, status);
  CLDB_EXPECT_EQ_UINT(1, env.append_calls);
  CLDB_EXPECT_EQ_UINT(1, env.sync_calls);
  CLDB_EXPECT_EQ_UINT(0, env.close_calls);
  CLDB_EXPECT_EQ_UINT(1, env.writable_destroys);
  CLDB_EXPECT_EQ_UINT(1, env.calls[MOCK_REMOVE_FILE]);
  CLDB_EXPECT_EQ_STR("x", cldb_buf_cstr(&env.written));
  mock_env_cleanup(&env);

  /* Close 失败：错误原样返回，文件仍被销毁和删除。 */
  init_write_failure(&env, WRITE_FAIL_CLOSE);
  status = cldb_write_string_to_file(&env.base, cldb_slice_make("x", 1),
                                     "file");
  CLDB_EXPECT_STATUS(CLDB_STATUS_IO_ERROR, status);
  CLDB_EXPECT_EQ_UINT(1, env.close_calls);
  CLDB_EXPECT_EQ_UINT(1, env.writable_destroys);
  CLDB_EXPECT_EQ_UINT(1, env.calls[MOCK_REMOVE_FILE]);
  mock_env_cleanup(&env);

  /* 删除失败不能覆盖原始写入错误。 */
  init_write_failure(&env, WRITE_FAIL_APPEND);
  env.append_status = cldb_status_corruption("mock.append", "bad");
  env.remove_file_status = cldb_status_io("mock.remove", "ignored");
  status = cldb_write_string_to_file(&env.base, cldb_slice_make("x", 1),
                                     "file");
  CLDB_EXPECT_STATUS(CLDB_STATUS_CORRUPTION, status);
  CLDB_EXPECT_EQ_UINT(1, env.calls[MOCK_REMOVE_FILE]);
  mock_env_cleanup(&env);
}

CLDB_TEST(env_read_file_to_string_success) {
  static const char kBinary[] = {'a', '\0', 'b', 'c'};
  mock_env_t env;
  cldb_buf_t data;

  mock_env_init(&env);
  env.seq_data = kBinary;
  env.seq_size = sizeof(kBinary);
  env.seq_chunk = 2;
  cldb_buf_init(&data);
  CLDB_ASSERT_TRUE(cldb_buf_append(&data, "stale", 5));
  CLDB_EXPECT_OK(cldb_read_file_to_string(&env.base, "file", &data));
  CLDB_EXPECT_EQ_UINT(1, env.calls[MOCK_NEW_SEQ]);
  CLDB_EXPECT_EQ_STR("file", env.last_name);
  CLDB_EXPECT_EQ_UINT(1, env.seq_destroys);
  CLDB_EXPECT_EQ_UINT(3, env.seq_read_calls);
  CLDB_EXPECT_EQ_UINT(sizeof(kBinary), data.size);
  CLDB_EXPECT_EQ_INT(0, memcmp(data.data, kBinary, sizeof(kBinary)));

  cldb_buf_clear(&data);
  env.seq_data = "";
  env.seq_size = 0;
  env.seq_chunk = 0;
  env.calls[MOCK_NEW_SEQ] = 0;
  env.seq_destroys = 0;
  env.seq_read_calls = 0;
  CLDB_EXPECT_OK(cldb_read_file_to_string(&env.base, "empty", &data));
  CLDB_EXPECT_EQ_UINT(1, env.calls[MOCK_NEW_SEQ]);
  CLDB_EXPECT_EQ_UINT(1, env.seq_destroys);
  CLDB_EXPECT_EQ_UINT(1, env.seq_read_calls);
  CLDB_EXPECT_EQ_UINT(0, data.size);
  cldb_buf_free(&data);
  mock_env_cleanup(&env);
}

CLDB_TEST(env_read_file_to_string_failures) {
  mock_env_t env;
  cldb_buf_t data;

  /* 打开失败：清空输出，不创建文件对象。 */
  mock_env_init(&env);
  env.new_sequential_status = cldb_status_not_found("mock.open", "missing");
  cldb_buf_init(&data);
  CLDB_ASSERT_TRUE(cldb_buf_append(&data, "stale", 5));
  CLDB_EXPECT_STATUS(CLDB_STATUS_NOT_FOUND,
                     cldb_read_file_to_string(&env.base, "missing", &data));
  CLDB_EXPECT_EQ_UINT(1, env.calls[MOCK_NEW_SEQ]);
  CLDB_EXPECT_EQ_UINT(0, env.seq_destroys);
  CLDB_EXPECT_EQ_UINT(0, data.size);
  cldb_buf_free(&data);
  mock_env_cleanup(&env);

  /* 读到一半失败：保留已读前缀，销毁文件，返回原错误。 */
  mock_env_init(&env);
  env.seq_data = "abcdef";
  env.seq_size = 6;
  env.seq_chunk = 2;
  env.seq_reads_before_error = 2;
  env.seq_read_status = cldb_status_io("mock.read", "fail");
  cldb_buf_init(&data);
  CLDB_EXPECT_STATUS(CLDB_STATUS_IO_ERROR,
                     cldb_read_file_to_string(&env.base, "file", &data));
  CLDB_EXPECT_EQ_UINT(3, env.seq_read_calls);
  CLDB_EXPECT_EQ_UINT(1, env.seq_destroys);
  CLDB_EXPECT_EQ_UINT(4, data.size);
  CLDB_EXPECT_EQ_INT(0, memcmp(data.data, "abcd", 4));
  cldb_buf_free(&data);
  mock_env_cleanup(&env);
}

CLDB_TEST(env_helpers_reject_null) {
  mock_env_t env;
  cldb_buf_t data;
  cldb_slice_t bad_slice = {NULL, 1};

  mock_env_init(&env);
  cldb_buf_init(&data);
  CLDB_EXPECT_STATUS(
      CLDB_STATUS_INVALID_ARGUMENT,
      cldb_write_string_to_file(NULL, cldb_slice_make("x", 1), "file"));
  CLDB_EXPECT_STATUS(
      CLDB_STATUS_INVALID_ARGUMENT,
      cldb_write_string_to_file(&env.base, cldb_slice_make("x", 1), NULL));
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_write_string_to_file(&env.base, bad_slice, "file"));
  CLDB_EXPECT_STATUS(
      CLDB_STATUS_INVALID_ARGUMENT,
      cldb_write_string_to_file_sync(NULL, cldb_slice_make("x", 1), "file"));
  CLDB_EXPECT_STATUS(
      CLDB_STATUS_INVALID_ARGUMENT,
      cldb_read_file_to_string(NULL, "file", &data));
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_read_file_to_string(&env.base, NULL, &data));
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_read_file_to_string(&env.base, "file", NULL));
  CLDB_EXPECT_EQ_UINT(0, env.calls[MOCK_NEW_WRITABLE]);
  CLDB_EXPECT_EQ_UINT(0, env.calls[MOCK_NEW_SEQ]);
  cldb_buf_free(&data);
  mock_env_cleanup(&env);
}

CLDB_TEST(env_log_varargs_and_null) {
  mock_env_t env;
  cldb_logger_t *logger = NULL;

  mock_env_init(&env);
  CLDB_EXPECT_OK(cldb_env_new_logger(&env.base, "log", &logger));
  CLDB_ASSERT_TRUE(logger != NULL);
  cldb_log(logger, "%s=%d", "x", 7);
  CLDB_EXPECT_EQ_UINT(1, env.logv_calls);
  CLDB_EXPECT_EQ_STR("x=7", env.log_text);

  /* NULL logger 不读取 format/可变参数，也不产生日志调用。 */
  cldb_log(NULL, "ignored %d", 1);
  CLDB_EXPECT_EQ_UINT(1, env.logv_calls);
  cldb_logger_destroy(logger);
  CLDB_EXPECT_EQ_UINT(1, env.logger_destroys);
  cldb_logger_destroy(NULL);
  mock_env_cleanup(&env);
}

CLDB_TEST(env_strvec_basic) {
  cldb_strvec_t vec;
  char source[] = "first";
  char name[16];
  size_t i;

  cldb_strvec_init(&vec);
  CLDB_EXPECT_EQ_UINT(0, cldb_strvec_size(&vec));
  CLDB_EXPECT_EQ_PTR(NULL, cldb_strvec_get(&vec, 0));

  CLDB_EXPECT_TRUE(cldb_strvec_push(&vec, source));
  source[0] = 'X';
  CLDB_EXPECT_TRUE(cldb_strvec_push(&vec, "second"));
  CLDB_EXPECT_TRUE(cldb_strvec_push(&vec, ""));
  CLDB_EXPECT_EQ_UINT(3, cldb_strvec_size(&vec));
  CLDB_EXPECT_EQ_STR("first", cldb_strvec_get(&vec, 0));
  CLDB_EXPECT_EQ_STR("second", cldb_strvec_get(&vec, 1));
  CLDB_EXPECT_EQ_STR("", cldb_strvec_get(&vec, 2));
  CLDB_EXPECT_EQ_PTR(NULL, cldb_strvec_get(&vec, 3));

  for (i = 0; i < 20; ++i) {
    (void)snprintf(name, sizeof(name), "item-%zu", i);
    CLDB_EXPECT_TRUE(cldb_strvec_push(&vec, name));
  }
  CLDB_EXPECT_EQ_UINT(23, cldb_strvec_size(&vec));
  CLDB_EXPECT_EQ_STR("item-0", cldb_strvec_get(&vec, 3));
  CLDB_EXPECT_EQ_STR("item-19", cldb_strvec_get(&vec, 22));

  /* clear 释放元素但保留数组容量，之后可继续复用。 */
  cldb_strvec_clear(&vec);
  CLDB_EXPECT_EQ_UINT(0, cldb_strvec_size(&vec));
  CLDB_EXPECT_TRUE(vec.capacity > 0);
  CLDB_EXPECT_EQ_PTR(NULL, cldb_strvec_get(&vec, 0));
  CLDB_EXPECT_TRUE(cldb_strvec_push(&vec, "again"));
  CLDB_EXPECT_EQ_STR("again", cldb_strvec_get(&vec, 0));

  cldb_strvec_free(&vec);
  CLDB_EXPECT_EQ_PTR(NULL, vec.items);
  CLDB_EXPECT_EQ_UINT(0, vec.size);
  CLDB_EXPECT_EQ_UINT(0, vec.capacity);

  cldb_strvec_init(NULL);
  cldb_strvec_clear(NULL);
  cldb_strvec_free(NULL);
  CLDB_EXPECT_FALSE(cldb_strvec_push(NULL, "x"));
  CLDB_EXPECT_FALSE(cldb_strvec_push(&vec, NULL));
  CLDB_EXPECT_EQ_UINT(0, cldb_strvec_size(NULL));
  CLDB_EXPECT_EQ_PTR(NULL, cldb_strvec_get(NULL, 0));
  CLDB_EXPECT_EQ_PTR(NULL, cldb_strvec_get(&vec, 0));
}

CLDB_TEST(env_destroy_null_safe) {
  cldb_env_destroy(NULL);
  cldb_sequential_file_destroy(NULL);
  cldb_random_access_file_destroy(NULL);
  cldb_writable_file_destroy(NULL);
  cldb_logger_destroy(NULL);
}

void cldb_register_env_tests(void) {
  CLDB_RUN_TEST(env_wrapper_forwards_all);
  CLDB_RUN_TEST(env_optional_and_alias_methods);
  CLDB_RUN_TEST(env_write_string_to_file_success);
  CLDB_RUN_TEST(env_write_string_to_file_failures);
  CLDB_RUN_TEST(env_read_file_to_string_success);
  CLDB_RUN_TEST(env_read_file_to_string_failures);
  CLDB_RUN_TEST(env_helpers_reject_null);
  CLDB_RUN_TEST(env_log_varargs_and_null);
  CLDB_RUN_TEST(env_strvec_basic);
  CLDB_RUN_TEST(env_destroy_null_safe);
}
