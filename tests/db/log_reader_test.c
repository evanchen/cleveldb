#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "db/log_format.h"
#include "db/log_reader.h"
#include "db/log_test_util.h"
#include "db/log_writer.h"
#include "harness.h"
#include "status_test_util.h"
#include "util/coding.h"
#include "util/crc32c.h"
#include "util/random.h"

/* WAL 读取器；对应上游 db/log_test.cc 的读取与损坏用例。 */

typedef struct string_source {
  cldb_sequential_file_t base; /* 必须为首成员 */
  cldb_slice_t contents;       /* 尚未读取的字节 */
  bool force_error;            /* 下次 Read 返回错误 */
  bool returned_partial;       /* 已读过不足一块的数据 */
} string_source_t;

static cldb_status_t source_read(cldb_sequential_file_t *file, size_t n,
                                 cldb_slice_t *result, char *scratch) {
  string_source_t *self = (string_source_t *)file;
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

static cldb_status_t source_skip(cldb_sequential_file_t *file, uint64_t n) {
  string_source_t *self = (string_source_t *)file;
  size_t count = (size_t)n;

  if (n > (uint64_t)self->contents.size) {
    self->contents = cldb_slice_empty();
    return cldb_status_not_found("string_source", "skipped past end");
  }
  cldb_slice_remove_prefix(&self->contents, count);
  return cldb_status_ok();
}

static void source_destroy(cldb_sequential_file_t *file) { (void)file; }

static const cldb_sequential_file_vtbl_t kSourceVtbl = {
    source_read, source_skip, source_destroy,
};

typedef struct report_collector {
  uint64_t dropped_bytes;
  char message[4096];
  size_t message_size;
} report_collector_t;

static void report_collector_add(void *arg, uint64_t bytes,
                                 cldb_status_t status) {
  report_collector_t *collector = (report_collector_t *)arg;
  char text[512];
  size_t length = cldb_status_to_string(status, text, sizeof(text));

  collector->dropped_bytes += bytes;
  if (length > sizeof(text) - 1) {
    length = sizeof(text) - 1;
  }
  if (length <= sizeof(collector->message) - 1 - collector->message_size) {
    memcpy(collector->message + collector->message_size, text, length);
    collector->message_size += length;
    collector->message[collector->message_size] = '\0';
  }
}

static void report_collector_reset(report_collector_t *collector) {
  memset(collector, 0, sizeof(*collector));
}

static bool match_error(const report_collector_t *collector,
                        const char *text) {
  return strstr(collector->message, text) != NULL;
}

typedef struct log_fixture {
  cldb_test_string_file_t dest;
  cldb_log_writer_t *writer;
  string_source_t source;
  report_collector_t report;
  cldb_log_reader_t *reader;
} log_fixture_t;

static void fixture_init(log_fixture_t *f) {
  memset(f, 0, sizeof(*f));
  cldb_test_string_file_init(&f->dest);
  f->source.base.vtbl = &kSourceVtbl;
  f->source.contents = cldb_slice_empty();
  report_collector_reset(&f->report);
}

static void fixture_cleanup(log_fixture_t *f) {
  cldb_log_reader_destroy(f->reader);
  cldb_log_writer_destroy(f->writer);
  cldb_test_string_file_free(&f->dest);
}

static void fixture_write(log_fixture_t *f, const char *data, size_t size) {
  if (f->writer == NULL) {
    CLDB_ASSERT_OK(cldb_log_writer_create(&f->dest.base, &f->writer));
  }
  CLDB_ASSERT_OK(
      cldb_log_writer_add_record(f->writer, cldb_slice_make(data, size)));
}

static void fixture_write_pattern(log_fixture_t *f, size_t length) {
  char *data = NULL;

  if (length > 0) {
    data = malloc(length);
    if (data == NULL) {
      CLDB_EXPECT_TRUE(false);
      return;
    }
    cldb_test_fill_pattern(data, length);
  }
  fixture_write(f, data, length);
  free(data);
}

static void fixture_reopen_writer(log_fixture_t *f) {
  CLDB_ASSERT_TRUE(f->writer != NULL);
  cldb_log_writer_destroy(f->writer);
  f->writer = NULL;
  CLDB_ASSERT_OK(cldb_log_writer_create_with_length(
      &f->dest.base, (uint64_t)f->dest.data.size, &f->writer));
}

static void fixture_start_reading(log_fixture_t *f, bool checksum,
                                  uint64_t initial_offset) {
  cldb_log_writer_destroy(f->writer);
  f->writer = NULL;
  cldb_log_reader_destroy(f->reader);
  f->reader = NULL;
  report_collector_reset(&f->report);
  f->source.contents = cldb_buf_slice(&f->dest.data);
  f->source.force_error = false;
  f->source.returned_partial = false;
  CLDB_ASSERT_OK(cldb_log_reader_create(
      &f->source.base, report_collector_add, &f->report, checksum,
      initial_offset, &f->reader));
}

static void fixture_expect_record(log_fixture_t *f, const void *expected,
                                  size_t size) {
  cldb_buf_t scratch;
  cldb_slice_t record;
  bool ok;

  cldb_buf_init(&scratch);
  ok = cldb_log_reader_read_record(f->reader, &record, &scratch);
  CLDB_EXPECT_TRUE(ok);
  if (ok) {
    CLDB_EXPECT_EQ_UINT(size, record.size);
    if (record.size == size && size > 0) {
      CLDB_EXPECT_EQ_INT(0, memcmp(record.data, expected, size));
    }
  }
  cldb_buf_free(&scratch);
}

static void fixture_expect_pattern(log_fixture_t *f, size_t length) {
  char *expected = NULL;
  cldb_buf_t scratch;
  cldb_slice_t record;
  bool ok;

  if (length > 0) {
    expected = malloc(length);
    if (expected == NULL) {
      CLDB_EXPECT_TRUE(false);
      return;
    }
    cldb_test_fill_pattern(expected, length);
  }
  cldb_buf_init(&scratch);
  ok = cldb_log_reader_read_record(f->reader, &record, &scratch);
  CLDB_EXPECT_TRUE(ok);
  if (ok) {
    CLDB_EXPECT_EQ_UINT(length, record.size);
    if (record.size == length && length > 0) {
      CLDB_EXPECT_EQ_INT(0, memcmp(record.data, expected, length));
    }
  }
  cldb_buf_free(&scratch);
  free(expected);
}

static void fixture_expect_eof(log_fixture_t *f) {
  cldb_buf_t scratch;
  cldb_slice_t record;
  bool ok;

  cldb_buf_init(&scratch);
  ok = cldb_log_reader_read_record(f->reader, &record, &scratch);
  CLDB_EXPECT_FALSE(ok);
  cldb_buf_free(&scratch);
}

static void increment_byte(cldb_buf_t *data, size_t offset,
                           unsigned int delta) {
  uint8_t value = (uint8_t)data->data[offset];

  data->data[offset] = (char)(uint8_t)(value + delta);
}

static void set_byte(cldb_buf_t *data, size_t offset, uint8_t value) {
  data->data[offset] = (char)value;
}

/* 按读取器的算法重算 CRC：覆盖 type 字节 + payload。 */
static void fix_checksum(cldb_buf_t *data, size_t header_offset,
                         size_t length) {
  uint32_t crc = cldb_crc32c_mask(cldb_crc32c_value(
      data->data + header_offset + CLDB_LOG_TYPE_OFFSET, length + 1));

  cldb_encode_fixed32(data->data + header_offset + CLDB_LOG_CRC_OFFSET, crc);
}

static void shrink(cldb_buf_t *data, size_t count) {
  CLDB_ASSERT_TRUE(count <= data->size);
  CLDB_ASSERT_TRUE(cldb_buf_resize(data, data->size - count));
}

static void wipe(cldb_buf_t *data, size_t begin, size_t end, char value) {
  CLDB_ASSERT_TRUE(begin <= end);
  CLDB_ASSERT_TRUE(end <= data->size);
  memset(data->data + begin, (unsigned char)value, end - begin);
}

static void make_skewed_string(uint32_t index, cldb_random_t *rnd,
                               cldb_buf_t *out) {
  char prefix[32];
  int prefix_length = snprintf(prefix, sizeof(prefix), "%u.", index);
  uint32_t length;

  CLDB_ASSERT_TRUE(prefix_length > 0);
  cldb_buf_clear(out);
  length = cldb_random_skewed(rnd, 17);
  while (out->size < (size_t)length) {
    size_t remaining = (size_t)length - out->size;
    size_t chunk = remaining < (size_t)prefix_length
                       ? remaining
                       : (size_t)prefix_length;
    CLDB_ASSERT_TRUE(cldb_buf_append(out, prefix, chunk));
  }
  CLDB_ASSERT_TRUE(cldb_buf_resize(out, (size_t)length));
}

CLDB_TEST(log_reader_create_validation) {
  log_fixture_t f;
  cldb_slice_t record;
  cldb_buf_t scratch;

  fixture_init(&f);
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_log_reader_create(NULL, NULL, NULL, true, 0,
                                            &f.reader));
  CLDB_EXPECT_EQ_PTR(NULL, f.reader);
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_log_reader_create(&f.source.base, NULL, NULL, true,
                                            0, NULL));
  CLDB_EXPECT_EQ_UINT(0, cldb_log_reader_last_record_offset(NULL));

  cldb_buf_init(&scratch);
  CLDB_EXPECT_FALSE(cldb_log_reader_read_record(NULL, &record, &scratch));
  CLDB_ASSERT_OK(cldb_log_reader_create(&f.source.base, NULL, NULL, true, 0,
                                        &f.reader));
  CLDB_EXPECT_FALSE(cldb_log_reader_read_record(f.reader, NULL, &scratch));
  CLDB_EXPECT_FALSE(cldb_log_reader_read_record(f.reader, &record, NULL));
  fixture_expect_eof(&f);
  cldb_buf_free(&scratch);
  fixture_cleanup(&f);
}

