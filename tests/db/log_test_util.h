#ifndef CLDB_TESTS_DB_LOG_TEST_UTIL_H_
#define CLDB_TESTS_DB_LOG_TEST_UTIL_H_

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "cleveldb/buf.h"
#include "cleveldb/env.h"
#include "cleveldb/slice.h"
#include "cleveldb/status.h"
#include "db/log_writer.h"

/* WAL 测试共用的内存 WritableFile 与确定性 payload。 */

/* 捕获追加内容；可注入 Append/Flush 失败。对象由调用者持有，destroy 回调
 * 不释放。 */
typedef struct cldb_test_string_file {
  cldb_writable_file_t base; /* 必须为首成员 */
  cldb_buf_t data;
  int append_calls;
  int flush_calls;
  int fail_append_call; /* 1 起始；0 表示不注入 */
  int fail_flush_call;
} cldb_test_string_file_t;

static inline cldb_status_t cldb_test_string_file_append(
    cldb_writable_file_t *file, cldb_slice_t data) {
  cldb_test_string_file_t *self = (cldb_test_string_file_t *)file;

  self->append_calls += 1;
  if (self->fail_append_call > 0 &&
      self->append_calls == self->fail_append_call) {
    return cldb_status_io("string_file", "injected append error");
  }
  if (!cldb_buf_append_slice(&self->data, data)) {
    return cldb_status_io("string_file", "out of memory");
  }
  return cldb_status_ok();
}

static inline cldb_status_t cldb_test_string_file_close(
    cldb_writable_file_t *file) {
  (void)file;
  return cldb_status_ok();
}

static inline cldb_status_t cldb_test_string_file_flush(
    cldb_writable_file_t *file) {
  cldb_test_string_file_t *self = (cldb_test_string_file_t *)file;

  self->flush_calls += 1;
  if (self->fail_flush_call > 0 &&
      self->flush_calls == self->fail_flush_call) {
    return cldb_status_io("string_file", "injected flush error");
  }
  return cldb_status_ok();
}

static inline cldb_status_t cldb_test_string_file_sync(
    cldb_writable_file_t *file) {
  (void)file;
  return cldb_status_ok();
}

static inline void cldb_test_string_file_destroy(
    cldb_writable_file_t *file) {
  (void)file;
}

static inline void cldb_test_string_file_init(
    cldb_test_string_file_t *file) {
  static const cldb_writable_file_vtbl_t kVtbl = {
      cldb_test_string_file_append, cldb_test_string_file_close,
      cldb_test_string_file_flush, cldb_test_string_file_sync,
      cldb_test_string_file_destroy,
  };

  memset(file, 0, sizeof(*file));
  file->base.vtbl = &kVtbl;
  cldb_buf_init(&file->data);
}

static inline void cldb_test_string_file_free(cldb_test_string_file_t *file) {
  cldb_buf_free(&file->data);
}

/* 确定性的 payload 字节，便于检查分片拼接结果。 */
static inline char cldb_test_pattern_byte(size_t index) {
  return (char)(unsigned char)((index * 31u + 7u) % 251u);
}

static inline void cldb_test_fill_pattern(char *data, size_t size) {
  size_t i;

  for (i = 0; i < size; ++i) {
    data[i] = cldb_test_pattern_byte(i);
  }
}

/* 写入一条 pattern payload；失败返回非 OK，临时内存总会被释放。 */
static inline cldb_status_t cldb_test_log_writer_add_pattern(
    cldb_log_writer_t *writer, size_t length) {
  char *data = NULL;
  cldb_status_t status;

  if (length > 0) {
    data = malloc(length);
    if (data == NULL) {
      return cldb_status_io("test", "out of memory");
    }
    cldb_test_fill_pattern(data, length);
  }
  status = cldb_log_writer_add_record(writer, cldb_slice_make(data, length));
  free(data);
  return status;
}

#endif /* CLDB_TESTS_DB_LOG_TEST_UTIL_H_ */
