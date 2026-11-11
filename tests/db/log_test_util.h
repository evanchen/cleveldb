#ifndef CLDB_TESTS_DB_LOG_TEST_UTIL_H_
#define CLDB_TESTS_DB_LOG_TEST_UTIL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cleveldb/buf.h"
#include "cleveldb/env.h"
#include "cleveldb/slice.h"
#include "cleveldb/status.h"
#include "db/log_format.h"
#include "db/log_writer.h"
#include "harness.h"
#include "util/coding.h"
#include "util/crc32c.h"

/* WAL 测试共用的内存文件、损坏注入与确定性 payload。 */

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

/* 内存 SequentialFile；contents 为尚未读取的字节。force_error 让下一次
 * Read 返回错误；returned_partial 记录已返回过不足一块的数据，用于发现
 * Reader 在 EOF 后重复读取。对象由调用者持有。 */
typedef struct cldb_test_string_source {
  cldb_sequential_file_t base; /* 必须为首成员 */
  cldb_slice_t contents;
  bool force_error;
  bool returned_partial;
} cldb_test_string_source_t;

static inline cldb_status_t cldb_test_string_source_read(
    cldb_sequential_file_t *file, size_t n, cldb_slice_t *result,
    char *scratch) {
  cldb_test_string_source_t *self = (cldb_test_string_source_t *)file;
  size_t count = n;

  CLDB_ASSERT_FALSE(self->returned_partial);
  if (self->force_error) {
    self->force_error = false;
    self->returned_partial = true;
    *result = cldb_slice_empty();
    return cldb_status_corruption("read error", NULL);
  }
  if (self->contents.size < count) {
    count = self->contents.size;
    self->returned_partial = true;
  }
  if (count > 0) {
    memcpy(scratch, self->contents.data, count);
  }
  *result = cldb_slice_make(scratch, count);
  cldb_slice_remove_prefix(&self->contents, count);
  return cldb_status_ok();
}

static inline cldb_status_t cldb_test_string_source_skip(
    cldb_sequential_file_t *file, uint64_t n) {
  cldb_test_string_source_t *self = (cldb_test_string_source_t *)file;
  size_t count = (size_t)n;

  if (n > (uint64_t)self->contents.size) {
    self->contents = cldb_slice_empty();
    return cldb_status_not_found("string_source", "skipped past end");
  }
  cldb_slice_remove_prefix(&self->contents, count);
  return cldb_status_ok();
}

static inline void cldb_test_string_source_destroy(
    cldb_sequential_file_t *file) {
  (void)file;
}

static inline void cldb_test_string_source_reset(
    cldb_test_string_source_t *source, cldb_slice_t contents) {
  source->contents = contents;
  source->force_error = false;
  source->returned_partial = false;
}

static inline void cldb_test_string_source_init(
    cldb_test_string_source_t *source) {
  static const cldb_sequential_file_vtbl_t kVtbl = {
      cldb_test_string_source_read, cldb_test_string_source_skip,
      cldb_test_string_source_destroy,
  };

  memset(source, 0, sizeof(*source));
  source->base.vtbl = &kVtbl;
  cldb_test_string_source_reset(source, cldb_slice_empty());
}

/* 收集 Reader 的上报；message 是各次状态文本的拼接。 */
typedef struct cldb_test_report {
  uint64_t dropped_bytes;
  char message[4096];
  size_t message_size;
} cldb_test_report_t;

static inline void cldb_test_report_callback(void *arg, uint64_t bytes,
                                             cldb_status_t status) {
  cldb_test_report_t *report = (cldb_test_report_t *)arg;
  char text[512];
  size_t length = cldb_status_to_string(status, text, sizeof(text));

  report->dropped_bytes += bytes;
  if (length > sizeof(text) - 1) {
    length = sizeof(text) - 1;
  }
  if (length <= sizeof(report->message) - 1 - report->message_size) {
    memcpy(report->message + report->message_size, text, length);
    report->message_size += length;
    report->message[report->message_size] = '\0';
  }
}

static inline void cldb_test_report_reset(cldb_test_report_t *report) {
  memset(report, 0, sizeof(*report));
}

static inline bool cldb_test_report_matches(const cldb_test_report_t *report,
                                            const char *text) {
  return strstr(report->message, text) != NULL;
}

/* 以下辅助直接改写物理字节，用于构造损坏输入。 */
static inline void cldb_test_increment_byte(cldb_buf_t *data, size_t offset,
                                            unsigned int delta) {
  uint8_t value = (uint8_t)data->data[offset];

  data->data[offset] = (char)(uint8_t)(value + delta);
}

static inline void cldb_test_set_byte(cldb_buf_t *data, size_t offset,
                                      uint8_t value) {
  data->data[offset] = (char)value;
}

static inline void cldb_test_flip_bit(cldb_buf_t *data, size_t offset,
                                      unsigned int bit) {
  unsigned char mask;

  CLDB_ASSERT_TRUE(offset < data->size);
  CLDB_ASSERT_TRUE(bit < 8);
  mask = (unsigned char)(1u << bit);
  data->data[offset] =
      (char)((unsigned char)data->data[offset] ^ mask);
}

/* 按读取器的算法重算 CRC：覆盖 type 字节 + payload。 */
static inline void cldb_test_fix_crc(cldb_buf_t *data, size_t header_offset,
                                     size_t length) {
  uint32_t crc = cldb_crc32c_mask(cldb_crc32c_value(
      data->data + header_offset + CLDB_LOG_TYPE_OFFSET, length + 1));

  cldb_encode_fixed32(data->data + header_offset + CLDB_LOG_CRC_OFFSET, crc);
}

static inline void cldb_test_shrink(cldb_buf_t *data, size_t count) {
  CLDB_ASSERT_TRUE(count <= data->size);
  CLDB_ASSERT_TRUE(cldb_buf_resize(data, data->size - count));
}

static inline void cldb_test_wipe(cldb_buf_t *data, size_t begin, size_t end,
                                  char value) {
  CLDB_ASSERT_TRUE(begin <= end);
  CLDB_ASSERT_TRUE(end <= data->size);
  memset(data->data + begin, (unsigned char)value, end - begin);
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
