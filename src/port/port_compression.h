#ifndef CLDB_SRC_PORT_PORT_COMPRESSION_H_
#define CLDB_SRC_PORT_PORT_COMPRESSION_H_

#include <stdbool.h>
#include <stddef.h>

#include "cleveldb/buf.h"

/* 压缩后端钩子；对应上游 port/port_stdcxx.h 的 Snappy/Zstd 函数。
 * 未启用第三方库时实现返回 false，读取压缩块会报 Corruption；启用后由
 * 构建配置替换实现。*_uncompress 的 output_length 是 output 可写字节数，
 * 必须等于块头声明的解压长度。compress 结果追加到 output，失败不改变
 * output。 */

bool cldb_port_snappy_compress(const char *input, size_t length,
                               cldb_buf_t *output);
bool cldb_port_snappy_get_uncompressed_length(const char *input, size_t length,
                                               size_t *result);
bool cldb_port_snappy_uncompress(const char *input, size_t length, char *output,
                                 size_t output_length);

bool cldb_port_zstd_compress(int level, const char *input, size_t length,
                             cldb_buf_t *output);
bool cldb_port_zstd_get_uncompressed_length(const char *input, size_t length,
                                             size_t *result);
bool cldb_port_zstd_uncompress(const char *input, size_t length, char *output,
                               size_t output_length);

#endif /* CLDB_SRC_PORT_PORT_COMPRESSION_H_ */
