#ifndef CLDB_SRC_UTIL_CODING_H_
#define CLDB_SRC_UTIL_CODING_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cleveldb/slice.h"
#include "util/buf.h"

/* 落盘数字编码；对应上游 util/coding.h：定长整数小端，varint 每字节
 * 低 7 位为数据、最高位为续位，字符串带 varint32 长度前缀。
 * 编码结果不依赖主机端序，可与上游逐字节互读。 */

/* 写 4 字节小端；对应上游 EncodeFixed32。dst 需有 4 字节可写空间。 */
static inline void cldb_encode_fixed32(char *dst, uint32_t value) {
  uint8_t *buffer = (uint8_t *)dst;

  buffer[0] = (uint8_t)value;
  buffer[1] = (uint8_t)(value >> 8);
  buffer[2] = (uint8_t)(value >> 16);
  buffer[3] = (uint8_t)(value >> 24);
}

/* 写 8 字节小端；对应上游 EncodeFixed64。dst 需有 8 字节可写空间。 */
static inline void cldb_encode_fixed64(char *dst, uint64_t value) {
  uint8_t *buffer = (uint8_t *)dst;

  buffer[0] = (uint8_t)value;
  buffer[1] = (uint8_t)(value >> 8);
  buffer[2] = (uint8_t)(value >> 16);
  buffer[3] = (uint8_t)(value >> 24);
  buffer[4] = (uint8_t)(value >> 32);
  buffer[5] = (uint8_t)(value >> 40);
  buffer[6] = (uint8_t)(value >> 48);
  buffer[7] = (uint8_t)(value >> 56);
}

/* 读 4 字节小端；对应上游 DecodeFixed32，ptr 需有 4 字节可读空间。 */
static inline uint32_t cldb_decode_fixed32(const char *ptr) {
  const uint8_t *buffer = (const uint8_t *)ptr;

  return (uint32_t)buffer[0] | ((uint32_t)buffer[1] << 8) |
         ((uint32_t)buffer[2] << 16) | ((uint32_t)buffer[3] << 24);
}

/* 读 8 字节小端；对应上游 DecodeFixed64，ptr 需有 8 字节可读空间。 */
static inline uint64_t cldb_decode_fixed64(const char *ptr) {
  const uint8_t *buffer = (const uint8_t *)ptr;

  return (uint64_t)buffer[0] | ((uint64_t)buffer[1] << 8) |
         ((uint64_t)buffer[2] << 16) | ((uint64_t)buffer[3] << 24) |
         ((uint64_t)buffer[4] << 32) | ((uint64_t)buffer[5] << 40) |
         ((uint64_t)buffer[6] << 48) | ((uint64_t)buffer[7] << 56);
}

/* 追加 fixed32/fixed64；对应上游 PutFixed32/PutFixed64。
 * 返回 false 表示缓冲追加失败，内容未改变。 */
bool cldb_put_fixed32(cldb_buf_t *dst, uint32_t value);
bool cldb_put_fixed64(cldb_buf_t *dst, uint64_t value);

/* 直接写 varint 并返回末尾指针；对应上游 EncodeVarint32/64。
 * varint32 最多 5 字节，varint64 最多 10 字节；dst 空间不足会越界。 */
char *cldb_encode_varint32(char *dst, uint32_t value);
char *cldb_encode_varint64(char *dst, uint64_t value);

/* 追加 varint；对应上游 PutVarint32/64。返回 false 表示追加失败。 */
bool cldb_put_varint32(cldb_buf_t *dst, uint32_t value);
bool cldb_put_varint64(cldb_buf_t *dst, uint64_t value);

/* 解析 varint 并返回末尾指针，失败返回 NULL；对应上游
 * GetVarint32Ptr/GetVarint64Ptr。只读 [p, limit)，失败时 *value 不变。
 * 与上游一致，接受非最短编码；仅截断或超过最大字节数才失败。 */
const char *cldb_get_varint32_ptr_fallback(const char *p, const char *limit,
                                           uint32_t *value);
const char *cldb_get_varint64_ptr(const char *p, const char *limit,
                                  uint64_t *value);

/* varint32 快速路径；对应上游 inline GetVarint32Ptr。首字节无续位时
 * 直接返回，否则交给 fallback 逐字节解析；失败返回 NULL。 */
static inline const char *cldb_get_varint32_ptr(const char *p,
                                                const char *limit,
                                                uint32_t *value) {
  if (p != NULL && limit != NULL && value != NULL && p < limit) {
    uint32_t result = (uint32_t)(uint8_t)p[0];

    if ((result & 128) == 0) {
      *value = result;
      return p + 1;
    }
  }
  return cldb_get_varint32_ptr_fallback(p, limit, value);
}

/* 从 Slice 头部解析 varint 并前进；对应上游 GetVarint32/64。
 * 失败返回 false，input 和 *value 均不变。 */
bool cldb_get_varint32(cldb_slice_t *input, uint32_t *value);
bool cldb_get_varint64(cldb_slice_t *input, uint64_t *value);

/* 追加 varint32 长度前缀和内容；对应上游 PutLengthPrefixedSlice。
 * size 超过 UINT32_MAX 或分配失败返回 false，内容不变；
 * value 不得指向 dst 自身的存储。 */
bool cldb_put_length_prefixed_slice(cldb_buf_t *dst, cldb_slice_t value);

/* 从 Slice 头部解析长度前缀内容并前进；对应上游
 * GetLengthPrefixedSlice。result 借用 input；内容不足返回 false，
 * 此时 input 已越过长度前缀，调用者应丢弃或自行回滚。 */
bool cldb_get_length_prefixed_slice(cldb_slice_t *input,
                                    cldb_slice_t *result);

/* varint 编码字节数（1..10）；对应上游 VarintLength。 */
size_t cldb_varint_length(uint64_t value);

#endif /* CLDB_SRC_UTIL_CODING_H_ */
