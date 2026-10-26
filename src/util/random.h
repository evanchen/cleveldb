#ifndef CLDB_SRC_UTIL_RANDOM_H_
#define CLDB_SRC_UTIL_RANDOM_H_

#include <stdbool.h>
#include <stdint.h>

/* Park–Miller 伪随机数状态；对应上游 util/random.h 的 Random。
 * 不用于加密，只用于测试/基准中的可重复随机序列。 */
typedef struct cldb_random {
  uint32_t seed; /* 当前状态；对应上游 seed_，范围 [1, 2^31-2]；勿改写 */
} cldb_random_t;

/* 初始化；对应上游构造函数，先取低 31 位，再把 0 和 2^31-1 修正为 1，
 * 否则后续序列会退化为常数；因此这些种子与 1 产生相同序列。
 * rnd 为 NULL 时不做任何事。 */
void cldb_random_init(cldb_random_t *rnd, uint32_t seed);

/* 生成下一个值；对应上游 Next()，返回 [1, 2^31-1)。
 * rnd 为 NULL 时返回 0。 */
uint32_t cldb_random_next(cldb_random_t *rnd);

/* 返回 [0, n-1]；对应上游 Uniform(n)，与上游一样用取模，
 * n 不整除 2^31-1 时存在轻微模偏置。n 必须 > 0；n == 0 或 rnd 为 NULL
 * 时返回 0，且不消耗状态。 */
uint32_t cldb_random_uniform(cldb_random_t *rnd, uint32_t n);

/* 约 1/n 概率返回 true；对应上游 OneIn(n)。n 必须 > 0；
 * n == 0 或 rnd 为 NULL 时返回 false，且不消耗状态。 */
bool cldb_random_one_in(cldb_random_t *rnd, uint32_t n);

/* 指数偏置到较小值：base 取自 [0, max_log]，再取 [0, 2^base)；
 * 对应上游 Skewed(max_log)，返回随机数值（常作长度），不是布尔。
 * max_log 必须 <= 31，越界返回 0 且不消耗状态。 */
uint32_t cldb_random_skewed(cldb_random_t *rnd, uint32_t max_log);

#endif /* CLDB_SRC_UTIL_RANDOM_H_ */