CLDB_TEST(log_reader_empty) {
  log_fixture_t f;

  fixture_init(&f);
  fixture_start_reading(&f, true, 0);
  fixture_expect_eof(&f);
  CLDB_EXPECT_EQ_UINT(0, f.report.dropped_bytes);
  CLDB_EXPECT_EQ_STR("", f.report.message);
  fixture_cleanup(&f);
}

CLDB_TEST(log_reader_read_write) {
  log_fixture_t f;

  fixture_init(&f);
  fixture_write(&f, "foo", 3);
  fixture_write(&f, "bar", 3);
  fixture_write(&f, "", 0);
  fixture_write(&f, "xxxx", 4);
  fixture_start_reading(&f, true, 0);

  fixture_expect_record(&f, "foo", 3);
  fixture_expect_record(&f, "bar", 3);
  fixture_expect_record(&f, "", 0);
  fixture_expect_record(&f, "xxxx", 4);
  fixture_expect_eof(&f);
  fixture_expect_eof(&f);
  CLDB_EXPECT_EQ_UINT(0, f.report.dropped_bytes);
  CLDB_EXPECT_EQ_STR("", f.report.message);
  fixture_cleanup(&f);
}

CLDB_TEST(log_reader_many_blocks) {
  log_fixture_t f;
  char expected[32];
  uint32_t i;

  fixture_init(&f);
  for (i = 0; i < 100000; ++i) {
    int length = snprintf(expected, sizeof(expected), "%u.", i);

    CLDB_ASSERT_TRUE(length > 0);
    fixture_write(&f, expected, (size_t)length);
  }
  fixture_start_reading(&f, true, 0);
  for (i = 0; i < 100000; ++i) {
    int length = snprintf(expected, sizeof(expected), "%u.", i);

    CLDB_ASSERT_TRUE(length > 0);
    fixture_expect_record(&f, expected, (size_t)length);
  }
  fixture_expect_eof(&f);
  CLDB_EXPECT_EQ_UINT(0, f.report.dropped_bytes);
  CLDB_EXPECT_EQ_STR("", f.report.message);
  fixture_cleanup(&f);
}

