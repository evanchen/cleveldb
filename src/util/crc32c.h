#ifndef CLDB_SRC_UTIL_CRC32C_H_
#define CLDB_SRC_UTIL_CRC32C_H_

#include <stddef.h>
#include <stdint.h>

/* 移植自上游 util/crc32c.h/.cc；原版权：
 * Copyright (c) 2011 The LevelDB Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file. See the AUTHORS file for names of contributors. */

/* CRC32C（Castagnoli，反射多项式 0x82f63b78）用于 WAL/Table 块校验。
 * 与普通 CRC32 相比有硬件指令支持，且检错特性适合存储场景。
 * 输入 data 借用；n > 0 时必须非 NULL。 */

/* 计算把 data[0,n) 接在 init_crc 之后的结果；对应上游 crc32c::Extend。
 * init_crc 是此前数据的 CRC32C；n == 0 时原样返回 init_crc。 */
uint32_t cldb_crc32c_extend(uint32_t init_crc, const char *data, size_t n);

/* 一次性计算 data[0,n) 的 CRC32C；对应上游 crc32c::Value。 */
static inline uint32_t cldb_crc32c_value(const char *data, size_t n) {
  return cldb_crc32c_extend(0, data, n);
}

/* 掩码常量；对应上游 kMaskDelta。 */
#define CLDB_CRC32C_MASK_DELTA UINT32_C(0xa282ead8)

/* 存储前掩码；对应上游 crc32c::Mask。上游动机：对含有内嵌 CRC 的
 * 数据再算 CRC 时，内嵌值会被当作普通数据，因此落盘值先做掩码。 */
static inline uint32_t cldb_crc32c_mask(uint32_t crc) {
  return ((crc >> 15) | (crc << 17)) + CLDB_CRC32C_MASK_DELTA;
}

/* 还原掩码；对应上游 crc32c::Unmask，满足 unmask(mask(x)) == x。 */
static inline uint32_t cldb_crc32c_unmask(uint32_t masked_crc) {
  uint32_t rot = masked_crc - CLDB_CRC32C_MASK_DELTA;

  return (rot >> 17) | (rot << 15);
}

#endif /* CLDB_SRC_UTIL_CRC32C_H_ */
