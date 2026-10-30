#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "db/log_format.h"
#include "db/log_writer.h"
#include "harness.h"
#include "status_test_util.h"
#include "util/coding.h"
#include "util/crc32c.h"

/* WAL 写入器；对应上游 db/log_test.cc 的写入用例。Reader 尚未实现，测试
 * 用本文件的最小解析器直接检查物理格式。 */

/* 捕获追加内容的 writable file；可用于注入 Append/Flush 失败。 */
typedef struct string_file {
  cldb_writable_file_t base; /* 必须为首成员 */
  cldb_buf_t data;
  int append_calls;
  int flush_calls;
  int fail_append_call; /* 1 起始；0 表示不注入 */
  int fail_flush_call;
} string_file_t;

static cldb_status_t string_file_append(cldb_writable_file_t *file,
                                        cldb_slice_t data) {
  string_file_t *self = (string_file_t *)file;

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

static cldb_status_t string_file_close(cldb_writable_file_t *file) {
  (void)file;
  return cldb_status_ok();
}

static cldb_status_t string_file_flush(cldb_writable_file_t *file) {
  string_file_t *self = (string_file_t *)file;

  self->flush_calls += 1;
  if (self->fail_flush_call > 0 &&
      self->flush_calls == self->fail_flush_call) {
    return cldb_status_io("string_file", "injected flush error");
  }
  return cldb_status_ok();
}

static cldb_status_t string_file_sync(cldb_writable_file_t *file) {
  (void)file;
  return cldb_status_ok();
}

static void string_file_destroy(cldb_writable_file_t *file) { (void)file; }

static const cldb_writable_file_vtbl_t kStringFileVtbl = {
    string_file_append, string_file_close,  string_file_flush,
    string_file_sync,   string_file_destroy,
};

static void string_file_init(string_file_t *file) {
  memset(file, 0, sizeof(*file));
  file->base.vtbl = &kStringFileVtbl;
  cldb_buf_init(&file->data);
}

static void string_file_cleanup(string_file_t *file) {
  cldb_buf_free(&file->data);
}

typedef struct parsed_record {
  cldb_log_record_type_t type;
  size_t length;
  const char *payload; /* 指向 data 内部；仅本次解析有效 */
} parsed_record_t;

/* 按块边界解析物理记录；base_offset 是 data 在虚拟文件中的起始偏移。
 * 校验长度、type、CRC 和块尾零填充，返回记录数。 */
static size_t parse_physical_records_at(const cldb_buf_t *data,
                                        uint64_t base_offset,
                                        parsed_record_t *records,
                                        size_t capacity) {
  uint32_t type_crc[CLDB_LOG_TYPE_CRC_COUNT];
  size_t pos = 0;
  size_t count = 0;

  cldb_log_init_type_crc(type_crc);
  while (pos < data->size) {
    size_t block_offset =
        (size_t)((base_offset + (uint64_t)pos) %
                 (uint64_t)CLDB_LOG_BLOCK_SIZE);
    size_t block_remaining = (size_t)CLDB_LOG_BLOCK_SIZE - block_offset;

    if (block_remaining < CLDB_LOG_HEADER_SIZE) {
      size_t i;

      for (i = 0; i < block_remaining; ++i) {
        CLDB_EXPECT_EQ_INT(0, (unsigned char)data->data[pos + i]);
      }
      pos += block_remaining;
      continue;
    }

    CLDB_ASSERT_TRUE(data->size - pos >= (size_t)CLDB_LOG_HEADER_SIZE);
    {
      uint32_t length =
          (uint32_t)(unsigned char)data->data[pos + CLDB_LOG_LENGTH_OFFSET] |
          ((uint32_t)(unsigned char)
               data->data[pos + CLDB_LOG_LENGTH_OFFSET + 1]
           << 8);
      uint8_t type = (uint8_t)data->data[pos + CLDB_LOG_TYPE_OFFSET];
      const char *payload = data->data + pos + CLDB_LOG_HEADER_SIZE;
      uint32_t stored;
      uint32_t expected;

      CLDB_ASSERT_TRUE(type <= CLDB_LOG_MAX_RECORD_TYPE);
      CLDB_ASSERT_TRUE((size_t)length <=
                       block_remaining - (size_t)CLDB_LOG_HEADER_SIZE);
      CLDB_ASSERT_TRUE((size_t)length <=
                       data->size - pos - (size_t)CLDB_LOG_HEADER_SIZE);
      CLDB_ASSERT_TRUE(count < capacity);
      records[count].type = (cldb_log_record_type_t)type;
      records[count].length = (size_t)length;
      records[count].payload = payload;

      stored = cldb_decode_fixed32(data->data + pos + CLDB_LOG_CRC_OFFSET);
      expected = cldb_crc32c_mask(
          cldb_crc32c_extend(type_crc[type], payload, (size_t)length));
      CLDB_EXPECT_EQ_UINT(expected, stored);

      pos += (size_t)CLDB_LOG_HEADER_SIZE + (size_t)length;
      ++count;
    }
  }
  CLDB_EXPECT_EQ_UINT(data->size, pos);
  return count;
}

/* 确定性的 payload 字节，便于检查分片拼接结果。 */
static char pattern_byte(size_t index) {
  return (char)(unsigned char)((index * 31u + 7u) % 251u);
}

static cldb_status_t add_pattern_record(cldb_log_writer_t *writer,
                                        size_t length) {
  char *data = NULL;
  cldb_status_t status;

  if (length > 0) {
    size_t i;

    data = malloc(length);
    if (data == NULL) {
      return cldb_status_io("test", "out of memory");
    }
    for (i = 0; i < length; ++i) {
      data[i] = pattern_byte(i);
    }
  }
  status = cldb_log_writer_add_record(writer, cldb_slice_make(data, length));
  free(data);
  return status;
}

/* 验证单条逻辑记录的分片类型、长度、CRC 与拼接后的 payload。 */
static void expect_pattern_fragments(const cldb_buf_t *data,
                                     uint64_t base_offset,
                                     size_t total_length,
                                     const cldb_log_record_type_t *types,
                                     const size_t *fragments,
                                     size_t fragment_count) {
  parsed_record_t records[8];
  size_t count;
  size_t i;
  size_t joined = 0;

  count = parse_physical_records_at(data, base_offset, records, 8);
  CLDB_ASSERT_EQ_UINT(fragment_count, count);
  for (i = 0; i < count; ++i) {
    size_t j;

    CLDB_EXPECT_EQ_INT(types[i], records[i].type);
    CLDB_EXPECT_EQ_UINT(fragments[i], records[i].length);
    for (j = 0; j < records[i].length; ++j) {
      CLDB_EXPECT_EQ_INT((unsigned char)pattern_byte(joined),
                         (unsigned char)records[i].payload[j]);
      ++joined;
    }
  }
  CLDB_EXPECT_EQ_UINT(total_length, joined);
}

CLDB_TEST(log_writer_create_validation) {
  string_file_t file;
  cldb_log_writer_t *writer = NULL;
  cldb_slice_t bad_slice = {NULL, 5};

  string_file_init(&file);
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_log_writer_create(NULL, &writer));
  CLDB_EXPECT_EQ_PTR(NULL, writer);
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_log_writer_create(&file.base, NULL));
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_log_writer_create_with_length(NULL, 0, &writer));
  CLDB_EXPECT_EQ_PTR(NULL, writer);

  CLDB_ASSERT_OK(cldb_log_writer_create(&file.base, &writer));
  CLDB_EXPECT_EQ_INT(0, writer->block_offset);
  CLDB_EXPECT_STATUS(
      CLDB_STATUS_INVALID_ARGUMENT,
      cldb_log_writer_add_record(NULL, cldb_slice_empty()));
  CLDB_EXPECT_STATUS(
      CLDB_STATUS_INVALID_ARGUMENT,
      cldb_log_writer_add_record(writer, bad_slice));
  CLDB_EXPECT_EQ_UINT(0, file.data.size);

  cldb_log_writer_destroy(writer);
  cldb_log_writer_destroy(NULL);
  string_file_cleanup(&file);
}

