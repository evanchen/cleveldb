#include "cleveldb/env.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ---------- 字符串数组 ---------- */

void cldb_strvec_init(cldb_strvec_t *vec) {
  if (vec == NULL) {
    return;
  }
  vec->items = NULL;
  vec->size = 0;
  vec->capacity = 0;
}

void cldb_strvec_clear(cldb_strvec_t *vec) {
  size_t i;

  if (vec == NULL) {
    return;
  }
  for (i = 0; i < vec->size; ++i) {
    free(vec->items[i]);
  }
  vec->size = 0;
}

void cldb_strvec_free(cldb_strvec_t *vec) {
  if (vec == NULL) {
    return;
  }
  cldb_strvec_clear(vec);
  free(vec->items);
  cldb_strvec_init(vec);
}

bool cldb_strvec_push(cldb_strvec_t *vec, const char *value) {
  char *copy;
  size_t length;
  size_t new_capacity;
  char **new_items;

  if (vec == NULL || value == NULL) {
    return false;
  }
  length = strlen(value);
  if (length == SIZE_MAX) {
    return false;
  }
  copy = malloc(length + 1);
  if (copy == NULL) {
    return false;
  }
  memcpy(copy, value, length + 1);

  if (vec->size == vec->capacity) {
    new_capacity = vec->capacity == 0 ? 8 : vec->capacity;
    if (new_capacity > SIZE_MAX / 2) {
      free(copy);
      return false;
    }
    new_capacity *= 2;
    if (new_capacity > SIZE_MAX / sizeof(*vec->items)) {
      free(copy);
      return false;
    }
    new_items = realloc(vec->items, new_capacity * sizeof(*vec->items));
    if (new_items == NULL) {
      free(copy);
      return false;
    }
    vec->items = new_items;
    vec->capacity = new_capacity;
  }
  vec->items[vec->size] = copy;
  vec->size++;
  return true;
}

size_t cldb_strvec_size(const cldb_strvec_t *vec) {
  return vec != NULL ? vec->size : 0;
}

const char *cldb_strvec_get(const cldb_strvec_t *vec, size_t index) {
  if (vec == NULL || index >= vec->size) {
    return NULL;
  }
  return vec->items[index];
}

/* ---------- 对象销毁 ---------- */

void cldb_env_destroy(cldb_env_t *env) {
  if (env != NULL && env->vtbl->destroy != NULL) {
    env->vtbl->destroy(env);
  }
}

void cldb_sequential_file_destroy(cldb_sequential_file_t *file) {
  if (file != NULL) {
    file->vtbl->destroy(file);
  }
}

void cldb_random_access_file_destroy(cldb_random_access_file_t *file) {
  if (file != NULL) {
    file->vtbl->destroy(file);
  }
}

void cldb_writable_file_destroy(cldb_writable_file_t *file) {
  if (file != NULL) {
    file->vtbl->destroy(file);
  }
}

void cldb_logger_destroy(cldb_logger_t *logger) {
  if (logger != NULL) {
    logger->vtbl->destroy(logger);
  }
}

/* ---------- 日志与文件辅助 ---------- */

void cldb_log(cldb_logger_t *logger, const char *format, ...) {
  va_list ap;

  if (logger == NULL || format == NULL) {
    return;
  }
  va_start(ap, format);
  cldb_logger_logv(logger, format, ap);
  va_end(ap);
}

/* 按上游 DoWriteStringToFile：创建、追加、可选 Sync、Close；任何失败都尽力
 * 删除残留文件，但保留原始错误。 */
static cldb_status_t write_string_to_file(cldb_env_t *env, cldb_slice_t data,
                                          const char *fname, bool should_sync) {
  cldb_writable_file_t *file = NULL;
  cldb_status_t status;

  status = cldb_env_new_writable_file(env, fname, &file);
  if (!cldb_status_is_ok(status)) {
    return status;
  }
  status = cldb_writable_file_append(file, data);
  if (cldb_status_is_ok(status) && should_sync) {
    status = cldb_writable_file_sync(file);
  }
  if (cldb_status_is_ok(status)) {
    status = cldb_writable_file_close(file);
  }
  cldb_writable_file_destroy(file);
  if (!cldb_status_is_ok(status)) {
    (void)cldb_env_remove_file(env, fname);
  }
  return status;
}