CLDB_TEST(log_reader_fragmentation) {
  log_fixture_t f;

  fixture_init(&f);
  fixture_write(&f, "small", 5);
  fixture_write_pattern(&f, 50000);
  fixture_write_pattern(&f, 100000);
  fixture_start_reading(&f, true, 0);

  fixture_expect_record(&f, "small", 5);
  fixture_expect_pattern(&f, 50000);
  fixture_expect_pattern(&f, 100000);
  fixture_expect_eof(&f);
  CLDB_EXPECT_EQ_UINT(0, f.report.dropped_bytes);
  CLDB_EXPECT_EQ_STR("", f.report.message);
  fixture_cleanup(&f);
}

CLDB_TEST(log_reader_marginal_trailer) {
  log_fixture_t f;
  size_t first_length =
      (size_t)CLDB_LOG_BLOCK_SIZE - 2 * (size_t)CLDB_LOG_HEADER_SIZE;

  fixture_init(&f);
  fixture_write_pattern(&f, first_length);
  fixture_write(&f, "", 0);
  fixture_write(&f, "bar", 3);
  fixture_start_reading(&f, true, 0);

  fixture_expect_pattern(&f, first_length);
  fixture_expect_record(&f, "", 0);
  fixture_expect_record(&f, "bar", 3);
  fixture_expect_eof(&f);
  CLDB_EXPECT_EQ_UINT(0, f.report.dropped_bytes);
  fixture_cleanup(&f);
}

