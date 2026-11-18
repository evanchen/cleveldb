#include "table/format.h"

#include <stdint.h>
#include <stdlib.h>

#include "port/port.h"
#include "port/port_compression.h"
#include "util/coding.h"
#include "util/crc32c.h"
#include "util/env_internal.h"

typedef bool (*get_uncompressed_length_fn)(const char *input, size_t length,
                                           size_t *result);
typedef bool (*uncompress_fn)(const char *input, size_t length, char *output,
                              size_t output_length);

/* 顺序写 offset、size；返回末尾指针。最多推进 20 字节。 */
static char *encode_handle(char *dst, const cldb_block_handle_t *handle) {
  dst = cldb_encode_varint64(dst, handle->offset);
  return cldb_encode_varint64(dst, handle->size);
}

/* 原子解析两个 varint；失败时 handle 和 cursor 均不变。 */
static bool decode_handle(cldb_slice_t *cursor,
                          cldb_block_handle_t *handle) {
  cldb_slice_t rest = *cursor;
  uint64_t offset;
  uint64_t size;

  if (!cldb_get_varint64(&rest, &offset) ||
      !cldb_get_varint64(&rest, &size)) {
    return false;
  }
  handle->offset = offset;
  handle->size = size;
  *cursor = rest;
  return true;
}

bool cldb_block_handle_encode(const cldb_block_handle_t *handle,
                              cldb_buf_t *dst) {
  char encoded[CLDB_BLOCK_HANDLE_MAX_ENCODED_LENGTH];
  char *end;

  if (handle == NULL || dst == NULL) {
    return false;
  }
  end = encode_handle(encoded, handle);
  return cldb_buf_append(dst, encoded, (size_t)(end - encoded));
}

cldb_status_t cldb_block_handle_decode(cldb_slice_t *input,
                                      cldb_block_handle_t *handle) {
  cldb_slice_t cursor;
  cldb_block_handle_t decoded;

  if (input == NULL || handle == NULL) {
    return cldb_status_invalid_argument("BlockHandle::DecodeFrom",
                                        "input or handle is NULL");
  }
  cursor = *input;
  if (!cldb_slice_is_valid(cursor) || !decode_handle(&cursor, &decoded)) {
    return cldb_status_corruption(NULL, "bad block handle");
  }
  *input = cursor;
  *handle = decoded;
  return cldb_status_ok();
}

bool cldb_footer_encode(const cldb_footer_t *footer, cldb_buf_t *dst) {
  enum { kHandleAreaSize = CLDB_FOOTER_ENCODED_LENGTH - CLDB_TABLE_MAGIC_SIZE };
  char encoded[CLDB_FOOTER_ENCODED_LENGTH] = {0};
  char *end;

  if (footer == NULL || dst == NULL) {
    return false;
  }
  end = encode_handle(encoded, &footer->metaindex_handle);
  end = encode_handle(end, &footer->index_handle);
  /* 两个 handle 之后到 magic 之前保持零填充。 */
  CLDB_ASSERT(end <= encoded + kHandleAreaSize);
  cldb_encode_fixed64(encoded + kHandleAreaSize, CLDB_TABLE_MAGIC_NUMBER);
  return cldb_buf_append(dst, encoded, sizeof(encoded));
}

cldb_status_t cldb_footer_decode(cldb_slice_t *input, cldb_footer_t *footer) {
  const char *magic_ptr;
  cldb_slice_t cursor;
  cldb_block_handle_t metaindex;
  cldb_block_handle_t index;

  if (input == NULL || footer == NULL) {
    return cldb_status_invalid_argument("Footer::DecodeFrom",
                                        "input or footer is NULL");
  }
  if (!cldb_slice_is_valid(*input) ||
      input->size < (size_t)CLDB_FOOTER_ENCODED_LENGTH) {
    return cldb_status_corruption(NULL,
                                  "not an sstable (footer too short)");
  }

  magic_ptr = input->data + CLDB_FOOTER_ENCODED_LENGTH - CLDB_TABLE_MAGIC_SIZE;
  if (cldb_decode_fixed64(magic_ptr) != CLDB_TABLE_MAGIC_NUMBER) {
    return cldb_status_corruption(NULL,
                                  "not an sstable (bad magic number)");
  }

  /* handle 只能占 magic 前的 40 字节，避免把填充或 magic 当 handle 解析。 */
  cursor = cldb_slice_make(
      input->data, CLDB_FOOTER_ENCODED_LENGTH - CLDB_TABLE_MAGIC_SIZE);
  if (!decode_handle(&cursor, &metaindex) ||
      !decode_handle(&cursor, &index)) {
    return cldb_status_corruption(NULL, "bad block handle");
  }

  footer->metaindex_handle = metaindex;
  footer->index_handle = index;
  *input = cldb_slice_make(input->data + CLDB_FOOTER_ENCODED_LENGTH,
                           input->size - CLDB_FOOTER_ENCODED_LENGTH);
  return cldb_status_ok();
}

