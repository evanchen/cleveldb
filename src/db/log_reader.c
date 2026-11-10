#include "db/log_reader.h"

#include <stdio.h>
#include <stdlib.h>

#include "util/coding.h"
#include "util/crc32c.h"

/* ReadPhysicalRecord 的返回类型：正常记录返回记录类型，另加 EOF 与坏记录。 */
enum {
  CLDB_LOG_READER_EOF = CLDB_LOG_MAX_RECORD_TYPE + 1,
  CLDB_LOG_READER_BAD_RECORD = CLDB_LOG_MAX_RECORD_TYPE + 2
};

/* 上报丢弃；对应上游 ReportDrop。缓冲末尾位置不早于 initial_offset 才上报，
 * 避免重复报告初始偏移之前的数据。 */
static void report_drop(cldb_log_reader_t *reader, uint64_t bytes,
                        cldb_status_t reason) {
  if (reader->reporter == NULL) {
    return;
  }
  if (reader->end_of_buffer_offset - (uint64_t)reader->buffer.size - bytes >=
      reader->initial_offset) {
    reader->reporter(reader->reporter_arg, bytes, reason);
  }
}

/* 构造 Corruption 状态并上报；对应上游 ReportCorruption。 */
static void report_corruption(cldb_log_reader_t *reader, uint64_t bytes,
                              const char *reason) {
  report_drop(reader, bytes, cldb_status_corruption(reason, NULL));
}

/* 跳过 initial_offset 之前的块；若落在块尾不足一个 header 的区域，则跳到
 * 下一块。对应上游 SkipToInitialBlock。 */
static bool skip_to_initial_block(cldb_log_reader_t *reader) {
  size_t offset_in_block =
      (size_t)(reader->initial_offset % (uint64_t)CLDB_LOG_BLOCK_SIZE);
  uint64_t block_start = reader->initial_offset - (uint64_t)offset_in_block;

  /* 块尾填充区最多为 header_size - 1 字节；用该长度判断是否跳过本块。 */
  if (offset_in_block >
      (size_t)(CLDB_LOG_BLOCK_SIZE - (CLDB_LOG_HEADER_SIZE - 1))) {
    block_start += (uint64_t)CLDB_LOG_BLOCK_SIZE;
  }
  reader->end_of_buffer_offset = block_start;
  if (block_start > 0) {
    cldb_status_t status =
        cldb_sequential_file_skip(reader->file, block_start);
    if (!cldb_status_is_ok(status)) {
      report_drop(reader, block_start, status);
      return false;
    }
  }
  return true;
}

/* 读取一条物理记录。返回记录类型、EOF 或 BAD；成功时 result 指向当前块缓冲
 * 中的 payload。对应上游 ReadPhysicalRecord。 */