CLDB_TEST(log_reader_marginal_trailer2) {
  log_fixture_t f;
  size_t first_length =
      (size_t)CLDB_LOG_BLOCK_SIZE - 2 * (size_t)CLDB_LOG_HEADER_SIZE;

  fixture_init(&f);
  fixture_write_pattern(&f, first_length);
  fixture_write(&f, "bar", 3);
  fixture_start_reading(&f, true, 0);

  fixture_expect_pattern(&f, first_length);
  fixture_expect_record(&f, "bar", 3);
  fixture_expect_eof(&f);
  CLDB_EXPECT_EQ_UINT(0, f.report.dropped_bytes);
  fixture_cleanup(&f);
}

CLDB_TEST(log_reader_short_trailer) {
  log_fixture_t f;
  size_t first_length =
      (size_t)CLDB_LOG_BLOCK_SIZE - 2 * (size_t)CLDB_LOG_HEADER_SIZE + 4;

  fixture_init(&f);
  fixture_write_pattern(&f, first_length);
  fixture_write(&f, "", 0);
  fixture_write(&f, "bar", 3);
  fixture_start_reading(&f, true, 0);

  fixture_expect_pattern(&f, first_length);
  fixture_expect_record(&f, "", 0);
  fixture_expect_record(&f, "bar", 3);
  fixture_expect_eof(&f);
  CLDB_EXPECT_EQ_UINT(0, f.report.dropped_bytes);
  fixture_cleanup(&f);
}

CLDB_TEST(log_reader_aligned_eof) {
  log_fixture_t f;
  size_t first_length =
      (size_t)CLDB_LOG_BLOCK_SIZE - 2 * (size_t)CLDB_LOG_HEADER_SIZE + 4;

  fixture_init(&f);
  fixture_write_pattern(&f, first_length);
  fixture_start_reading(&f, true, 0);

  fixture_expect_pattern(&f, first_length);
  fixture_expect_eof(&f);
  CLDB_EXPECT_EQ_UINT(0, f.report.dropped_bytes);
  fixture_cleanup(&f);
}

CLDB_TEST(log_reader_open_for_append) {
  log_fixture_t f;

  fixture_init(&f);
  fixture_write(&f, "hello", 5);
  fixture_reopen_writer(&f);
  fixture_write(&f, "world", 5);
  fixture_start_reading(&f, true, 0);

  fixture_expect_record(&f, "hello", 5);
  fixture_expect_record(&f, "world", 5);
  fixture_expect_eof(&f);
  CLDB_EXPECT_EQ_UINT(0, f.report.dropped_bytes);
  fixture_cleanup(&f);
}

CLDB_TEST(log_reader_random_skewed) {
  log_fixture_t f;
  cldb_random_t write_rnd;
  cldb_random_t read_rnd;
  cldb_buf_t expected;
  uint32_t i;

  fixture_init(&f);
  cldb_random_init(&write_rnd, 301);
  cldb_random_init(&read_rnd, 301);
  cldb_buf_init(&expected);
  for (i = 0; i < 500; ++i) {
    make_skewed_string(i, &write_rnd, &expected);
    fixture_write(&f, expected.data, expected.size);
  }
  fixture_start_reading(&f, true, 0);
  for (i = 0; i < 500; ++i) {
    make_skewed_string(i, &read_rnd, &expected);
    fixture_expect_record(&f, expected.data, expected.size);
  }
  fixture_expect_eof(&f);
  CLDB_EXPECT_EQ_UINT(0, f.report.dropped_bytes);
  CLDB_EXPECT_EQ_STR("", f.report.message);
  cldb_buf_free(&expected);
  fixture_cleanup(&f);
}

CLDB_TEST(log_reader_read_error) {
  log_fixture_t f;

  fixture_init(&f);
  fixture_write(&f, "foo", 3);
  fixture_start_reading(&f, true, 0);
  f.source.force_error = true;
  fixture_expect_eof(&f);
  CLDB_EXPECT_EQ_UINT(CLDB_LOG_BLOCK_SIZE, f.report.dropped_bytes);
  CLDB_EXPECT_TRUE(match_error(&f.report, "read error"));
  fixture_cleanup(&f);
}

