#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cleveldb/env.h"
#include "db/log_format.h"
#include "db/log_reader.h"
#include "db/log_test_util.h"
#include "db/log_writer.h"
#include "harness.h"
#include "status_test_util.h"
#include "util/coding.h"
#include "util/crc32c.h"
#include "util/random.h"

/* WAL 字节兼容、截断/翻位/类型损坏注入与确定性 fuzz。
 * 对应上游 db/log_test.cc 的格式与恢复用例。 */

enum { kMaxCompatRecords = 64 };
enum { kExternalBigSize = 32768 };

/* 固定序列的 golden：header 依次为 masked CRC、长度、类型，随后 payload。
 * 分别覆盖空记录、单条记录和混合长度记录，用于锚定 CRC 与字节布局。 */
static const unsigned char kGoldenEmpty[] = {
    0x05, 0x2b, 0x28, 0x43, 0x00, 0x00, 0x01,
};

static const unsigned char kGoldenHello[] = {
    0x0b, 0xb9, 0x57, 0x58, 0x05, 0x00, 0x01,
    'h',  'e',  'l',  'l',  'o',
};

static const unsigned char kGoldenFour[] = {
    0xdd, 0x5f, 0xb3, 0x7a, 0x03, 0x00, 0x01, 0x66, 0x6f, 0x6f,
    0xba, 0xea, 0xec, 0x44, 0x03, 0x00, 0x01, 0x62, 0x61, 0x72,
    0x05, 0x2b, 0x28, 0x43, 0x00, 0x00, 0x01,
    0x15, 0x3d, 0x26, 0xe9, 0x04, 0x00, 0x01, 0x78, 0x78, 0x78, 0x78,
};

/* 用生产 Writer 编码序列；file 由调用者释放。 */
static void encode_with_writer(cldb_test_string_file_t *file,
                               const cldb_slice_t *records, size_t count) {
  cldb_log_writer_t *writer = NULL;
  size_t i;

  cldb_test_string_file_init(file);
  CLDB_ASSERT_OK(cldb_log_writer_create(&file->base, &writer));
  for (i = 0; i < count; ++i) {
    CLDB_ASSERT_OK(cldb_log_writer_add_record(writer, records[i]));
  }
  cldb_log_writer_destroy(writer);
}

/* 向 out 追加 length 字节的确定性 pattern。 */
static bool append_pattern(cldb_buf_t *out, size_t length) {
  size_t start = out->size;

  if (length > SIZE_MAX - start) {
    return false;
  }
  if (!cldb_buf_resize(out, start + length)) {
    return false;
  }
  cldb_test_fill_pattern(out->data + start, length);
  return true;
}

/* 构造 pattern 记录；slices 指向 payload 内部，payload 必须最后释放。 */
static bool make_pattern_records(const size_t *lengths, size_t count,
                                 cldb_buf_t *payload,
                                 cldb_slice_t *records) {
  size_t offsets[kMaxCompatRecords];
  size_t i;

  if (count > (size_t)kMaxCompatRecords) {
    return false;
  }
  cldb_buf_clear(payload);
  for (i = 0; i < count; ++i) {
    offsets[i] = payload->size;
    if (!append_pattern(payload, lengths[i])) {
      return false;
    }
  }
  for (i = 0; i < count; ++i) {
    records[i] = lengths[i] == 0
                     ? cldb_slice_empty()
                     : cldb_slice_make(payload->data + offsets[i], lengths[i]);
  }
  return true;
}

/* 在记录前 4 字节写入序号，使 fuzz 结果可唯一匹配。 */
static bool make_unique_pattern_records(const size_t *lengths, size_t count,
                                        cldb_buf_t *payload,
                                        cldb_slice_t *records) {
  size_t i;

  if (!make_pattern_records(lengths, count, payload, records)) {
    return false;
  }
  for (i = 0; i < count; ++i) {
    size_t offset;

    if (records[i].size < 4) {
      return false;
    }
    offset = (size_t)(records[i].data - payload->data);
    cldb_encode_fixed32(payload->data + offset, (uint32_t)i);
  }
  return true;
}

/* 独立参考编码器：按格式规范手工拼 header、分片和块尾补零，不复用 Writer。
 * CRC 原语沿用已由 golden 校验的实现。 */
static bool reference_append_record(cldb_buf_t *out, unsigned int type,
                                    const char *payload, size_t length) {
  char header[CLDB_LOG_HEADER_SIZE];
  char type_byte = (char)type;
  uint32_t crc;
  uint32_t length32;

  if (length > UINT16_MAX || (payload == NULL && length > 0)) {
    return false;
  }
  crc = cldb_crc32c_mask(cldb_crc32c_extend(
      cldb_crc32c_value(&type_byte, 1), payload, length));
  length32 = (uint32_t)length;
  header[0] = (char)(uint8_t)(crc & 0xffu);
  header[1] = (char)(uint8_t)((crc >> 8) & 0xffu);
  header[2] = (char)(uint8_t)((crc >> 16) & 0xffu);
  header[3] = (char)(uint8_t)((crc >> 24) & 0xffu);
  header[4] = (char)(uint8_t)(length32 & 0xffu);
  header[5] = (char)(uint8_t)((length32 >> 8) & 0xffu);
  header[6] = (char)(uint8_t)type;
  if (!cldb_buf_append(out, header, CLDB_LOG_HEADER_SIZE)) {
    return false;
  }
  return length == 0 || cldb_buf_append(out, payload, length);
}