/* 手写 FULL "hello" 的 golden；与日志格式测试的 12 字节 hexdump 一致。 */
CLDB_TEST(log_writer_hello_golden) {
  static const char kHelloRecord[] = {
      (char)0x0b, (char)0xb9, (char)0x57, (char)0x58, (char)0x05,
      (char)0x00, (char)0x01, 'h',         'e',         'l',
      'l',         'o',
  };
  string_file_t file;
  cldb_log_writer_t *writer = NULL;

  string_file_init(&file);
  CLDB_ASSERT_OK(cldb_log_writer_create(&file.base, &writer));
  CLDB_ASSERT_OK(
      cldb_log_writer_add_record(writer, cldb_slice_make("hello", 5)));

  CLDB_EXPECT_EQ_UINT(sizeof(kHelloRecord), file.data.size);
  if (file.data.size == sizeof(kHelloRecord)) {
    CLDB_EXPECT_EQ_INT(0, memcmp(file.data.data, kHelloRecord,
                                 sizeof(kHelloRecord)));
  }
  CLDB_EXPECT_EQ_INT(12, writer->block_offset);
  CLDB_EXPECT_EQ_INT(2, file.append_calls);
  CLDB_EXPECT_EQ_INT(1, file.flush_calls);

  cldb_log_writer_destroy(writer);
  string_file_cleanup(&file);
}