CLDB_TEST(log_reader_checksum_disabled) {
  log_fixture_t f;

  fixture_init(&f);
  fixture_write(&f, "foo", 3);
  increment_byte(&f.dest.data, 0, 10);
  fixture_start_reading(&f, false, 0);

  fixture_expect_record(&f, "foo", 3);
  fixture_expect_eof(&f);
  CLDB_EXPECT_EQ_UINT(0, f.report.dropped_bytes);
  fixture_cleanup(&f);
}

CLDB_TEST(log_reader_bad_record_type) {
  log_fixture_t f;

  fixture_init(&f);
  fixture_write(&f, "foo", 3);
  increment_byte(&f.dest.data, CLDB_LOG_TYPE_OFFSET, 100);
  fix_checksum(&f.dest.data, 0, 3);
  fixture_start_reading(&f, true, 0);

  fixture_expect_eof(&f);
  CLDB_EXPECT_EQ_UINT(3, f.report.dropped_bytes);
  CLDB_EXPECT_TRUE(match_error(&f.report, "unknown record type"));
  fixture_cleanup(&f);
}

CLDB_TEST(log_reader_zero_record_ignored) {
  log_fixture_t f;

  fixture_init(&f);
  fixture_write(&f, "", 0);
  set_byte(&f.dest.data, CLDB_LOG_TYPE_OFFSET, CLDB_LOG_RECORD_TYPE_ZERO);
  fix_checksum(&f.dest.data, 0, 0);
  fixture_start_reading(&f, true, 0);

  fixture_expect_eof(&f);
  CLDB_EXPECT_EQ_UINT(0, f.report.dropped_bytes);
  CLDB_EXPECT_EQ_STR("", f.report.message);
  fixture_cleanup(&f);
}

CLDB_TEST(log_reader_truncated_trailing_ignored) {
  log_fixture_t f;

  fixture_init(&f);
  fixture_write(&f, "foo", 3);
  shrink(&f.dest.data, 4);
  fixture_start_reading(&f, true, 0);

  fixture_expect_eof(&f);
  CLDB_EXPECT_EQ_UINT(0, f.report.dropped_bytes);
  CLDB_EXPECT_EQ_STR("", f.report.message);
  fixture_cleanup(&f);
}

CLDB_TEST(log_reader_bad_length) {
  log_fixture_t f;
  size_t first_length =
      (size_t)CLDB_LOG_BLOCK_SIZE - (size_t)CLDB_LOG_HEADER_SIZE;

  fixture_init(&f);
  fixture_write_pattern(&f, first_length);
  fixture_write(&f, "foo", 3);
  increment_byte(&f.dest.data, CLDB_LOG_LENGTH_OFFSET, 1);
  fixture_start_reading(&f, true, 0);

  fixture_expect_record(&f, "foo", 3);
  fixture_expect_eof(&f);
  CLDB_EXPECT_EQ_UINT(CLDB_LOG_BLOCK_SIZE, f.report.dropped_bytes);
  CLDB_EXPECT_TRUE(match_error(&f.report, "bad record length"));
  fixture_cleanup(&f);
}

CLDB_TEST(log_reader_bad_length_at_end_ignored) {
  log_fixture_t f;

  fixture_init(&f);
  fixture_write(&f, "foo", 3);
  shrink(&f.dest.data, 1);
  fixture_start_reading(&f, true, 0);

  fixture_expect_eof(&f);
  CLDB_EXPECT_EQ_UINT(0, f.report.dropped_bytes);
  CLDB_EXPECT_EQ_STR("", f.report.message);
  fixture_cleanup(&f);
}

CLDB_TEST(log_reader_checksum_mismatch) {
  log_fixture_t f;

  fixture_init(&f);
  fixture_write(&f, "foo", 3);
  increment_byte(&f.dest.data, 0, 10);
  fixture_start_reading(&f, true, 0);

  fixture_expect_eof(&f);
  CLDB_EXPECT_EQ_UINT(10, f.report.dropped_bytes);
  CLDB_EXPECT_TRUE(match_error(&f.report, "checksum mismatch"));
  fixture_cleanup(&f);
}

CLDB_TEST(log_reader_unexpected_middle_type) {
  log_fixture_t f;

  fixture_init(&f);
  fixture_write(&f, "foo", 3);
  set_byte(&f.dest.data, CLDB_LOG_TYPE_OFFSET, CLDB_LOG_RECORD_TYPE_MIDDLE);
  fix_checksum(&f.dest.data, 0, 3);
  fixture_start_reading(&f, true, 0);

  fixture_expect_eof(&f);
  CLDB_EXPECT_EQ_UINT(3, f.report.dropped_bytes);
  CLDB_EXPECT_TRUE(match_error(&f.report, "missing start"));
  fixture_cleanup(&f);
}