cldb_status_t cldb_write_string_to_file(cldb_env_t *env, cldb_slice_t data,
                                        const char *fname) {
  if (env == NULL || fname == NULL ||
      (data.size > 0 && data.data == NULL)) {
    return cldb_status_invalid_argument("WriteStringToFile",
                                        "invalid argument");
  }
  return write_string_to_file(env, data, fname, false);
}

cldb_status_t cldb_write_string_to_file_sync(cldb_env_t *env, cldb_slice_t data,
                                             const char *fname) {
  if (env == NULL || fname == NULL ||
      (data.size > 0 && data.data == NULL)) {
    return cldb_status_invalid_argument("WriteStringToFileSync",
                                        "invalid argument");
  }
  return write_string_to_file(env, data, fname, true);
}

cldb_status_t cldb_read_file_to_string(cldb_env_t *env, const char *fname,
                                       cldb_buf_t *data) {
  enum { kBufferSize = 8192 };
  char scratch[kBufferSize];
  cldb_sequential_file_t *file = NULL;
  cldb_status_t status;

  if (env == NULL || fname == NULL || data == NULL) {
    return cldb_status_invalid_argument("ReadFileToString",
                                        "invalid argument");
  }
  cldb_buf_clear(data);
  status = cldb_env_new_sequential_file(env, fname, &file);
  if (!cldb_status_is_ok(status)) {
    return status;
  }
  for (;;) {
    cldb_slice_t fragment;

    status = cldb_sequential_file_read(file, sizeof(scratch), &fragment,
                                       scratch);
    if (!cldb_status_is_ok(status)) {
      break;
    }
    if (!cldb_buf_append(data, fragment.data, fragment.size)) {
      status = cldb_status_io("ReadFileToString", "out of memory");
      break;
    }
    if (fragment.size == 0) {
      break;
    }
  }
  cldb_sequential_file_destroy(file);
  return status;
}

/* ---------- EnvWrapper 转发 ---------- */

static cldb_status_t wrapper_new_sequential_file(
    cldb_env_t *env, const char *fname, cldb_sequential_file_t **result) {
  cldb_env_wrapper_t *wrapper = (cldb_env_wrapper_t *)env;

  return cldb_env_new_sequential_file(wrapper->target, fname, result);
}

static cldb_status_t wrapper_new_random_access_file(
    cldb_env_t *env, const char *fname, cldb_random_access_file_t **result) {
  cldb_env_wrapper_t *wrapper = (cldb_env_wrapper_t *)env;

  return cldb_env_new_random_access_file(wrapper->target, fname, result);
}

static cldb_status_t wrapper_new_writable_file(
    cldb_env_t *env, const char *fname, cldb_writable_file_t **result) {
  cldb_env_wrapper_t *wrapper = (cldb_env_wrapper_t *)env;

  return cldb_env_new_writable_file(wrapper->target, fname, result);
}

static cldb_status_t wrapper_new_appendable_file(
    cldb_env_t *env, const char *fname, cldb_writable_file_t **result) {
  cldb_env_wrapper_t *wrapper = (cldb_env_wrapper_t *)env;

  return cldb_env_new_appendable_file(wrapper->target, fname, result);
}

static bool wrapper_file_exists(cldb_env_t *env, const char *fname) {
  cldb_env_wrapper_t *wrapper = (cldb_env_wrapper_t *)env;

  return cldb_env_file_exists(wrapper->target, fname);
}

static cldb_status_t wrapper_get_children(cldb_env_t *env, const char *dir,
                                          cldb_strvec_t *result) {
  cldb_env_wrapper_t *wrapper = (cldb_env_wrapper_t *)env;

  return cldb_env_get_children(wrapper->target, dir, result);
}

static cldb_status_t wrapper_remove_file(cldb_env_t *env, const char *fname) {
  cldb_env_wrapper_t *wrapper = (cldb_env_wrapper_t *)env;

  return cldb_env_remove_file(wrapper->target, fname);
}

static cldb_status_t wrapper_delete_file(cldb_env_t *env, const char *fname) {
  cldb_env_wrapper_t *wrapper = (cldb_env_wrapper_t *)env;

  return cldb_env_delete_file(wrapper->target, fname);
}

static cldb_status_t wrapper_create_dir(cldb_env_t *env,
                                        const char *dirname) {
  cldb_env_wrapper_t *wrapper = (cldb_env_wrapper_t *)env;

  return cldb_env_create_dir(wrapper->target, dirname);
}

static cldb_status_t wrapper_remove_dir(cldb_env_t *env,
                                        const char *dirname) {
  cldb_env_wrapper_t *wrapper = (cldb_env_wrapper_t *)env;

  return cldb_env_remove_dir(wrapper->target, dirname);
}