/* 解压并接管 scratch；失败时释放 scratch 和临时输出。 */
static cldb_status_t read_compressed_block(
    const char *data, size_t n, char *scratch,
    get_uncompressed_length_fn get_length, uncompress_fn uncompress,
    const char *length_error, const char *contents_error,
    cldb_block_contents_t *result) {
  size_t ulength;
  size_t alloc_size;
  char *ubuf;

  if (!get_length(data, n, &ulength)) {
    free(scratch);
    return cldb_status_corruption(NULL, length_error);
  }
  /* malloc(0) 行为因实现而异；空内容也分配 1 字节以保持所有权明确。 */
  alloc_size = ulength > 0 ? ulength : 1;
  ubuf = malloc(alloc_size);
  if (ubuf == NULL) {
    free(scratch);
    return cldb_env_out_of_memory("ReadBlock");
  }
  if (!uncompress(data, n, ubuf, ulength)) {
    free(ubuf);
    free(scratch);
    return cldb_status_corruption(NULL, contents_error);
  }

  free(scratch);
  result->data = cldb_slice_make(ubuf, ulength);
  result->heap_allocated = true;
  result->cachable = true;
  return cldb_status_ok();
}

cldb_status_t cldb_read_block(cldb_random_access_file_t *file,
                              const cldb_read_options_t *options,
                              cldb_block_handle_t handle,
                              cldb_block_contents_t *result) {
  cldb_status_t status;
  cldb_slice_t contents;
  char *scratch;
  const char *data;
  size_t n;
  size_t requested;
  uint8_t type;

  if (result == NULL) {
    return cldb_status_invalid_argument("ReadBlock", "result is NULL");
  }
  result->data = cldb_slice_empty();
  result->cachable = false;
  result->heap_allocated = false;
  if (file == NULL || options == NULL) {
    return cldb_status_invalid_argument("ReadBlock",
                                        "file or options is NULL");
  }
  if (handle.size > SIZE_MAX - (size_t)CLDB_BLOCK_TRAILER_SIZE) {
    return cldb_status_corruption(NULL, "bad block handle");
  }

  n = (size_t)handle.size;
  requested = n + (size_t)CLDB_BLOCK_TRAILER_SIZE;
  scratch = malloc(requested);
  if (scratch == NULL) {
    return cldb_env_out_of_memory("ReadBlock");
  }
  status = cldb_random_access_file_read(file, handle.offset, requested,
                                        &contents, scratch);
  if (!cldb_status_is_ok(status)) {
    free(scratch);
    return status;
  }
  if (contents.size != requested || contents.data == NULL) {
    free(scratch);
    return cldb_status_corruption(NULL, "truncated block read");
  }
  data = contents.data;

  if (options->verify_checksums) {
    uint32_t expected = cldb_crc32c_unmask(cldb_decode_fixed32(
        data + n + (size_t)CLDB_BLOCK_TRAILER_CRC_OFFSET));
    uint32_t actual =
        cldb_crc32c_value(data, n + (size_t)CLDB_BLOCK_TRAILER_TYPE_SIZE);

    if (actual != expected) {
      free(scratch);
      return cldb_status_corruption(NULL, "block checksum mismatch");
    }
  }

  type = (uint8_t)data[n + (size_t)CLDB_BLOCK_TRAILER_TYPE_OFFSET];
  switch (type) {
    case CLDB_COMPRESSION_NONE:
      if (data != scratch) {
        /* 文件实现经 mmap 或自有缓冲直接返回：数据随文件存活，不重复缓存。 */
        free(scratch);
        result->data = cldb_slice_make(data, n);
        result->heap_allocated = false;
        result->cachable = false;
      } else {
        result->data = cldb_slice_make(scratch, n);
        result->heap_allocated = true;
        result->cachable = true;
      }
      return cldb_status_ok();

    case CLDB_COMPRESSION_SNAPPY:
      return read_compressed_block(
          data, n, scratch, cldb_port_snappy_get_uncompressed_length,
          cldb_port_snappy_uncompress,
          "corrupted snappy compressed block length",
          "corrupted snappy compressed block contents", result);

    case CLDB_COMPRESSION_ZSTD:
      return read_compressed_block(
          data, n, scratch, cldb_port_zstd_get_uncompressed_length,
          cldb_port_zstd_uncompress,
          "corrupted zstd compressed block length",
          "corrupted zstd compressed block contents", result);

    default:
      free(scratch);
      return cldb_status_corruption(NULL, "bad block type");
  }
}
