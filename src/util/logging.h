#ifndef CLDB_SRC_UTIL_LOGGING_H_
#define CLDB_SRC_UTIL_LOGGING_H_

#include <stdbool.h>
#include <stdint.h>

#include "cleveldb/buf.h"
#include "cleveldb/slice.h"

/* 诊断文本格式化；对应上游 util/logging.h。数字转十进制，非可打印字节
 * 转 "\xNN"，用于日志和调试输出。函数失败只可能是参数错误或分配失败。 */

/* 追加 num 的十进制文本；对应上游 AppendNumberTo。 */
bool cldb_append_number_to(cldb_buf_t *dst, uint64_t num);

/* 追加 value 的转义文本；对应上游 AppendEscapedStringTo。可打印 ASCII
 * (0x20..0x7e) 原样输出，其余字节输出为小写 "\xNN"。失败时 dst 可能已有
 * 部分追加。 */
bool cldb_append_escaped_string_to(cldb_buf_t *dst, cldb_slice_t value);

/* 用 num 的十进制文本替换 dst 内容；对应上游 NumberToString。 */
bool cldb_number_to_string(cldb_buf_t *dst, uint64_t num);

/* 用 value 的转义文本替换 dst 内容；对应上游 EscapeString。 */
bool cldb_escape_string(cldb_buf_t *dst, cldb_slice_t value);

/* 从 in 开头解析十进制数字并前进；对应上游 ConsumeDecimalNumber。
 * 溢出或没有数字时返回 false，in 和 *value 均不变。 */
bool cldb_consume_decimal_number(cldb_slice_t *in, uint64_t *value);

#endif /* CLDB_SRC_UTIL_LOGGING_H_ */