/* 块尾不足 header 时补零；最多 CLDB_LOG_HEADER_SIZE - 1 字节。 */
static bool reference_pad(cldb_buf_t *out, size_t count) {
  static const char kZeros[CLDB_LOG_HEADER_SIZE - 1] = {0};

  if (count > sizeof(kZeros)) {
    return false;
  }
  return cldb_buf_append(out, kZeros, count);
}

/* 编码整个序列；ends 可选，返回每条逻辑记录最后一个物理字节之后的位置。 */
static bool reference_encode_with_ends(const cldb_slice_t *records,
                                       size_t count, cldb_buf_t *out,
                                       size_t *ends, size_t *end_count) {
  size_t block_offset = 0;
  size_t i;

  if (count > (size_t)kMaxCompatRecords) {
    return false;
  }
  cldb_buf_clear(out);
  for (i = 0; i < count; ++i) {
    const char *pos = records[i].data;
    size_t left = records[i].size;
    bool first_fragment = true;

    do {
      size_t leftover = (size_t)CLDB_LOG_BLOCK_SIZE - block_offset;
      size_t fragment_length;
      unsigned int type;

      if (leftover < (size_t)CLDB_LOG_HEADER_SIZE) {
        if (!reference_pad(out, leftover)) {
          return false;
        }
        block_offset = 0;
        leftover = (size_t)CLDB_LOG_BLOCK_SIZE;
      }
      leftover -= (size_t)CLDB_LOG_HEADER_SIZE;
      fragment_length = left < leftover ? left : leftover;
      if (first_fragment) {
        type = left == fragment_length
                   ? (unsigned int)CLDB_LOG_RECORD_TYPE_FULL
                   : (unsigned int)CLDB_LOG_RECORD_TYPE_FIRST;
      } else {
        type = left == fragment_length
                   ? (unsigned int)CLDB_LOG_RECORD_TYPE_LAST
                   : (unsigned int)CLDB_LOG_RECORD_TYPE_MIDDLE;
      }
      if (!reference_append_record(out, type, pos, fragment_length)) {
        return false;
      }
      block_offset += (size_t)CLDB_LOG_HEADER_SIZE + fragment_length;
      if (fragment_length > 0) {
        pos += fragment_length;
      }
      left -= fragment_length;
      first_fragment = false;
    } while (left > 0);
    if (ends != NULL) {
      ends[i] = out->size;
    }
  }
  if (end_count != NULL) {
    *end_count = count;
  }
  return true;
}

/* 同时用生产 Writer 和参考编码器编码，并确认两者逐字节一致。 */
static bool encode_layout(const cldb_slice_t *records, size_t count,
                          cldb_test_string_file_t *file,
                          cldb_buf_t *reference, size_t *ends,
                          size_t *end_count) {
  encode_with_writer(file, records, count);
  if (!reference_encode_with_ends(records, count, reference, ends,
                                  end_count)) {
    return false;
  }
  if (reference->size != file->data.size) {
    return false;
  }
  return reference->size == 0 ||
         memcmp(reference->data, file->data.data, reference->size) == 0;
}

/* 比较 Writer 与参考编码器输出；不经过 golden。 */
static void expect_encodings_equal(const cldb_slice_t *records,
                                   size_t count) {
  cldb_test_string_file_t file;
  cldb_buf_t reference;
  size_t ends[kMaxCompatRecords];
  size_t end_count = 0;
  bool encoded;

  cldb_buf_init(&reference);
  encoded = encode_layout(records, count, &file, &reference, ends, &end_count);
  CLDB_EXPECT_TRUE(encoded);
  CLDB_EXPECT_EQ_UINT(count, end_count);
  cldb_test_string_file_free(&file);
  cldb_buf_free(&reference);
}

/* 比较 Writer 与参考编码器输出都等于给定 golden。 */
static void expect_golden(const cldb_slice_t *records, size_t count,
                          const unsigned char *golden, size_t golden_size) {
  cldb_test_string_file_t file;
  cldb_buf_t reference;
  size_t ends[kMaxCompatRecords];
  size_t end_count = 0;
  bool encoded;

  cldb_buf_init(&reference);
  encoded = encode_layout(records, count, &file, &reference, ends, &end_count);
  CLDB_EXPECT_TRUE(encoded);
  CLDB_EXPECT_EQ_UINT(golden_size, file.data.size);
  CLDB_EXPECT_EQ_UINT(golden_size, reference.size);
  if (file.data.size == golden_size && golden_size > 0) {
    CLDB_EXPECT_EQ_INT(0, memcmp(file.data.data, golden, golden_size));
  }
  if (reference.size == golden_size && golden_size > 0) {
    CLDB_EXPECT_EQ_INT(0, memcmp(reference.data, golden, golden_size));
  }
  cldb_test_string_file_free(&file);
  cldb_buf_free(&reference);
}