CLDB_TEST(log_reader_unexpected_last_type) {
  log_fixture_t f;

  fixture_init(&f);
  fixture_write(&f, "foo", 3);
  set_byte(&f.dest.data, CLDB_LOG_TYPE_OFFSET, CLDB_LOG_RECORD_TYPE_LAST);
  fix_checksum(&f.dest.data, 0, 3);
  fixture_start_reading(&f, true, 0);

  fixture_expect_eof(&f);
  CLDB_EXPECT_EQ_UINT(3, f.report.dropped_bytes);
  CLDB_EXPECT_TRUE(match_error(&f.report, "missing start"));
  fixture_cleanup(&f);
}

CLDB_TEST(log_reader_unexpected_full_type) {
  log_fixture_t f;

  fixture_init(&f);
  fixture_write(&f, "foo", 3);
  fixture_write(&f, "bar", 3);
  set_byte(&f.dest.data, CLDB_LOG_TYPE_OFFSET, CLDB_LOG_RECORD_TYPE_FIRST);
  fix_checksum(&f.dest.data, 0, 3);
  fixture_start_reading(&f, true, 0);

  fixture_expect_record(&f, "bar", 3);
  fixture_expect_eof(&f);
  CLDB_EXPECT_EQ_UINT(3, f.report.dropped_bytes);
  CLDB_EXPECT_TRUE(match_error(&f.report, "partial record without end"));
  fixture_cleanup(&f);
}

CLDB_TEST(log_reader_unexpected_first_type) {
  log_fixture_t f;

  fixture_init(&f);
  fixture_write(&f, "foo", 3);
  fixture_write_pattern(&f, 100000);
  set_byte(&f.dest.data, CLDB_LOG_TYPE_OFFSET, CLDB_LOG_RECORD_TYPE_FIRST);
  fix_checksum(&f.dest.data, 0, 3);
  fixture_start_reading(&f, true, 0);

  fixture_expect_pattern(&f, 100000);
  fixture_expect_eof(&f);
  CLDB_EXPECT_EQ_UINT(3, f.report.dropped_bytes);
  CLDB_EXPECT_TRUE(match_error(&f.report, "partial record without end"));
  fixture_cleanup(&f);
}

CLDB_TEST(log_reader_missing_last_ignored) {
  log_fixture_t f;

  fixture_init(&f);
  fixture_write_pattern(&f, CLDB_LOG_BLOCK_SIZE);
  shrink(&f.dest.data, 14);
  fixture_start_reading(&f, true, 0);

  fixture_expect_eof(&f);
  CLDB_EXPECT_EQ_UINT(0, f.report.dropped_bytes);
  CLDB_EXPECT_EQ_STR("", f.report.message);
  fixture_cleanup(&f);
}

CLDB_TEST(log_reader_partial_last_ignored) {
  log_fixture_t f;

  fixture_init(&f);
  fixture_write_pattern(&f, CLDB_LOG_BLOCK_SIZE);
  shrink(&f.dest.data, 1);
  fixture_start_reading(&f, true, 0);

  fixture_expect_eof(&f);
  CLDB_EXPECT_EQ_UINT(0, f.report.dropped_bytes);
  CLDB_EXPECT_EQ_STR("", f.report.message);
  fixture_cleanup(&f);
}

CLDB_TEST(log_reader_skip_into_multi_record) {
  log_fixture_t f;

  fixture_init(&f);
  fixture_write_pattern(&f, 3 * (size_t)CLDB_LOG_BLOCK_SIZE);
  fixture_write(&f, "correct", 7);
  fixture_start_reading(&f, true, (uint64_t)CLDB_LOG_BLOCK_SIZE);

  fixture_expect_record(&f, "correct", 7);
  fixture_expect_eof(&f);
  CLDB_EXPECT_EQ_UINT(0, f.report.dropped_bytes);
  CLDB_EXPECT_EQ_STR("", f.report.message);
  fixture_cleanup(&f);
}

