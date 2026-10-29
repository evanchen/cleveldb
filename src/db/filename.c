#include "db/filename.h"

#include <stdio.h>
#include <string.h>

#include "port/port.h"
#include "util/logging.h"

/* 把 dbname 与 suffix 拼成新路径并替换 result；suffix 以 '/' 开头。 */
static bool make_name(const char *dbname, const char *suffix,
                      cldb_buf_t *result) {
  cldb_buf_t tmp;
  bool ok;

  if (dbname == NULL || suffix == NULL || result == NULL) {
    return false;
  }
  cldb_buf_init(&tmp);
  ok = cldb_buf_append(&tmp, dbname, strlen(dbname)) &&
       cldb_buf_append(&tmp, suffix, strlen(suffix));
  if (ok) {
    cldb_buf_swap(&tmp, result);
  }
  cldb_buf_free(&tmp);
  return ok;
}

/* 生成 "<dbname>/<至少 6 位 number>.<suffix>"。 */
static bool make_numbered_name(const char *dbname, uint64_t number,
                               const char *suffix, cldb_buf_t *result) {
  char fragment[64];
  int length;

  if (dbname == NULL || suffix == NULL || result == NULL) {
    return false;
  }
  length = snprintf(fragment, sizeof(fragment), "/%06llu.%s",
                    (unsigned long long)number, suffix);
  if (length < 0 || (size_t)length >= sizeof(fragment)) {
    return false;
  }
  return make_name(dbname, fragment, result);
}

bool cldb_log_file_name(const char *dbname, uint64_t number,
                        cldb_buf_t *result) {
  return make_numbered_name(dbname, number, "log", result);
}

bool cldb_table_file_name(const char *dbname, uint64_t number,
                          cldb_buf_t *result) {
  return make_numbered_name(dbname, number, "ldb", result);
}

bool cldb_sst_table_file_name(const char *dbname, uint64_t number,
                              cldb_buf_t *result) {
  return make_numbered_name(dbname, number, "sst", result);
}

bool cldb_descriptor_file_name(const char *dbname, uint64_t number,
                               cldb_buf_t *result) {
  char fragment[64];
  int length;

  if (dbname == NULL || result == NULL) {
    return false;
  }
  length = snprintf(fragment, sizeof(fragment), "/MANIFEST-%06llu",
                    (unsigned long long)number);
  if (length < 0 || (size_t)length >= sizeof(fragment)) {
    return false;
  }
  return make_name(dbname, fragment, result);
}

bool cldb_temp_file_name(const char *dbname, uint64_t number,
                         cldb_buf_t *result) {
  return make_numbered_name(dbname, number, "dbtmp", result);
}

bool cldb_current_file_name(const char *dbname, cldb_buf_t *result) {
  return make_name(dbname, "/CURRENT", result);
}

bool cldb_lock_file_name(const char *dbname, cldb_buf_t *result) {
  return make_name(dbname, "/LOCK", result);
}

bool cldb_info_log_file_name(const char *dbname, cldb_buf_t *result) {
  return make_name(dbname, "/LOG", result);
}

bool cldb_old_info_log_file_name(const char *dbname, cldb_buf_t *result) {
  return make_name(dbname, "/LOG.old", result);
}

/* 比较切片与 C 字符串；切片来自非 NULL 文件名，data 不为 NULL。 */
static bool slice_equals_cstr(cldb_slice_t slice, const char *text) {
  size_t length = strlen(text);

  return slice.size == length && memcmp(slice.data, text, length) == 0;
}

/* 判断切片是否以 C 字符串开头。 */
static bool slice_starts_with_cstr(cldb_slice_t slice, const char *prefix) {
  size_t length = strlen(prefix);

  return slice.size >= length && memcmp(slice.data, prefix, length) == 0;
}