CLDB_TEST(log_writer_fragmentation) {
  static const struct {
    size_t length;
    cldb_log_record_type_t types[4];
    size_t fragments[4];
    size_t count;
    size_t file_size;
  } kCases[] = {
      {0, {CLDB_LOG_RECORD_TYPE_FULL}, {0}, 1, 7},
      {1, {CLDB_LOG_RECORD_TYPE_FULL}, {1}, 1, 8},
      {7, {CLDB_LOG_RECORD_TYPE_FULL}, {7}, 1, 14},
      {32760, {CLDB_LOG_RECORD_TYPE_FULL}, {32760}, 1, 32767},
      {32761, {CLDB_LOG_RECORD_TYPE_FULL}, {32761}, 1, 32768},
      {32768,
       {CLDB_LOG_RECORD_TYPE_FIRST, CLDB_LOG_RECORD_TYPE_LAST},
       {32761, 7},
       2,
       32782},
      {100000,
       {CLDB_LOG_RECORD_TYPE_FIRST, CLDB_LOG_RECORD_TYPE_MIDDLE,
        CLDB_LOG_RECORD_TYPE_MIDDLE, CLDB_LOG_RECORD_TYPE_LAST},
       {32761, 32761, 32761, 1717},
       4,
       100028},
  };
  size_t c;

  for (c = 0; c < sizeof(kCases) / sizeof(kCases[0]); ++c) {
    string_file_t file;
    cldb_log_writer_t *writer = NULL;

    string_file_init(&file);
    CLDB_ASSERT_OK(cldb_log_writer_create(&file.base, &writer));
    CLDB_ASSERT_OK(add_pattern_record(writer, kCases[c].length));
    CLDB_EXPECT_EQ_UINT(kCases[c].file_size, file.data.size);
    expect_pattern_fragments(&file.data, 0, kCases[c].length,
                             kCases[c].types, kCases[c].fragments,
                             kCases[c].count);
    cldb_log_writer_destroy(writer);
    string_file_cleanup(&file);
  }
}

/* 块尾剩 0..6 字节时先补零，再在新块写整条记录。 */
CLDB_TEST(log_writer_trailer_padding) {
  int leftover;

  for (leftover = 0; leftover <= 6; ++leftover) {
    string_file_t file;
    cldb_log_writer_t *writer = NULL;
    parsed_record_t records[2];
    size_t first_length =
        (size_t)(CLDB_LOG_BLOCK_SIZE - CLDB_LOG_HEADER_SIZE - leftover);
    size_t count;

    string_file_init(&file);
    CLDB_ASSERT_OK(cldb_log_writer_create(&file.base, &writer));
    CLDB_ASSERT_OK(add_pattern_record(writer, first_length));
    CLDB_ASSERT_OK(
        cldb_log_writer_add_record(writer, cldb_slice_make("x", 1)));

    CLDB_EXPECT_EQ_UINT(
        (size_t)CLDB_LOG_BLOCK_SIZE + CLDB_LOG_HEADER_SIZE + 1,
        file.data.size);
    count = parse_physical_records_at(&file.data, 0, records, 2);
    CLDB_ASSERT_EQ_UINT(2, count);
    CLDB_EXPECT_EQ_INT(CLDB_LOG_RECORD_TYPE_FULL, records[0].type);
    CLDB_EXPECT_EQ_UINT(first_length, records[0].length);
    CLDB_EXPECT_EQ_INT(pattern_byte(0), records[0].payload[0]);
    CLDB_EXPECT_EQ_INT(pattern_byte(first_length - 1),
                       records[0].payload[first_length - 1]);
    CLDB_EXPECT_EQ_INT(CLDB_LOG_RECORD_TYPE_FULL, records[1].type);
    CLDB_EXPECT_EQ_UINT(1, records[1].length);
    CLDB_EXPECT_EQ_INT('x', records[1].payload[0]);

    cldb_log_writer_destroy(writer);
    string_file_cleanup(&file);
  }
}