static cldb_status_t wrapper_delete_dir(cldb_env_t *env,
                                        const char *dirname) {
  cldb_env_wrapper_t *wrapper = (cldb_env_wrapper_t *)env;

  return cldb_env_delete_dir(wrapper->target, dirname);
}

static cldb_status_t wrapper_get_file_size(cldb_env_t *env, const char *fname,
                                           uint64_t *size) {
  cldb_env_wrapper_t *wrapper = (cldb_env_wrapper_t *)env;

  return cldb_env_get_file_size(wrapper->target, fname, size);
}

static cldb_status_t wrapper_rename_file(cldb_env_t *env, const char *src,
                                         const char *target) {
  cldb_env_wrapper_t *wrapper = (cldb_env_wrapper_t *)env;

  return cldb_env_rename_file(wrapper->target, src, target);
}

static cldb_status_t wrapper_lock_file(cldb_env_t *env, const char *fname,
                                       cldb_file_lock_t **lock) {
  cldb_env_wrapper_t *wrapper = (cldb_env_wrapper_t *)env;

  return cldb_env_lock_file(wrapper->target, fname, lock);
}

static cldb_status_t wrapper_unlock_file(cldb_env_t *env,
                                         cldb_file_lock_t *lock) {
  cldb_env_wrapper_t *wrapper = (cldb_env_wrapper_t *)env;

  return cldb_env_unlock_file(wrapper->target, lock);
}

static void wrapper_schedule(cldb_env_t *env, void (*fn)(void *), void *arg) {
  cldb_env_wrapper_t *wrapper = (cldb_env_wrapper_t *)env;

  cldb_env_schedule(wrapper->target, fn, arg);
}

static void wrapper_start_thread(cldb_env_t *env, void (*fn)(void *),
                                 void *arg) {
  cldb_env_wrapper_t *wrapper = (cldb_env_wrapper_t *)env;

  cldb_env_start_thread(wrapper->target, fn, arg);
}

static cldb_status_t wrapper_get_test_directory(cldb_env_t *env,
                                                cldb_buf_t *path) {
  cldb_env_wrapper_t *wrapper = (cldb_env_wrapper_t *)env;

  return cldb_env_get_test_directory(wrapper->target, path);
}

static cldb_status_t wrapper_new_logger(cldb_env_t *env, const char *fname,
                                        cldb_logger_t **result) {
  cldb_env_wrapper_t *wrapper = (cldb_env_wrapper_t *)env;

  return cldb_env_new_logger(wrapper->target, fname, result);
}

static uint64_t wrapper_now_micros(cldb_env_t *env) {
  cldb_env_wrapper_t *wrapper = (cldb_env_wrapper_t *)env;

  return cldb_env_now_micros(wrapper->target);
}

static void wrapper_sleep_for_microseconds(cldb_env_t *env, int micros) {
  cldb_env_wrapper_t *wrapper = (cldb_env_wrapper_t *)env;

  cldb_env_sleep_for_microseconds(wrapper->target, micros);
}

static const cldb_env_vtbl_t kEnvWrapperVtbl = {
    .new_sequential_file = wrapper_new_sequential_file,
    .new_random_access_file = wrapper_new_random_access_file,
    .new_writable_file = wrapper_new_writable_file,
    .new_appendable_file = wrapper_new_appendable_file,
    .file_exists = wrapper_file_exists,
    .get_children = wrapper_get_children,
    .remove_file = wrapper_remove_file,
    .delete_file = wrapper_delete_file,
    .create_dir = wrapper_create_dir,
    .remove_dir = wrapper_remove_dir,
    .delete_dir = wrapper_delete_dir,
    .get_file_size = wrapper_get_file_size,
    .rename_file = wrapper_rename_file,
    .lock_file = wrapper_lock_file,
    .unlock_file = wrapper_unlock_file,
    .schedule = wrapper_schedule,
    .start_thread = wrapper_start_thread,
    .get_test_directory = wrapper_get_test_directory,
    .new_logger = wrapper_new_logger,
    .now_micros = wrapper_now_micros,
    .sleep_for_microseconds = wrapper_sleep_for_microseconds,
    .destroy = NULL,
};

void cldb_env_wrapper_init(cldb_env_wrapper_t *wrapper, cldb_env_t *target) {
  if (wrapper == NULL) {
    return;
  }
  wrapper->base.vtbl = &kEnvWrapperVtbl;
  wrapper->target = target;
}