static unsigned int read_physical_record(cldb_log_reader_t *reader,
                                         cldb_slice_t *result) {
  size_t drop_size;

  *result = cldb_slice_empty();
  for (;;) {
    /* 块尾不足 header 时丢弃剩余字节并换块；最后一块读不满则视为 EOF。 */
    if (reader->buffer.size < (size_t)CLDB_LOG_HEADER_SIZE) {
      if (!reader->eof) {
        reader->buffer = cldb_slice_empty();
        {
          cldb_status_t status = cldb_sequential_file_read(
              reader->file, (size_t)CLDB_LOG_BLOCK_SIZE, &reader->buffer,
              reader->backing_store);
          reader->end_of_buffer_offset += (uint64_t)reader->buffer.size;
          if (!cldb_status_is_ok(status)) {
            reader->buffer = cldb_slice_empty();
            report_drop(reader, (uint64_t)CLDB_LOG_BLOCK_SIZE, status);
            reader->eof = true;
            return CLDB_LOG_READER_EOF;
          }
        }
        if (reader->buffer.size < (size_t)CLDB_LOG_BLOCK_SIZE) {
          reader->eof = true;
        }
        continue;
      }
      reader->buffer = cldb_slice_empty();
      return CLDB_LOG_READER_EOF;
    }

    {
      const char *header = reader->buffer.data;
      uint32_t length =
          (uint32_t)(unsigned char)header[CLDB_LOG_LENGTH_OFFSET] |
          ((uint32_t)(unsigned char)header[CLDB_LOG_LENGTH_OFFSET + 1] << 8);
      unsigned int type =
          (unsigned int)(unsigned char)header[CLDB_LOG_TYPE_OFFSET];

      if ((size_t)CLDB_LOG_HEADER_SIZE + (size_t)length >
          reader->buffer.size) {
        /* 剩余字节不足以容纳声明的 payload；最后一块可能是单纯截断。 */
        drop_size = reader->buffer.size;
        reader->buffer = cldb_slice_empty();
        if (!reader->eof) {
          report_corruption(reader, (uint64_t)drop_size, "bad record length");
          return CLDB_LOG_READER_BAD_RECORD;
        }
        return CLDB_LOG_READER_EOF;
      }

      if (type == (unsigned int)CLDB_LOG_RECORD_TYPE_ZERO && length == 0) {
        /* 预分配文件中的空块；静默跳过。 */
        reader->buffer = cldb_slice_empty();
        return CLDB_LOG_READER_BAD_RECORD;
      }

      if (reader->checksum) {
        uint32_t expected_crc =
            cldb_crc32c_unmask(cldb_decode_fixed32(header));
        uint32_t actual_crc = cldb_crc32c_value(
            header + CLDB_LOG_TYPE_OFFSET, (size_t)length + 1);
        if (expected_crc != actual_crc) {
          drop_size = reader->buffer.size;
          reader->buffer = cldb_slice_empty();
          report_corruption(reader, (uint64_t)drop_size, "checksum mismatch");
          return CLDB_LOG_READER_BAD_RECORD;
        }
      }

      cldb_slice_remove_prefix(
          &reader->buffer,
          (size_t)CLDB_LOG_HEADER_SIZE + (size_t)length);
      {
        uint64_t physical_start =
            reader->end_of_buffer_offset - (uint64_t)reader->buffer.size -
            (uint64_t)CLDB_LOG_HEADER_SIZE - (uint64_t)length;
        if (physical_start < reader->initial_offset) {
          *result = cldb_slice_empty();
          return CLDB_LOG_READER_BAD_RECORD;
        }
      }
      *result =
          cldb_slice_make(header + CLDB_LOG_HEADER_SIZE, (size_t)length);
      return type;
    }
  }
}

/* 追加到 scratch；失败时上报内存不足。 */
static bool append_to_scratch(cldb_log_reader_t *reader, cldb_buf_t *scratch,
                              cldb_slice_t fragment) {
  if (cldb_buf_append(scratch, fragment.data, fragment.size)) {
    return true;
  }
  report_drop(reader, (uint64_t)scratch->size,
              cldb_status_io("cldb_log_reader", "out of memory"));
  return false;
}

cldb_status_t cldb_log_reader_create(cldb_sequential_file_t *file,
                                     cldb_log_reader_reporter_fn_t reporter,
                                     void *reporter_arg, bool checksum,
                                     uint64_t initial_offset,
                                     cldb_log_reader_t **out) {
  cldb_log_reader_t *reader;

  if (out == NULL) {
    return cldb_status_invalid_argument("cldb_log_reader_create",
                                        "out is NULL");
  }
  *out = NULL;
  if (file == NULL) {
    return cldb_status_invalid_argument("cldb_log_reader_create",
                                        "file is NULL");
  }

  reader = calloc(1, sizeof(*reader));
  if (reader == NULL) {
    return cldb_status_io("out of memory", NULL);
  }
  reader->file = file;
  reader->reporter = reporter;
  reader->reporter_arg = reporter_arg;
  reader->checksum = checksum;
  reader->buffer = cldb_slice_empty();
  reader->initial_offset = initial_offset;
  reader->resyncing = initial_offset > 0;
  *out = reader;
  return cldb_status_ok();
}

void cldb_log_reader_destroy(cldb_log_reader_t *reader) { free(reader); }

