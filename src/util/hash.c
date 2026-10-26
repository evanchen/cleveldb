#include "util/hash.h"

#include "util/coding.h"

uint32_t cldb_hash(const char *data, size_t n, uint32_t seed) {
  static const uint32_t kM = UINT32_C(0xc6a4a793);
  static const uint32_t kR = 24;
  uint32_t h;
  size_t remaining;
  const char *p = data;

  if (n == 0) {
    return seed;
  }
  /* 对应上游 h = seed ^ (n * m)，其中乘积只取低 32 位。 */
  h = seed ^ ((uint32_t)n * kM);
  remaining = n;
  while (remaining >= 4) {
    h += cldb_decode_fixed32(p);
    p += 4;
    remaining -= 4;
    h *= kM;
    h ^= h >> 16;
  }
  /* 尾部 0..3 字节按小端拼入，等价于上游 switch 的 fallthrough 顺序。 */
  if (remaining >= 3) {
    h += (uint32_t)(uint8_t)p[2] << 16;
  }
  if (remaining >= 2) {
    h += (uint32_t)(uint8_t)p[1] << 8;
  }
  if (remaining >= 1) {
    h += (uint32_t)(uint8_t)p[0];
    h *= kM;
    h ^= h >> kR;
  }
  return h;
}