/* 从已打开的 SequentialFile 读取至 EOF，逐条与 expected 前缀比较。 */
static size_t read_from_file(cldb_sequential_file_t *file,
                             const cldb_slice_t *expected,
                             size_t expected_count,
                             cldb_test_report_t *report_out) {
  cldb_test_report_t report;
  cldb_log_reader_t *reader = NULL;
  cldb_buf_t scratch;
  cldb_slice_t record;
  size_t matched = 0;

  cldb_test_report_reset(&report);
  CLDB_ASSERT_OK(cldb_log_reader_create(
      file, cldb_test_report_callback, &report, true, 0, &reader));
  cldb_buf_init(&scratch);
  while (cldb_log_reader_read_record(reader, &record, &scratch)) {
    if (matched >= expected_count) {
      CLDB_EXPECT_TRUE(false);
      break;
    }
    CLDB_EXPECT_EQ_UINT(expected[matched].size, record.size);
    if (record.size == expected[matched].size && record.size > 0) {
      CLDB_EXPECT_EQ_INT(
          0, memcmp(record.data, expected[matched].data, record.size));
    }
    matched += 1;
  }
  cldb_buf_free(&scratch);
  cldb_log_reader_destroy(reader);
  if (report_out != NULL) {
    *report_out = report;
  }
  return matched;
}

/* 内存 WAL 版 read_from_file；仅用于无需真实文件的矩阵测试。 */
static size_t read_prefix(cldb_slice_t wal, const cldb_slice_t *expected,
                          size_t expected_count,
                          cldb_test_report_t *report_out) {
  cldb_test_string_source_t source;
  cldb_test_report_t report;

  cldb_test_string_source_init(&source);
  cldb_test_string_source_reset(&source, wal);
  {
    size_t matched = read_from_file(&source.base, expected, expected_count,
                                    &report);
    if (report_out != NULL) {
      *report_out = report;
    }
    return matched;
  }
}

/* 期望恢复前缀 prefix_count 条，之后 EOF 且没有额外记录。 */
static void check_prefix_only(cldb_slice_t wal, const cldb_slice_t *prefix,
                              size_t prefix_count,
                              cldb_test_report_t *report_out) {
  cldb_test_report_t report;
  size_t matched = read_prefix(wal, prefix, prefix_count, &report);

  CLDB_EXPECT_EQ_UINT(prefix_count, matched);
  if (report_out != NULL) {
    *report_out = report;
  }
}

/* 截断到 cut 字节；应恢复 ends <= cut 的记录，且尾部截断静默丢弃。 */
static void check_truncation(cldb_slice_t wal, const cldb_slice_t *records,
                             size_t count, const size_t *ends, size_t cut) {
  cldb_test_report_t report;
  size_t expected = 0;
  size_t matched;

  CLDB_ASSERT_TRUE(cut <= wal.size);
  while (expected < count && ends[expected] <= cut) {
    expected += 1;
  }
  matched = read_prefix(cldb_slice_make(wal.data, cut), records, count,
                        &report);
  CLDB_EXPECT_EQ_UINT(expected, matched);
  CLDB_EXPECT_EQ_UINT(0, report.dropped_bytes);
}

/* 采集边界候选，重复无妨；超出 limit 或容量时忽略。 */
static void append_sample(size_t *samples, size_t *count, size_t capacity,
                          size_t value, size_t limit) {
  if (value <= limit && *count < capacity) {
    samples[*count] = value;
    *count += 1;
  }
}

CLDB_TEST(log_compat_golden_vectors) {
  cldb_slice_t empty[1];
  cldb_slice_t hello[1];
  cldb_slice_t four[4];

  empty[0] = cldb_slice_empty();
  hello[0] = cldb_slice_make("hello", 5);
  four[0] = cldb_slice_make("foo", 3);
  four[1] = cldb_slice_make("bar", 3);
  four[2] = cldb_slice_empty();
  four[3] = cldb_slice_make("xxxx", 4);

  expect_golden(empty, 1, kGoldenEmpty,
                sizeof(kGoldenEmpty) / sizeof(kGoldenEmpty[0]));
  expect_golden(hello, 1, kGoldenHello,
                sizeof(kGoldenHello) / sizeof(kGoldenHello[0]));
  expect_golden(four, 4, kGoldenFour,
                sizeof(kGoldenFour) / sizeof(kGoldenFour[0]));
}

