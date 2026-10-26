#include "util/random.h"

#include <stddef.h>

/* Park–Miller 最小标准生成器：M = 2^31-1，A = 16807。 */
enum { CLDB_RANDOM_M = 2147483647 };

void cldb_random_init(cldb_random_t *rnd, uint32_t seed) {
  if (rnd == NULL) {
    return;
  }
  rnd->seed = seed & UINT32_C(0x7fffffff);
  if (rnd->seed == 0 || rnd->seed == (uint32_t)CLDB_RANDOM_M) {
    rnd->seed = 1;
  }
}

uint32_t cldb_random_next(cldb_random_t *rnd) {
  static const uint64_t kA = 16807;
  uint64_t product;
  uint32_t next;

  if (rnd == NULL) {
    return 0;
  }
  product = (uint64_t)rnd->seed * kA;
  /* 2^31 ≡ 1 (mod M)，故 product 与 (product >> 31) + (product & M)
   * 同余；一次归约最多多出 A-1，有效种子下结果不可能等于 M，故用 >。 */
  next = (uint32_t)((product >> 31) + (product & (uint64_t)CLDB_RANDOM_M));
  if (next > (uint32_t)CLDB_RANDOM_M) {
    next -= (uint32_t)CLDB_RANDOM_M;
  }
  rnd->seed = next;
  return next;
}

uint32_t cldb_random_uniform(cldb_random_t *rnd, uint32_t n) {
  if (rnd == NULL || n == 0) {
    return 0;
  }
  return cldb_random_next(rnd) % n;
}

bool cldb_random_one_in(cldb_random_t *rnd, uint32_t n) {
  if (rnd == NULL || n == 0) {
    return false;
  }
  return (cldb_random_next(rnd) % n) == 0;
}

uint32_t cldb_random_skewed(cldb_random_t *rnd, uint32_t max_log) {
  uint32_t base;

  if (rnd == NULL || max_log > 31) {
    return 0;
  }
  base = cldb_random_uniform(rnd, max_log + 1);
  return cldb_random_uniform(rnd, UINT32_C(1) << base);
}