CLDB_TEST(log_reader_error_joins_records) {
  log_fixture_t f;

  fixture_init(&f);
  fixture_write_pattern(&f, CLDB_LOG_BLOCK_SIZE);
  fixture_write_pattern(&f, CLDB_LOG_BLOCK_SIZE);
  fixture_write(&f, "correct", 7);
  wipe(&f.dest.data, CLDB_LOG_BLOCK_SIZE, 2 * (size_t)CLDB_LOG_BLOCK_SIZE,
       'x');
  fixture_start_reading(&f, true, 0);

  fixture_expect_record(&f, "correct", 7);
  fixture_expect_eof(&f);
  CLDB_EXPECT_TRUE(f.report.dropped_bytes >=
                   2 * (uint64_t)CLDB_LOG_BLOCK_SIZE);
  CLDB_EXPECT_TRUE(f.report.dropped_bytes <=
                   2 * (uint64_t)CLDB_LOG_BLOCK_SIZE + 100);
  CLDB_EXPECT_TRUE(match_error(&f.report, "checksum mismatch"));
  fixture_cleanup(&f);
}

CLDB_TEST(log_reader_block_tail_garbage_ignored) {
  log_fixture_t f;
  size_t first_length =
      (size_t)CLDB_LOG_BLOCK_SIZE - (size_t)CLDB_LOG_HEADER_SIZE - 6;
  size_t i;

  fixture_init(&f);
  fixture_write_pattern(&f, first_length);
  fixture_write(&f, "bar", 3);
  for (i = 0; i < 6; ++i) {
    set_byte(&f.dest.data,
             (size_t)CLDB_LOG_BLOCK_SIZE - 6 + i, (uint8_t)0x5a);
  }
  fixture_start_reading(&f, true, 0);

  fixture_expect_pattern(&f, first_length);
  fixture_expect_record(&f, "bar", 3);
  fixture_expect_eof(&f);
  CLDB_EXPECT_EQ_UINT(0, f.report.dropped_bytes);
  CLDB_EXPECT_EQ_STR("", f.report.message);
  fixture_cleanup(&f);
}

/* 与上游 WriteInitialOffsetLog 相同的记录尺寸；后续记录从这些偏移开始。 */
static const size_t kInitialRecordSizes[] = {
    10000,
    10000,
    2 * (size_t)CLDB_LOG_BLOCK_SIZE - 1000,
    1,
    13716,
    (size_t)CLDB_LOG_BLOCK_SIZE - (size_t)CLDB_LOG_HEADER_SIZE,
};

static const uint64_t kInitialRecordOffsets[] = {
    0,
    (uint64_t)CLDB_LOG_HEADER_SIZE + 10000,
    2 * ((uint64_t)CLDB_LOG_HEADER_SIZE + 10000),
    2 * ((uint64_t)CLDB_LOG_HEADER_SIZE + 10000) +
        (2 * (uint64_t)CLDB_LOG_BLOCK_SIZE - 1000) +
        3 * (uint64_t)CLDB_LOG_HEADER_SIZE,
    2 * ((uint64_t)CLDB_LOG_HEADER_SIZE + 10000) +
        (2 * (uint64_t)CLDB_LOG_BLOCK_SIZE - 1000) +
        4 * (uint64_t)CLDB_LOG_HEADER_SIZE + 1,
    3 * (uint64_t)CLDB_LOG_BLOCK_SIZE,
};

static void write_initial_offset_log(log_fixture_t *f) {
  size_t i;

  for (i = 0; i < sizeof(kInitialRecordSizes) / sizeof(kInitialRecordSizes[0]);
       ++i) {
    char *data = malloc(kInitialRecordSizes[i]);
    size_t j;

    CLDB_ASSERT_TRUE(data != NULL);
    for (j = 0; j < kInitialRecordSizes[i]; ++j) {
      data[j] = (char)('a' + (int)i);
    }
    fixture_write(f, data, kInitialRecordSizes[i]);
    free(data);
  }
}

static void check_initial_offset(uint64_t initial_offset,
                                 size_t expected_start) {
  log_fixture_t f;
  size_t record_count =
      sizeof(kInitialRecordSizes) / sizeof(kInitialRecordSizes[0]);
  size_t i;
  bool failed = false;

  fixture_init(&f);
  write_initial_offset_log(&f);
  fixture_start_reading(&f, true, initial_offset);
  for (i = expected_start; i < record_count; ++i) {
    cldb_buf_t scratch;
    cldb_slice_t record;
    bool ok;

    cldb_buf_init(&scratch);
    ok = cldb_log_reader_read_record(f.reader, &record, &scratch);
    if (!ok) {
      cldb_buf_free(&scratch);
      CLDB_EXPECT_TRUE(ok);
      failed = true;
      break;
    }
    CLDB_EXPECT_EQ_UINT(kInitialRecordSizes[i], record.size);
    if (record.size > 0) {
      CLDB_EXPECT_EQ_INT((unsigned char)('a' + (int)i),
                         (unsigned char)record.data[0]);
    }
    CLDB_EXPECT_EQ_UINT(kInitialRecordOffsets[i],
                        cldb_log_reader_last_record_offset(f.reader));
    cldb_buf_free(&scratch);
  }
  if (!failed) {
    fixture_expect_eof(&f);
  }
  fixture_cleanup(&f);
}