CLDB_TEST(log_compat_encoding_differential) {
  static const size_t kBoundaryLengths[] = {
      0, 1, 6, 7, 8, 32753, 32754, 1, 32760, 8,
      32761, 32762, 0, 32768, 32769, 65536, 65537,
  };
  cldb_slice_t simple[6];
  cldb_slice_t records[kMaxCompatRecords];
  cldb_buf_t payload;
  size_t count = sizeof(kBoundaryLengths) / sizeof(kBoundaryLengths[0]);

  simple[0] = cldb_slice_empty();
  simple[1] = cldb_slice_make("hello", 5);
  simple[2] = cldb_slice_make("foo", 3);
  simple[3] = cldb_slice_make("bar", 3);
  simple[4] = cldb_slice_empty();
  simple[5] = cldb_slice_make("xxxx", 4);
  expect_encodings_equal(simple, 6);

  cldb_buf_init(&payload);
  if (!make_pattern_records(kBoundaryLengths, count, &payload, records)) {
    CLDB_EXPECT_TRUE(false);
  } else {
    expect_encodings_equal(records, count);
  }
  cldb_buf_free(&payload);
}

CLDB_TEST(log_compat_encoding_random) {
  cldb_random_t rnd;
  cldb_slice_t records[kMaxCompatRecords];
  size_t lengths[kMaxCompatRecords];
  cldb_buf_t payload;
  size_t round;

  cldb_buf_init(&payload);
  cldb_random_init(&rnd, 204u);
  for (round = 0; round < 4; ++round) {
    size_t i;
    size_t count = 32;

    for (i = 0; i < count; ++i) {
      uint32_t pick = cldb_random_uniform(&rnd, 12u);

      if (pick == 0) {
        lengths[i] = 0;
      } else if (pick == 1) {
        lengths[i] = 32750 + (size_t)cldb_random_uniform(&rnd, 20u);
      } else if (pick == 2) {
        lengths[i] = 32768 + (size_t)cldb_random_uniform(&rnd, 16385u);
      } else {
        lengths[i] = (size_t)cldb_random_skewed(&rnd, 15u);
      }
    }
    if (!make_pattern_records(lengths, count, &payload, records)) {
      CLDB_EXPECT_TRUE(false);
      break;
    }
    expect_encodings_equal(records, count);
  }
  cldb_buf_free(&payload);
}

CLDB_TEST(log_compat_truncation_matrix) {
  static const size_t kLengths[] = {0, 1, 5, 6, 7, 8, 300};
  cldb_slice_t records[sizeof(kLengths) / sizeof(kLengths[0])];
  cldb_test_string_file_t file;
  cldb_buf_t reference;
  cldb_buf_t payload;
  size_t ends[sizeof(kLengths) / sizeof(kLengths[0])];
  size_t end_count = 0;
  size_t cut;
  bool encoded;

  cldb_buf_init(&reference);
  cldb_buf_init(&payload);
  if (!make_pattern_records(kLengths,
                            sizeof(kLengths) / sizeof(kLengths[0]),
                            &payload, records)) {
    CLDB_EXPECT_TRUE(false);
    cldb_buf_free(&payload);
    cldb_buf_free(&reference);
    return;
  }
  encoded = encode_layout(records,
                          sizeof(kLengths) / sizeof(kLengths[0]), &file,
                          &reference, ends, &end_count);
  CLDB_EXPECT_TRUE(encoded);
  if (encoded) {
    for (cut = 0; cut <= file.data.size; ++cut) {
      check_truncation(cldb_buf_slice(&file.data), records,
                       sizeof(kLengths) / sizeof(kLengths[0]), ends, cut);
    }
  }
  cldb_test_string_file_free(&file);
  cldb_buf_free(&payload);
  cldb_buf_free(&reference);
}