bool cldb_log_reader_read_record(cldb_log_reader_t *reader,
                                 cldb_slice_t *record,
                                 cldb_buf_t *scratch) {
  bool in_fragmented_record = false;
  uint64_t prospective_record_offset = 0;

  if (reader == NULL || record == NULL || scratch == NULL) {
    return false;
  }
  *record = cldb_slice_empty();
  if (reader->last_record_offset < reader->initial_offset) {
    if (!skip_to_initial_block(reader)) {
      return false;
    }
  }
  cldb_buf_clear(scratch);

  for (;;) {
    cldb_slice_t fragment = cldb_slice_empty();
    unsigned int record_type = read_physical_record(reader, &fragment);
    uint64_t physical_record_offset =
        reader->end_of_buffer_offset - (uint64_t)reader->buffer.size -
        (uint64_t)CLDB_LOG_HEADER_SIZE - (uint64_t)fragment.size;

    if (reader->resyncing) {
      /* 初始偏移后先丢弃不完整的尾部，直到遇到 FULL/FIRST。 */
      if (record_type == (unsigned int)CLDB_LOG_RECORD_TYPE_MIDDLE) {
        continue;
      }
      if (record_type == (unsigned int)CLDB_LOG_RECORD_TYPE_LAST) {
        reader->resyncing = false;
        continue;
      }
      reader->resyncing = false;
    }

    switch (record_type) {
      case CLDB_LOG_RECORD_TYPE_FULL:
        if (in_fragmented_record && scratch->size > 0) {
          report_corruption(reader, (uint64_t)scratch->size,
                            "partial record without end(1)");
        }
        cldb_buf_clear(scratch);
        prospective_record_offset = physical_record_offset;
        reader->last_record_offset = prospective_record_offset;
        *record = fragment;
        return true;

      case CLDB_LOG_RECORD_TYPE_FIRST:
        if (in_fragmented_record && scratch->size > 0) {
          report_corruption(reader, (uint64_t)scratch->size,
                            "partial record without end(2)");
        }
        prospective_record_offset = physical_record_offset;
        cldb_buf_clear(scratch);
        if (!append_to_scratch(reader, scratch, fragment)) {
          return false;
        }
        in_fragmented_record = true;
        break;

      case CLDB_LOG_RECORD_TYPE_MIDDLE:
        if (!in_fragmented_record) {
          report_corruption(reader, (uint64_t)fragment.size,
                            "missing start of fragmented record(1)");
        } else if (!append_to_scratch(reader, scratch, fragment)) {
          return false;
        }
        break;

      case CLDB_LOG_RECORD_TYPE_LAST:
        if (!in_fragmented_record) {
          report_corruption(reader, (uint64_t)fragment.size,
                            "missing start of fragmented record(2)");
        } else {
          if (!append_to_scratch(reader, scratch, fragment)) {
            return false;
          }
          *record = cldb_buf_slice(scratch);
          reader->last_record_offset = prospective_record_offset;
          return true;
        }
        break;

      case CLDB_LOG_READER_EOF:
        if (in_fragmented_record) {
          /* 最后一条记录缺少结尾；末尾截断属于正常情况，静默忽略。 */
          cldb_buf_clear(scratch);
          in_fragmented_record = false;
        }
        return false;

      case CLDB_LOG_READER_BAD_RECORD:
        if (in_fragmented_record) {
          report_corruption(reader, (uint64_t)scratch->size,
                            "error in middle of record");
          cldb_buf_clear(scratch);
          in_fragmented_record = false;
        }
        break;

      default:
        {
          char message[64];

          snprintf(message, sizeof(message), "unknown record type %u",
                   record_type);
          report_corruption(reader, (uint64_t)(scratch->size + fragment.size),
                            message);
          cldb_buf_clear(scratch);
          in_fragmented_record = false;
        }
        break;
    }
  }
}

uint64_t cldb_log_reader_last_record_offset(const cldb_log_reader_t *reader) {
  return reader != NULL ? reader->last_record_offset : 0;
}