/* 记录恰好留下一个 header 空间时：空记录占满块尾；非空记录先写
 * 零长度 FIRST，再在新块写 LAST。对应上游 MarginalTrailer 用例。 */
CLDB_TEST(log_writer_marginal_trailer) {
  parsed_record_t records[3];
  size_t count;

  {
    string_file_t file;
    cldb_log_writer_t *writer = NULL;

    string_file_init(&file);
    CLDB_ASSERT_OK(cldb_log_writer_create(&file.base, &writer));
    CLDB_ASSERT_OK(add_pattern_record(writer, 32754));
    CLDB_ASSERT_OK(cldb_log_writer_add_record(writer, cldb_slice_empty()));
    CLDB_ASSERT_OK(
        cldb_log_writer_add_record(writer, cldb_slice_make("bar", 3)));

    CLDB_EXPECT_EQ_UINT(
        (size_t)CLDB_LOG_BLOCK_SIZE + CLDB_LOG_HEADER_SIZE + 3,
        file.data.size);
    count = parse_physical_records_at(&file.data, 0, records, 3);
    CLDB_ASSERT_EQ_UINT(3, count);
    CLDB_EXPECT_EQ_INT(CLDB_LOG_RECORD_TYPE_FULL, records[0].type);
    CLDB_EXPECT_EQ_UINT(32754, records[0].length);
    CLDB_EXPECT_EQ_INT(CLDB_LOG_RECORD_TYPE_FULL, records[1].type);
    CLDB_EXPECT_EQ_UINT(0, records[1].length);
    CLDB_EXPECT_EQ_INT(CLDB_LOG_RECORD_TYPE_FULL, records[2].type);
    CLDB_EXPECT_EQ_UINT(3, records[2].length);
    CLDB_EXPECT_EQ_INT(0, memcmp(records[2].payload, "bar", 3));

    cldb_log_writer_destroy(writer);
    string_file_cleanup(&file);
  }

  {
    string_file_t file;
    cldb_log_writer_t *writer = NULL;

    string_file_init(&file);
    CLDB_ASSERT_OK(cldb_log_writer_create(&file.base, &writer));
    CLDB_ASSERT_OK(add_pattern_record(writer, 32754));
    CLDB_ASSERT_OK(
        cldb_log_writer_add_record(writer, cldb_slice_make("bar", 3)));

    CLDB_EXPECT_EQ_UINT(
        (size_t)CLDB_LOG_BLOCK_SIZE + CLDB_LOG_HEADER_SIZE + 3,
        file.data.size);
    count = parse_physical_records_at(&file.data, 0, records, 3);
    CLDB_ASSERT_EQ_UINT(3, count);
    CLDB_EXPECT_EQ_INT(CLDB_LOG_RECORD_TYPE_FULL, records[0].type);
    CLDB_EXPECT_EQ_UINT(32754, records[0].length);
    CLDB_EXPECT_EQ_INT(CLDB_LOG_RECORD_TYPE_FIRST, records[1].type);
    CLDB_EXPECT_EQ_UINT(0, records[1].length);
    CLDB_EXPECT_EQ_INT(CLDB_LOG_RECORD_TYPE_LAST, records[2].type);
    CLDB_EXPECT_EQ_UINT(3, records[2].length);
    CLDB_EXPECT_EQ_INT(0, memcmp(records[2].payload, "bar", 3));

    cldb_log_writer_destroy(writer);
    string_file_cleanup(&file);
  }
}

CLDB_TEST(log_writer_dest_length_offset) {
  static const uint64_t kLengths[] = {0, 1, 32767, 32768, 32769, 100000};
  static const int kOffsets[] = {0, 1, 32767, 0, 1, 1696};
  size_t i;

  for (i = 0; i < sizeof(kLengths) / sizeof(kLengths[0]); ++i) {
    string_file_t file;
    cldb_log_writer_t *writer = NULL;

    string_file_init(&file);
    CLDB_ASSERT_OK(cldb_log_writer_create_with_length(&file.base,
                                                      kLengths[i], &writer));
    CLDB_EXPECT_EQ_INT(kOffsets[i], writer->block_offset);
    cldb_log_writer_destroy(writer);
    string_file_cleanup(&file);
  }
}