CLDB_TEST(log_reader_initial_offset) {
  static const struct {
    uint64_t offset;
    size_t expected_start;
  } kCases[] = {
      {0, 0},
      {1, 1},
      {10000, 1},
      {10007, 1},
      {10008, 2},
      {20014, 2},
      {20015, 3},
      {(uint64_t)CLDB_LOG_BLOCK_SIZE - 4, 3},
      {(uint64_t)CLDB_LOG_BLOCK_SIZE + 1, 3},
      {2 * (uint64_t)CLDB_LOG_BLOCK_SIZE + 1, 3},
      {2 * ((uint64_t)CLDB_LOG_HEADER_SIZE + 1000) +
           (2 * (uint64_t)CLDB_LOG_BLOCK_SIZE - 1000) +
           3 * (uint64_t)CLDB_LOG_HEADER_SIZE,
       3},
      {3 * (uint64_t)CLDB_LOG_BLOCK_SIZE - 3, 5},
  };
  size_t i;

  for (i = 0; i < sizeof(kCases) / sizeof(kCases[0]); ++i) {
    check_initial_offset(kCases[i].offset, kCases[i].expected_start);
  }
}

CLDB_TEST(log_reader_initial_offset_past_end) {
  static const uint64_t kDeltas[] = {0, 1, 5, CLDB_LOG_BLOCK_SIZE};
  log_fixture_t f;
  uint64_t written;
  size_t i;

  fixture_init(&f);
  fixture_write_pattern(&f, 10000);
  written = (uint64_t)f.dest.data.size;
  for (i = 0; i < sizeof(kDeltas) / sizeof(kDeltas[0]); ++i) {
    fixture_start_reading(&f, true, written + kDeltas[i]);
    fixture_expect_eof(&f);
  }
  fixture_cleanup(&f);
}

void cldb_register_log_reader_tests(void) {
  CLDB_RUN_TEST(log_reader_create_validation);
  CLDB_RUN_TEST(log_reader_empty);
  CLDB_RUN_TEST(log_reader_read_write);
  CLDB_RUN_TEST(log_reader_many_blocks);
  CLDB_RUN_TEST(log_reader_fragmentation);
  CLDB_RUN_TEST(log_reader_marginal_trailer);
  CLDB_RUN_TEST(log_reader_marginal_trailer2);
  CLDB_RUN_TEST(log_reader_short_trailer);
  CLDB_RUN_TEST(log_reader_aligned_eof);
  CLDB_RUN_TEST(log_reader_open_for_append);
  CLDB_RUN_TEST(log_reader_random_skewed);
  CLDB_RUN_TEST(log_reader_read_error);
  CLDB_RUN_TEST(log_reader_checksum_disabled);
  CLDB_RUN_TEST(log_reader_bad_record_type);
  CLDB_RUN_TEST(log_reader_zero_record_ignored);
  CLDB_RUN_TEST(log_reader_truncated_trailing_ignored);
  CLDB_RUN_TEST(log_reader_bad_length);
  CLDB_RUN_TEST(log_reader_bad_length_at_end_ignored);
  CLDB_RUN_TEST(log_reader_checksum_mismatch);
  CLDB_RUN_TEST(log_reader_unexpected_middle_type);
  CLDB_RUN_TEST(log_reader_unexpected_last_type);
  CLDB_RUN_TEST(log_reader_unexpected_full_type);
  CLDB_RUN_TEST(log_reader_unexpected_first_type);
  CLDB_RUN_TEST(log_reader_missing_last_ignored);
  CLDB_RUN_TEST(log_reader_partial_last_ignored);
  CLDB_RUN_TEST(log_reader_skip_into_multi_record);
  CLDB_RUN_TEST(log_reader_error_joins_records);
  CLDB_RUN_TEST(log_reader_block_tail_garbage_ignored);
  CLDB_RUN_TEST(log_reader_initial_offset);
  CLDB_RUN_TEST(log_reader_initial_offset_past_end);
}
