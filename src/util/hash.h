#ifndef CLDB_SRC_UTIL_HASH_H_
#define CLDB_SRC_UTIL_HASH_H_

#include <stddef.h>
#include <stdint.h>

/* 内部非加密哈希；对应上游 util/hash.h 的 Hash，供 Bloom、Cache 等使用。
 * 相同输入与 seed 的结果稳定，但不承诺抗碰撞；data 只读。
 * n > 0 时 data 必须非 NULL；n == 0 时可为 NULL，直接返回 seed。 */
uint32_t cldb_hash(const char *data, size_t n, uint32_t seed);

#endif /* CLDB_SRC_UTIL_HASH_H_ */