CLDB_TEST(log_writer_dest_length_resume) {
  static const char kHelloRecord[] = {
      (char)0x0b, (char)0xb9, (char)0x57, (char)0x58, (char)0x05,
      (char)0x00, (char)0x01, 'h',         'e',         'l',
      'l',         'o',
  };
  parsed_record_t records[2];
  size_t count;

  /* 从虚拟偏移 100 继续写。 */
  {
    string_file_t file;
    cldb_log_writer_t *writer = NULL;

    string_file_init(&file);
    CLDB_ASSERT_OK(
        cldb_log_writer_create_with_length(&file.base, 100, &writer));
    CLDB_ASSERT_OK(
        cldb_log_writer_add_record(writer, cldb_slice_make("hello", 5)));

    CLDB_EXPECT_EQ_INT(112, writer->block_offset);
    CLDB_EXPECT_EQ_UINT(sizeof(kHelloRecord), file.data.size);
    if (file.data.size == sizeof(kHelloRecord)) {
      CLDB_EXPECT_EQ_INT(0, memcmp(file.data.data, kHelloRecord,
                                   sizeof(kHelloRecord)));
    }
    count = parse_physical_records_at(&file.data, 100, records, 2);
    CLDB_ASSERT_EQ_UINT(1, count);
    CLDB_EXPECT_EQ_INT(CLDB_LOG_RECORD_TYPE_FULL, records[0].type);
    CLDB_EXPECT_EQ_UINT(5, records[0].length);

    cldb_log_writer_destroy(writer);
    string_file_cleanup(&file);
  }

  /* 块尾剩 3 字节：先补零，再在新块写 FULL。 */
  {
    string_file_t file;
    cldb_log_writer_t *writer = NULL;

    string_file_init(&file);
    CLDB_ASSERT_OK(cldb_log_writer_create_with_length(
        &file.base, (uint64_t)CLDB_LOG_BLOCK_SIZE - 3, &writer));
    CLDB_ASSERT_OK(
        cldb_log_writer_add_record(writer, cldb_slice_make("x", 1)));

    CLDB_EXPECT_EQ_INT(8, writer->block_offset);
    CLDB_EXPECT_EQ_UINT(11, file.data.size);
    count = parse_physical_records_at(
        &file.data, (uint64_t)CLDB_LOG_BLOCK_SIZE - 3, records, 2);
    CLDB_ASSERT_EQ_UINT(1, count);
    CLDB_EXPECT_EQ_INT(CLDB_LOG_RECORD_TYPE_FULL, records[0].type);
    CLDB_EXPECT_EQ_UINT(1, records[0].length);
    CLDB_EXPECT_EQ_INT('x', records[0].payload[0]);

    cldb_log_writer_destroy(writer);
    string_file_cleanup(&file);
  }

  /* 块尾剩 7 字节：非空记录先写零长度 FIRST，再在新块写 LAST。 */
  {
    string_file_t file;
    cldb_log_writer_t *writer = NULL;

    string_file_init(&file);
    CLDB_ASSERT_OK(cldb_log_writer_create_with_length(
        &file.base, (uint64_t)CLDB_LOG_BLOCK_SIZE - 7, &writer));
    CLDB_ASSERT_OK(
        cldb_log_writer_add_record(writer, cldb_slice_make("bar", 3)));

    CLDB_EXPECT_EQ_INT(10, writer->block_offset);
    CLDB_EXPECT_EQ_UINT(17, file.data.size);
    count = parse_physical_records_at(
        &file.data, (uint64_t)CLDB_LOG_BLOCK_SIZE - 7, records, 2);
    CLDB_ASSERT_EQ_UINT(2, count);
    CLDB_EXPECT_EQ_INT(CLDB_LOG_RECORD_TYPE_FIRST, records[0].type);
    CLDB_EXPECT_EQ_UINT(0, records[0].length);
    CLDB_EXPECT_EQ_INT(CLDB_LOG_RECORD_TYPE_LAST, records[1].type);
    CLDB_EXPECT_EQ_UINT(3, records[1].length);
    CLDB_EXPECT_EQ_INT(0, memcmp(records[1].payload, "bar", 3));

    cldb_log_writer_destroy(writer);
    string_file_cleanup(&file);
  }
}