CLDB_TEST(log_compat_truncation_cross_block) {
  static const size_t kLengths[] = {3, 32754, 3};
  cldb_slice_t records[sizeof(kLengths) / sizeof(kLengths[0])];
  cldb_test_string_file_t file;
  cldb_buf_t reference;
  cldb_buf_t payload;
  size_t ends[sizeof(kLengths) / sizeof(kLengths[0])];
  size_t end_count = 0;
  size_t samples[512];
  size_t sample_count = 0;
  size_t i;
  size_t d;
  size_t cut;
  bool encoded;

  cldb_buf_init(&reference);
  cldb_buf_init(&payload);
  if (!make_pattern_records(kLengths,
                            sizeof(kLengths) / sizeof(kLengths[0]),
                            &payload, records)) {
    CLDB_EXPECT_TRUE(false);
    cldb_buf_free(&payload);
    cldb_buf_free(&reference);
    return;
  }
  encoded = encode_layout(records,
                          sizeof(kLengths) / sizeof(kLengths[0]), &file,
                          &reference, ends, &end_count);
  CLDB_EXPECT_TRUE(encoded);
  if (encoded) {
    size_t wal_size = file.data.size;

    for (i = 0; i <= 16 && i <= wal_size; ++i) {
      append_sample(samples, &sample_count,
                    sizeof(samples) / sizeof(samples[0]), i, wal_size);
    }
    for (i = 0; i < end_count; ++i) {
      for (d = 0; d <= 16; ++d) {
        if (ends[i] >= d) {
          append_sample(samples, &sample_count,
                        sizeof(samples) / sizeof(samples[0]), ends[i] - d,
                        wal_size);
        }
        if (wal_size - ends[i] >= d) {
          append_sample(samples, &sample_count,
                        sizeof(samples) / sizeof(samples[0]), ends[i] + d,
                        wal_size);
        }
      }
    }
    for (cut = CLDB_LOG_BLOCK_SIZE; cut <= wal_size;
         cut += CLDB_LOG_BLOCK_SIZE) {
      for (d = 0; d <= 16; ++d) {
        if (cut >= d) {
          append_sample(samples, &sample_count,
                        sizeof(samples) / sizeof(samples[0]), cut - d,
                        wal_size);
        }
        if (wal_size - cut >= d) {
          append_sample(samples, &sample_count,
                        sizeof(samples) / sizeof(samples[0]), cut + d,
                        wal_size);
        }
      }
    }
    for (d = 0; d <= 16 && d <= wal_size; ++d) {
      append_sample(samples, &sample_count,
                    sizeof(samples) / sizeof(samples[0]), wal_size - d,
                    wal_size);
    }
    {
      size_t stride = wal_size / 64 + 1;

      for (cut = 0; cut <= wal_size; cut += stride) {
        append_sample(samples, &sample_count,
                      sizeof(samples) / sizeof(samples[0]), cut, wal_size);
      }
    }
    for (i = 0; i < sample_count; ++i) {
      check_truncation(cldb_buf_slice(&file.data), records,
                       sizeof(kLengths) / sizeof(kLengths[0]), ends,
                       samples[i]);
    }
  }
  cldb_test_string_file_free(&file);
  cldb_buf_free(&payload);
  cldb_buf_free(&reference);
}

CLDB_TEST(log_compat_bit_flip_matrix) {
  static const size_t kLastLength = 100;
  cldb_slice_t records[3];
  cldb_slice_t prefix[2];
  cldb_test_string_file_t file;
  cldb_buf_t reference;
  cldb_buf_t payload;
  cldb_buf_t mutated;
  size_t ends[3];
  size_t end_count = 0;
  size_t last_header;
  size_t offset;
  unsigned int bit;
  bool encoded;

  cldb_test_string_file_init(&file);
  cldb_buf_init(&reference);
  cldb_buf_init(&payload);
  cldb_buf_init(&mutated);
  if (!cldb_buf_resize(&payload, kLastLength)) {
    CLDB_EXPECT_TRUE(false);
    goto done;
  }
  cldb_test_fill_pattern(payload.data, kLastLength);
  records[0] = cldb_slice_make("first", 5);
  records[1] = cldb_slice_make("second", 6);
  records[2] = cldb_slice_make(payload.data, kLastLength);
  prefix[0] = records[0];
  prefix[1] = records[1];
  encoded = encode_layout(records, 3, &file, &reference, ends, &end_count);
  CLDB_EXPECT_TRUE(encoded);
  if (!encoded) {
    goto done;
  }
  last_header = file.data.size - (CLDB_LOG_HEADER_SIZE + kLastLength);
  for (offset = last_header; offset < file.data.size; ++offset) {
    for (bit = 0; bit < 8; ++bit) {
      cldb_test_report_t report;

      cldb_buf_assign(&mutated, file.data.data, file.data.size);
      cldb_test_flip_bit(&mutated, offset, bit);
      check_prefix_only(cldb_buf_slice(&mutated), prefix, 2, &report);
      if (offset != last_header + CLDB_LOG_LENGTH_OFFSET &&
          offset != last_header + CLDB_LOG_LENGTH_OFFSET + 1) {
        CLDB_EXPECT_TRUE(report.dropped_bytes > 0);
      }
    }
  }

done:
  cldb_test_string_file_free(&file);
  cldb_buf_free(&payload);
  cldb_buf_free(&mutated);
  cldb_buf_free(&reference);
}

CLDB_TEST(log_compat_bit_flip_cross_block) {
  static const size_t kLengths[] = {1, 1, 32768};
  cldb_slice_t records[3];
  cldb_slice_t prefix[2];
  cldb_test_string_file_t file;
  cldb_buf_t reference;
  cldb_buf_t payload;
  cldb_buf_t mutated;
  size_t ends[3];
  size_t end_count = 0;
  size_t first_fragment;
  size_t size;
  size_t range_start[3];
  size_t range_end[3];
  size_t r;
  unsigned int bit;
  bool encoded;

  cldb_test_string_file_init(&file);
  cldb_buf_init(&reference);
  cldb_buf_init(&payload);
  cldb_buf_init(&mutated);
  if (!make_pattern_records(kLengths, 3, &payload, records)) {
    CLDB_EXPECT_TRUE(false);
    goto done;
  }
  prefix[0] = records[0];
  prefix[1] = records[1];
  encoded = encode_layout(records, 3, &file, &reference, ends, &end_count);
  CLDB_EXPECT_TRUE(encoded);
  if (!encoded) {
    goto done;
  }
  size = file.data.size;
  first_fragment = ends[1];
  range_start[0] = first_fragment;
  range_end[0] = first_fragment + 9;
  range_start[1] = (size_t)CLDB_LOG_BLOCK_SIZE - 9;
  range_end[1] = (size_t)CLDB_LOG_BLOCK_SIZE + 9;
  range_start[2] = size - 9;
  range_end[2] = size;
  for (r = 0; r < 3; ++r) {
    size_t offset;

    for (offset = range_start[r]; offset < range_end[r] && offset < size;
         ++offset) {
      for (bit = 0; bit < 8; ++bit) {
        cldb_buf_assign(&mutated, file.data.data, size);
        cldb_test_flip_bit(&mutated, offset, bit);
        check_prefix_only(cldb_buf_slice(&mutated), prefix, 2, NULL);
      }
    }
  }

done:
  cldb_test_string_file_free(&file);
  cldb_buf_free(&payload);
  cldb_buf_free(&mutated);
  cldb_buf_free(&reference);
}