bool cldb_parse_file_name(const char *filename, uint64_t *number,
                          cldb_file_type_t *type) {
  cldb_slice_t rest;
  cldb_file_type_t parsed_type;
  uint64_t parsed_number;

  if (filename == NULL || number == NULL || type == NULL) {
    return false;
  }
  /* 只接受 basename；上游模式本身也匹配不了路径分隔符。 */
  if (strchr(filename, '/') != NULL) {
    return false;
  }
  rest = cldb_slice_make(filename, strlen(filename));

  if (slice_equals_cstr(rest, "CURRENT")) {
    parsed_number = 0;
    parsed_type = CLDB_FILE_TYPE_CURRENT;
  } else if (slice_equals_cstr(rest, "LOCK")) {
    parsed_number = 0;
    parsed_type = CLDB_FILE_TYPE_DB_LOCK;
  } else if (slice_equals_cstr(rest, "LOG") ||
             slice_equals_cstr(rest, "LOG.old")) {
    parsed_number = 0;
    parsed_type = CLDB_FILE_TYPE_INFO_LOG;
  } else if (slice_starts_with_cstr(rest, "MANIFEST-")) {
    cldb_slice_remove_prefix(&rest, sizeof("MANIFEST-") - 1);
    if (!cldb_consume_decimal_number(&rest, &parsed_number) ||
        rest.size != 0) {
      return false;
    }
    parsed_type = CLDB_FILE_TYPE_DESCRIPTOR;
  } else {
    if (!cldb_consume_decimal_number(&rest, &parsed_number)) {
      return false;
    }
    if (slice_equals_cstr(rest, ".log")) {
      parsed_type = CLDB_FILE_TYPE_LOG;
    } else if (slice_equals_cstr(rest, ".sst") ||
               slice_equals_cstr(rest, ".ldb")) {
      parsed_type = CLDB_FILE_TYPE_TABLE;
    } else if (slice_equals_cstr(rest, ".dbtmp")) {
      parsed_type = CLDB_FILE_TYPE_TEMP;
    } else {
      return false;
    }
  }
  *number = parsed_number;
  *type = parsed_type;
  return true;
}

cldb_status_t cldb_set_current_file(cldb_env_t *env, const char *dbname,
                                    uint64_t descriptor_number) {
  cldb_buf_t manifest;
  cldb_buf_t contents;
  cldb_buf_t temp_path;
  cldb_buf_t current_path;
  cldb_status_t status = cldb_status_ok();
  const char *basename;
  size_t dbname_size;

  if (env == NULL || dbname == NULL) {
    return cldb_status_invalid_argument("SetCurrentFile", "invalid argument");
  }
  cldb_buf_init(&manifest);
  cldb_buf_init(&contents);
  cldb_buf_init(&temp_path);
  cldb_buf_init(&current_path);

  if (!cldb_descriptor_file_name(dbname, descriptor_number, &manifest) ||
      !cldb_temp_file_name(dbname, descriptor_number, &temp_path) ||
      !cldb_current_file_name(dbname, &current_path)) {
    status = cldb_status_io("SetCurrentFile", "out of memory");
    goto cleanup;
  }

  dbname_size = strlen(dbname);
  basename = cldb_buf_cstr(&manifest);
  CLDB_ASSERT(strncmp(basename, dbname, dbname_size) == 0 &&
              basename[dbname_size] == '/');
  basename += dbname_size + 1;
  if (!cldb_buf_append(&contents, basename, strlen(basename)) ||
      !cldb_buf_append_char(&contents, '\n')) {
    status = cldb_status_io("SetCurrentFile", "out of memory");
    goto cleanup;
  }

  status = cldb_write_string_to_file_sync(
      env, cldb_buf_slice(&contents), cldb_buf_cstr(&temp_path));
  if (cldb_status_is_ok(status)) {
    status = cldb_env_rename_file(env, cldb_buf_cstr(&temp_path),
                                  cldb_buf_cstr(&current_path));
  }
  if (!cldb_status_is_ok(status)) {
    (void)cldb_env_remove_file(env, cldb_buf_cstr(&temp_path));
  }

cleanup:
  cldb_buf_free(&manifest);
  cldb_buf_free(&contents);
  cldb_buf_free(&temp_path);
  cldb_buf_free(&current_path);
  return status;
}