CLDB_TEST(log_writer_error_propagation) {
  string_file_t file;
  cldb_log_writer_t *writer = NULL;

  /* header 追加失败。 */
  string_file_init(&file);
  file.fail_append_call = 1;
  CLDB_ASSERT_OK(cldb_log_writer_create(&file.base, &writer));
  CLDB_EXPECT_STATUS(
      CLDB_STATUS_IO_ERROR,
      cldb_log_writer_add_record(writer, cldb_slice_make("hello", 5)));
  CLDB_EXPECT_EQ_INT(1, file.append_calls);
  CLDB_EXPECT_EQ_INT(0, file.flush_calls);
  CLDB_EXPECT_EQ_UINT(0, file.data.size);
  cldb_log_writer_destroy(writer);
  string_file_cleanup(&file);

  /* payload 追加失败。 */
  string_file_init(&file);
  file.fail_append_call = 2;
  CLDB_ASSERT_OK(cldb_log_writer_create(&file.base, &writer));
  CLDB_EXPECT_STATUS(
      CLDB_STATUS_IO_ERROR,
      cldb_log_writer_add_record(writer, cldb_slice_make("hello", 5)));
  CLDB_EXPECT_EQ_INT(2, file.append_calls);
  CLDB_EXPECT_EQ_INT(0, file.flush_calls);
  CLDB_EXPECT_EQ_UINT(CLDB_LOG_HEADER_SIZE, file.data.size);
  cldb_log_writer_destroy(writer);
  string_file_cleanup(&file);

  /* Flush 失败。 */
  string_file_init(&file);
  file.fail_flush_call = 1;
  CLDB_ASSERT_OK(cldb_log_writer_create(&file.base, &writer));
  CLDB_EXPECT_STATUS(
      CLDB_STATUS_IO_ERROR,
      cldb_log_writer_add_record(writer, cldb_slice_make("hello", 5)));
  CLDB_EXPECT_EQ_INT(2, file.append_calls);
  CLDB_EXPECT_EQ_INT(1, file.flush_calls);
  CLDB_EXPECT_EQ_UINT(12, file.data.size);
  cldb_log_writer_destroy(writer);
  string_file_cleanup(&file);

  /* 块尾零填充失败。 */
  string_file_init(&file);
  file.fail_append_call = 1;
  CLDB_ASSERT_OK(cldb_log_writer_create_with_length(
      &file.base, (uint64_t)CLDB_LOG_BLOCK_SIZE - 3, &writer));
  CLDB_EXPECT_STATUS(
      CLDB_STATUS_IO_ERROR,
      cldb_log_writer_add_record(writer, cldb_slice_make("x", 1)));
  CLDB_EXPECT_EQ_INT(1, file.append_calls);
  CLDB_EXPECT_EQ_INT(0, file.flush_calls);
  CLDB_EXPECT_EQ_UINT(0, file.data.size);
  cldb_log_writer_destroy(writer);
  string_file_cleanup(&file);

  /* 第二片 header 失败后停止，不再写后续分片。 */
  string_file_init(&file);
  file.fail_append_call = 3;
  CLDB_ASSERT_OK(cldb_log_writer_create(&file.base, &writer));
  CLDB_EXPECT_STATUS(CLDB_STATUS_IO_ERROR,
                     add_pattern_record(writer, 100000));
  CLDB_EXPECT_EQ_INT(3, file.append_calls);
  CLDB_EXPECT_EQ_INT(1, file.flush_calls);
  CLDB_EXPECT_EQ_UINT(CLDB_LOG_BLOCK_SIZE, file.data.size);
  cldb_log_writer_destroy(writer);
  string_file_cleanup(&file);
}

void cldb_register_log_writer_tests(void) {
  CLDB_RUN_TEST(log_writer_create_validation);
  CLDB_RUN_TEST(log_writer_hello_golden);
  CLDB_RUN_TEST(log_writer_fragmentation);
  CLDB_RUN_TEST(log_writer_trailer_padding);
  CLDB_RUN_TEST(log_writer_marginal_trailer);
  CLDB_RUN_TEST(log_writer_dest_length_offset);
  CLDB_RUN_TEST(log_writer_dest_length_resume);
  CLDB_RUN_TEST(log_writer_error_propagation);
}
