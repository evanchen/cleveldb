#include "db/log_writer.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "port/port.h"
#include "util/coding.h"
#include "util/crc32c.h"

/* 写一条物理记录；对应上游 Writer::EmitPhysicalRecord。length 必须能放进
 * uint16，且 header + payload 不跨块。失败时 block_offset 仍按已尝试写入
 * 推进，与上游一致。 */
static cldb_status_t emit_physical_record(cldb_log_writer_t *writer,
                                          cldb_log_record_type_t type,
                                          const char *ptr, size_t length) {
  char header[CLDB_LOG_HEADER_SIZE];
  uint32_t crc;
  cldb_status_t status;

  CLDB_ASSERT(length <= (size_t)UINT16_MAX);
  CLDB_ASSERT(writer->block_offset >= 0);
  CLDB_ASSERT((size_t)writer->block_offset + CLDB_LOG_HEADER_SIZE + length <=
              (size_t)CLDB_LOG_BLOCK_SIZE);

  header[CLDB_LOG_LENGTH_OFFSET] = (char)(uint8_t)(length & 0xffu);
  header[CLDB_LOG_LENGTH_OFFSET + 1] =
      (char)(uint8_t)((length >> 8) & 0xffu);
  header[CLDB_LOG_TYPE_OFFSET] = (char)type;

  /* CRC 以 type_crc[type] 为初值，等价于覆盖 type 字节后再算 payload。 */
  crc = cldb_crc32c_extend(writer->type_crc[type], ptr, length);
  cldb_encode_fixed32(header + CLDB_LOG_CRC_OFFSET, cldb_crc32c_mask(crc));

  status = cldb_writable_file_append(
      writer->dest, cldb_slice_make(header, CLDB_LOG_HEADER_SIZE));
  if (cldb_status_is_ok(status)) {
    status = cldb_writable_file_append(writer->dest,
                                       cldb_slice_make(ptr, length));
    if (cldb_status_is_ok(status)) {
      status = cldb_writable_file_flush(writer->dest);
    }
  }
  writer->block_offset += (int)(CLDB_LOG_HEADER_SIZE + length);
  return status;
}

cldb_status_t cldb_log_writer_create(cldb_writable_file_t *dest,
                                     cldb_log_writer_t **out) {
  return cldb_log_writer_create_with_length(dest, 0, out);
}

cldb_status_t cldb_log_writer_create_with_length(cldb_writable_file_t *dest,
                                                 uint64_t dest_length,
                                                 cldb_log_writer_t **out) {
  cldb_log_writer_t *writer;

  if (out == NULL) {
    return cldb_status_invalid_argument(
        "cldb_log_writer_create_with_length", "out is NULL");
  }
  *out = NULL;
  if (dest == NULL) {
    return cldb_status_invalid_argument(
        "cldb_log_writer_create_with_length", "dest is NULL");
  }

  writer = calloc(1, sizeof(*writer));
  if (writer == NULL) {
    return cldb_status_io("out of memory", NULL);
  }
  writer->dest = dest;
  writer->block_offset =
      (int)(dest_length % (uint64_t)CLDB_LOG_BLOCK_SIZE);
  cldb_log_init_type_crc(writer->type_crc);
  *out = writer;
  return cldb_status_ok();
}

void cldb_log_writer_destroy(cldb_log_writer_t *writer) { free(writer); }

cldb_status_t cldb_log_writer_add_record(cldb_log_writer_t *writer,
                                         cldb_slice_t record) {
  static const char kZeroPadding[CLDB_LOG_HEADER_SIZE - 1] = {0};
  const char *ptr;
  size_t left;
  bool begin = true;
  cldb_status_t status = cldb_status_ok();

  if (writer == NULL) {
    return cldb_status_invalid_argument("cldb_log_writer_add_record",
                                        "writer is NULL");
  }
  if (writer->dest == NULL) {
    return cldb_status_invalid_argument("cldb_log_writer_add_record",
                                        "dest is NULL");
  }
  if (writer->block_offset < 0 ||
      writer->block_offset > CLDB_LOG_BLOCK_SIZE) {
    return cldb_status_invalid_argument("cldb_log_writer_add_record",
                                        "invalid block offset");
  }
  if (record.size > 0 && record.data == NULL) {
    return cldb_status_invalid_argument("cldb_log_writer_add_record",
                                        "record data is NULL");
  }

  ptr = record.data;
  left = record.size;
  do {
    int leftover = CLDB_LOG_BLOCK_SIZE - writer->block_offset;

    /* 块尾放不下 header 时补零换块；整块已满则直接换块。上游忽略补零
     * Append 的错误，C 版直接返回，避免继续写入错位数据。 */
    if (leftover < CLDB_LOG_HEADER_SIZE) {
      if (leftover > 0) {
        status = cldb_writable_file_append(
            writer->dest,
            cldb_slice_make(kZeroPadding, (size_t)leftover));
        if (!cldb_status_is_ok(status)) {
          return status;
        }
      }
      writer->block_offset = 0;
    }

    {
      size_t avail = (size_t)(CLDB_LOG_BLOCK_SIZE - CLDB_LOG_HEADER_SIZE -
                              writer->block_offset);
      size_t fragment_length = left < avail ? left : avail;
      bool end = (left == fragment_length);
      cldb_log_record_type_t type;

      if (begin && end) {
        type = CLDB_LOG_RECORD_TYPE_FULL;
      } else if (begin) {
        type = CLDB_LOG_RECORD_TYPE_FIRST;
      } else if (end) {
        type = CLDB_LOG_RECORD_TYPE_LAST;
      } else {
        type = CLDB_LOG_RECORD_TYPE_MIDDLE;
      }

      status = emit_physical_record(writer, type, ptr, fragment_length);
      ptr += fragment_length;
      left -= fragment_length;
      begin = false;
    }
  } while (cldb_status_is_ok(status) && left > 0);

  return status;
}