/* 物理 type 5/6 与 Reader 内部的 EOF/BAD_RECORD 哨兵同值，按上游行为也走
 * 哨兵分支；recovered 记录该类型下期望恢复的记录数。 */
typedef struct type_case {
  unsigned int type;
  uint64_t dropped_bytes;
  const char *message; /* NULL 表示不应上报 */
  size_t recovered;
} type_case_t;

static const type_case_t kTailTypeCases[] = {
    {(unsigned int)CLDB_LOG_RECORD_TYPE_ZERO, 100, "unknown record type 0", 2},
    {(unsigned int)CLDB_LOG_RECORD_TYPE_FIRST, 0, NULL, 2},
    {(unsigned int)CLDB_LOG_RECORD_TYPE_MIDDLE, 100,
     "missing start of fragmented record(1)", 2},
    {(unsigned int)CLDB_LOG_RECORD_TYPE_LAST, 100,
     "missing start of fragmented record(2)", 2},
    {5, 0, NULL, 2},
    {6, 0, NULL, 2},
    {255, 100, "unknown record type 255", 2},
};

static const type_case_t kInteriorTypeCases[] = {
    {(unsigned int)CLDB_LOG_RECORD_TYPE_ZERO, 6, "unknown record type 0", 2},
    {(unsigned int)CLDB_LOG_RECORD_TYPE_FIRST, 6,
     "partial record without end(1)", 2},
    {(unsigned int)CLDB_LOG_RECORD_TYPE_MIDDLE, 6,
     "missing start of fragmented record(1)", 2},
    {(unsigned int)CLDB_LOG_RECORD_TYPE_LAST, 6,
     "missing start of fragmented record(2)", 2},
    {5, 0, NULL, 1},
    {6, 0, NULL, 2},
    {255, 6, "unknown record type 255", 2},
};

/* 改写最后一条记录的 type 并修正 CRC；前缀必须完好。 */
CLDB_TEST(log_compat_type_corruption_tail) {
  static const size_t kLastLength = 100;
  cldb_slice_t records[3];
  cldb_slice_t prefix[2];
  cldb_test_string_file_t file;
  cldb_buf_t reference;
  cldb_buf_t payload;
  cldb_buf_t mutated;
  size_t ends[3];
  size_t end_count = 0;
  size_t last_header;
  size_t i;
  bool encoded;

  cldb_test_string_file_init(&file);
  cldb_buf_init(&reference);
  cldb_buf_init(&payload);
  cldb_buf_init(&mutated);
  if (!cldb_buf_resize(&payload, kLastLength)) {
    CLDB_EXPECT_TRUE(false);
    goto done;
  }
  cldb_test_fill_pattern(payload.data, kLastLength);
  records[0] = cldb_slice_make("first", 5);
  records[1] = cldb_slice_make("second", 6);
  records[2] = cldb_slice_make(payload.data, kLastLength);
  prefix[0] = records[0];
  prefix[1] = records[1];
  encoded = encode_layout(records, 3, &file, &reference, ends, &end_count);
  CLDB_EXPECT_TRUE(encoded);
  if (!encoded) {
    goto done;
  }
  last_header = file.data.size - (CLDB_LOG_HEADER_SIZE + kLastLength);
  for (i = 0; i < sizeof(kTailTypeCases) / sizeof(kTailTypeCases[0]); ++i) {
    cldb_test_report_t report;

    cldb_buf_assign(&mutated, file.data.data, file.data.size);
    cldb_test_set_byte(&mutated, last_header + CLDB_LOG_TYPE_OFFSET,
                       (uint8_t)kTailTypeCases[i].type);
    cldb_test_fix_crc(&mutated, last_header, kLastLength);
    check_prefix_only(cldb_buf_slice(&mutated), prefix,
                      kTailTypeCases[i].recovered, &report);
    CLDB_EXPECT_EQ_UINT(kTailTypeCases[i].dropped_bytes,
                        report.dropped_bytes);
    if (kTailTypeCases[i].message != NULL) {
      CLDB_EXPECT_TRUE(
          cldb_test_report_matches(&report, kTailTypeCases[i].message));
    }
  }

done:
  cldb_test_string_file_free(&file);
  cldb_buf_free(&payload);
  cldb_buf_free(&mutated);
  cldb_buf_free(&reference);
}

