#include "port/port_compression.h"

#include "port/port.h"

/* 默认构建不链接压缩库；各钩子返回 false，调用方据此报 Corruption。
 * 启用 Snappy/Zstd 时替换本文件的实现即可，接口保持不变。 */

bool cldb_port_snappy_compress(const char *input, size_t length,
                               cldb_buf_t *output) {
  CLDB_UNUSED(input);
  CLDB_UNUSED(length);
  CLDB_UNUSED(output);
  return false;
}

bool cldb_port_snappy_get_uncompressed_length(const char *input, size_t length,
                                               size_t *result) {
  CLDB_UNUSED(input);
  CLDB_UNUSED(length);
  CLDB_UNUSED(result);
  return false;
}

bool cldb_port_snappy_uncompress(const char *input, size_t length, char *output,
                                 size_t output_length) {
  CLDB_UNUSED(input);
  CLDB_UNUSED(length);
  CLDB_UNUSED(output);
  CLDB_UNUSED(output_length);
  return false;
}

bool cldb_port_zstd_compress(int level, const char *input, size_t length,
                             cldb_buf_t *output) {
  CLDB_UNUSED(level);
  CLDB_UNUSED(input);
  CLDB_UNUSED(length);
  CLDB_UNUSED(output);
  return false;
}

bool cldb_port_zstd_get_uncompressed_length(const char *input, size_t length,
                                             size_t *result) {
  CLDB_UNUSED(input);
  CLDB_UNUSED(length);
  CLDB_UNUSED(result);
  return false;
}

bool cldb_port_zstd_uncompress(const char *input, size_t length, char *output,
                               size_t output_length) {
  CLDB_UNUSED(input);
  CLDB_UNUSED(length);
  CLDB_UNUSED(output);
  CLDB_UNUSED(output_length);
  return false;
}