/* 改写中间记录的 type：应跳过该记录，并继续恢复后续记录。 */
CLDB_TEST(log_compat_type_corruption_interior) {
  cldb_slice_t records[3];
  cldb_slice_t expected[2];
  cldb_test_string_file_t file;
  cldb_buf_t reference;
  cldb_buf_t mutated;
  size_t ends[3];
  size_t end_count = 0;
  size_t middle_header;
  size_t i;
  bool encoded;

  cldb_buf_init(&reference);
  cldb_buf_init(&mutated);
  records[0] = cldb_slice_make("first", 5);
  records[1] = cldb_slice_make("second", 6);
  records[2] = cldb_slice_make("third", 5);
  expected[0] = records[0];
  expected[1] = records[2];
  encoded = encode_layout(records, 3, &file, &reference, ends, &end_count);
  CLDB_EXPECT_TRUE(encoded);
  if (!encoded) {
    goto done;
  }
  middle_header = CLDB_LOG_HEADER_SIZE + 5;
  for (i = 0;
       i < sizeof(kInteriorTypeCases) / sizeof(kInteriorTypeCases[0]); ++i) {
    cldb_test_report_t report;

    cldb_buf_assign(&mutated, file.data.data, file.data.size);
    cldb_test_set_byte(&mutated, middle_header + CLDB_LOG_TYPE_OFFSET,
                       (uint8_t)kInteriorTypeCases[i].type);
    cldb_test_fix_crc(&mutated, middle_header, 6);
    check_prefix_only(cldb_buf_slice(&mutated), expected,
                      kInteriorTypeCases[i].recovered, &report);
    CLDB_EXPECT_EQ_UINT(kInteriorTypeCases[i].dropped_bytes,
                        report.dropped_bytes);
    if (kInteriorTypeCases[i].message != NULL) {
      CLDB_EXPECT_TRUE(
          cldb_test_report_matches(&report, kInteriorTypeCases[i].message));
    }
  }

done:
  cldb_test_string_file_free(&file);
  cldb_buf_free(&mutated);
  cldb_buf_free(&reference);
}

/* 读取被截断/随机改写过的 WAL；返回的每条记录必须是原序列中顺序靠后的
 * 完整记录，索引不得回退。 */
static void check_fuzz_recovery(cldb_slice_t wal,
                                const cldb_slice_t *records, size_t count) {
  cldb_test_string_source_t source;
  cldb_log_reader_t *reader = NULL;
  cldb_buf_t scratch;
  cldb_slice_t record;
  size_t next = 0;
  uint64_t previous_offset = 0;

  cldb_test_string_source_init(&source);
  cldb_test_string_source_reset(&source, wal);
  CLDB_ASSERT_OK(cldb_log_reader_create(
      &source.base, NULL, NULL, true, 0, &reader));
  cldb_buf_init(&scratch);
  while (cldb_log_reader_read_record(reader, &record, &scratch)) {
    size_t i;
    bool found = false;

    for (i = next; i < count; ++i) {
      if (records[i].size == record.size &&
          (record.size == 0 ||
           memcmp(records[i].data, record.data, record.size) == 0)) {
        next = i + 1;
        found = true;
        break;
      }
    }
    if (!found) {
      CLDB_EXPECT_TRUE(false);
      break;
    }
    {
      uint64_t offset = cldb_log_reader_last_record_offset(reader);

      CLDB_EXPECT_TRUE(offset >= previous_offset);
      CLDB_EXPECT_TRUE(offset <= (uint64_t)wal.size);
      previous_offset = offset;
    }
  }
  cldb_buf_free(&scratch);
  cldb_log_reader_destroy(reader);
}

CLDB_TEST(log_compat_fuzz_recovery) {
  cldb_random_t rnd;
  cldb_slice_t records[32];
  size_t lengths[32];
  cldb_test_string_file_t file;
  cldb_buf_t payload;
  cldb_buf_t mutated;
  size_t i;
  size_t iteration;

  cldb_buf_init(&payload);
  cldb_buf_init(&mutated);
  cldb_random_init(&rnd, 204u);
  lengths[0] = 4;
  lengths[1] = 32754;
  lengths[2] = 32768;
  lengths[3] = 8;
  for (i = 4; i < 32; ++i) {
    lengths[i] = 4 + (size_t)cldb_random_skewed(&rnd, 14u);
  }
  if (!make_unique_pattern_records(lengths, 32, &payload, records)) {
    CLDB_EXPECT_TRUE(false);
    cldb_buf_free(&payload);
    cldb_buf_free(&mutated);
    return;
  }
  encode_with_writer(&file, records, 32);
  for (iteration = 0; iteration < 200; ++iteration) {
    size_t cut;
    uint32_t mutations;
    uint32_t m;

    cldb_buf_assign(&mutated, file.data.data, file.data.size);
    if (iteration % 3 == 0) {
      cut = file.data.size;
    } else {
      cut = (size_t)cldb_random_uniform(
          &rnd, (uint32_t)(file.data.size + 1));
    }
    if (cut < mutated.size) {
      CLDB_ASSERT_TRUE(cldb_buf_resize(&mutated, cut));
    }
    mutations = cldb_random_uniform(&rnd, 4u) + 1u;
    for (m = 0; m < mutations && mutated.size > 0; ++m) {
      size_t offset = (size_t)cldb_random_uniform(
          &rnd, (uint32_t)mutated.size);
      uint8_t value = (uint8_t)(cldb_random_next(&rnd) & 0xffu);

      cldb_test_set_byte(&mutated, offset, value);
    }
    check_fuzz_recovery(cldb_buf_slice(&mutated), records, 32);
  }
  cldb_test_string_file_free(&file);
  cldb_buf_free(&payload);
  cldb_buf_free(&mutated);
}

/* 可选交叉验证：CLDB_LOG_COMPAT_WAL 读取上游生成的 WAL，
 * CLDB_LOG_COMPAT_OUT 写出本实现的 WAL 供上游读取。
 * 记录序列为 "hello"、空记录、"foo"、32768 个 'x'、"bar"。 */
static void check_external_file(cldb_env_t *env, const char *path,
                                const cldb_slice_t *expected, size_t count) {
  cldb_sequential_file_t *file = NULL;
  cldb_test_report_t report;
  cldb_status_t status;
  size_t matched;

  status = cldb_env_new_sequential_file(env, path, &file);
  CLDB_EXPECT_STATUS(CLDB_STATUS_OK, status);
  if (file == NULL) {
    return;
  }
  matched = read_from_file(file, expected, count, &report);
  CLDB_EXPECT_EQ_UINT(count, matched);
  CLDB_EXPECT_EQ_UINT(0, report.dropped_bytes);
  cldb_sequential_file_destroy(file);
}

static bool write_external_file(cldb_env_t *env, const char *path,
                                const cldb_slice_t *records, size_t count) {
  cldb_writable_file_t *file = NULL;
  cldb_log_writer_t *writer = NULL;
  cldb_status_t status;
  size_t i;
  bool ok = true;

  status = cldb_env_new_writable_file(env, path, &file);
  CLDB_EXPECT_STATUS(CLDB_STATUS_OK, status);
  if (file == NULL) {
    return false;
  }
  status = cldb_log_writer_create(file, &writer);
  CLDB_EXPECT_STATUS(CLDB_STATUS_OK, status);
  if (writer == NULL) {
    cldb_writable_file_destroy(file);
    return false;
  }
  for (i = 0; i < count; ++i) {
    status = cldb_log_writer_add_record(writer, records[i]);
    if (!cldb_status_is_ok(status)) {
      CLDB_EXPECT_STATUS(CLDB_STATUS_OK, status);
      ok = false;
      break;
    }
  }
  cldb_log_writer_destroy(writer);
  cldb_writable_file_destroy(file);
  return ok;
}

CLDB_TEST(log_compat_external_optional) {
  const char *wal_path = getenv("CLDB_LOG_COMPAT_WAL");
  const char *out_path = getenv("CLDB_LOG_COMPAT_OUT");
  cldb_env_t *env;
  char *big;
  cldb_slice_t expected[5];

  big = malloc((size_t)kExternalBigSize);
  if (big == NULL) {
    CLDB_EXPECT_TRUE(false);
    return;
  }
  memset(big, 'x', (size_t)kExternalBigSize);
  expected[0] = cldb_slice_make("hello", 5);
  expected[1] = cldb_slice_empty();
  expected[2] = cldb_slice_make("foo", 3);
  expected[3] = cldb_slice_make(big, (size_t)kExternalBigSize);
  expected[4] = cldb_slice_make("bar", 3);
  env = cldb_env_default();
  if (wal_path != NULL && wal_path[0] != '\0') {
    check_external_file(env, wal_path, expected, 5);
  }
  if (out_path != NULL && out_path[0] != '\0') {
    if (write_external_file(env, out_path, expected, 5)) {
      check_external_file(env, out_path, expected, 5);
    }
  }
  free(big);
}

void cldb_register_log_compat_tests(void) {
  CLDB_RUN_TEST(log_compat_golden_vectors);
  CLDB_RUN_TEST(log_compat_encoding_differential);
  CLDB_RUN_TEST(log_compat_encoding_random);
  CLDB_RUN_TEST(log_compat_truncation_matrix);
  CLDB_RUN_TEST(log_compat_truncation_cross_block);
  CLDB_RUN_TEST(log_compat_bit_flip_matrix);
  CLDB_RUN_TEST(log_compat_bit_flip_cross_block);
  CLDB_RUN_TEST(log_compat_type_corruption_tail);
  CLDB_RUN_TEST(log_compat_type_corruption_interior);
  CLDB_RUN_TEST(log_compat_fuzz_recovery);
  CLDB_RUN_TEST(log_compat_external_optional);
}
