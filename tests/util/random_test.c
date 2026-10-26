#include <stddef.h>
#include <stdint.h>

#include "harness.h"
#include "util/random.h"

/* Park–Miller 最小标准序列，与上游 Random(1) 一致。 */
CLDB_TEST(random_known_sequence) {
  static const uint32_t kExpected[] = {
      16807,      282475249, 1622650073, 984943658, 1144108930,
      470211272,  101027544, 1457850878, 1458777923, 2007237709,
  };
  cldb_random_t rnd;
  size_t i;

  cldb_random_init(&rnd, 1);
  CLDB_ASSERT_EQ_UINT(1, rnd.seed);
  for (i = 0; i < sizeof(kExpected) / sizeof(kExpected[0]); ++i) {
    CLDB_ASSERT_EQ_UINT(kExpected[i], cldb_random_next(&rnd));
    CLDB_ASSERT_EQ_UINT(kExpected[i], rnd.seed);
  }
}

/* 对应上游构造函数对坏种子的修正。 */
CLDB_TEST(random_bad_seeds) {
  static const uint32_t kBadSeeds[] = {
      0, UINT32_C(2147483647), UINT32_C(0x80000000), UINT32_MAX,
      UINT32_C(0x80000001),
  };
  cldb_random_t rnd;
  size_t i;

  for (i = 0; i < sizeof(kBadSeeds) / sizeof(kBadSeeds[0]); ++i) {
    cldb_random_init(&rnd, kBadSeeds[i]);
    CLDB_EXPECT_EQ_UINT(1, rnd.seed);
    CLDB_EXPECT_EQ_UINT(16807, cldb_random_next(&rnd));
  }

  cldb_random_init(&rnd, UINT32_C(0x7ffffffe));
  CLDB_EXPECT_EQ_UINT(UINT32_C(0x7ffffffe), rnd.seed);
}

CLDB_TEST(random_state_matches_next) {
  cldb_random_t rnd;

  cldb_random_init(&rnd, 301);
  CLDB_ASSERT_EQ_UINT(301, rnd.seed);
  CLDB_ASSERT_EQ_UINT(UINT32_C(5058907), cldb_random_next(&rnd));
  CLDB_EXPECT_EQ_UINT(UINT32_C(5058907), rnd.seed);
}

CLDB_TEST(random_uniform_range) {
  static const uint32_t kRanges[] = {1, 2, 3, 7, 10, 100, 1000};
  cldb_random_t rnd;
  size_t i;
  uint32_t draw;

  for (i = 0; i < sizeof(kRanges) / sizeof(kRanges[0]); ++i) {
    uint32_t n = kRanges[i];

    cldb_random_init(&rnd, 301 + n);
    for (draw = 0; draw < 10000; ++draw) {
      uint32_t value = cldb_random_uniform(&rnd, n);

      CLDB_ASSERT_TRUE(value < n);
      if (n == 1) {
        CLDB_ASSERT_EQ_UINT(0, value);
      }
    }
  }

  cldb_random_init(&rnd, 301);
  CLDB_EXPECT_EQ_UINT(301, rnd.seed);
  CLDB_EXPECT_EQ_UINT(0, cldb_random_uniform(&rnd, 0));
  CLDB_EXPECT_EQ_UINT(301, rnd.seed);
  CLDB_EXPECT_EQ_UINT(0, cldb_random_uniform(NULL, 10));
}

CLDB_TEST(random_one_in_frequency) {
  cldb_random_t rnd;
  uint32_t count = 0;
  uint32_t i;

  cldb_random_init(&rnd, 301);
  for (i = 0; i < 100000; ++i) {
    if (cldb_random_one_in(&rnd, 10)) {
      ++count;
    }
  }
  CLDB_EXPECT_TRUE(count > 9000 && count < 11000);

  cldb_random_init(&rnd, 301);
  for (i = 0; i < 1000; ++i) {
    CLDB_ASSERT_TRUE(cldb_random_one_in(&rnd, 1));
  }

  cldb_random_init(&rnd, 301);
  CLDB_EXPECT_EQ_UINT(301, rnd.seed);
  CLDB_EXPECT_FALSE(cldb_random_one_in(&rnd, 0));
  CLDB_EXPECT_EQ_UINT(301, rnd.seed);
  CLDB_EXPECT_FALSE(cldb_random_one_in(NULL, 10));
}

CLDB_TEST(random_skewed_distribution) {
  cldb_random_t rnd;
  uint32_t max_value = 0;
  uint32_t large = 0;
  uint32_t i;

  cldb_random_init(&rnd, 301);
  for (i = 0; i < 1000; ++i) {
    CLDB_ASSERT_EQ_UINT(0, cldb_random_skewed(&rnd, 0));
  }

  cldb_random_init(&rnd, 301);
  for (i = 0; i < 100000; ++i) {
    uint32_t value = cldb_random_skewed(&rnd, 10);

    CLDB_ASSERT_TRUE(value < (UINT32_C(1) << 10));
    if (value >= (UINT32_C(1) << 9)) {
      ++large;
    }
    if (value > max_value) {
      max_value = value;
    }
  }
  CLDB_EXPECT_TRUE(large > 0 && large < 20000);
  CLDB_EXPECT_EQ_UINT(UINT32_C(1023), max_value);

  cldb_random_init(&rnd, 301);
  CLDB_EXPECT_TRUE(cldb_random_skewed(&rnd, 31) < (UINT32_C(1) << 31));
}

CLDB_TEST(random_skewed_repeatable) {
  cldb_random_t a;
  cldb_random_t b;
  cldb_random_t c;
  uint32_t i;

  cldb_random_init(&a, 301);
  cldb_random_init(&b, 301);
  cldb_random_init(&c, 302);
  for (i = 0; i < 1000; ++i) {
    CLDB_ASSERT_EQ_UINT(cldb_random_skewed(&a, 10),
                        cldb_random_skewed(&b, 10));
  }
  CLDB_EXPECT_TRUE(cldb_random_next(&a) != cldb_random_next(&c));
}

/* 有效种子下输出恒在 [1, M-1]；若归约允许 next == M，这里会捕获。 */
CLDB_TEST(random_output_range) {
  static const uint32_t kSeeds[] = {
      1, 2, 301, UINT32_C(0x7ffffffe), UINT32_C(0x12345678),
  };
  static const uint32_t kModulus = UINT32_C(2147483647);
  cldb_random_t rnd;
  size_t i;
  uint32_t draw;

  for (i = 0; i < sizeof(kSeeds) / sizeof(kSeeds[0]); ++i) {
    cldb_random_init(&rnd, kSeeds[i]);
    for (draw = 0; draw < 10000; ++draw) {
      uint32_t value = cldb_random_next(&rnd);

      CLDB_ASSERT_TRUE(value >= 1 && value < kModulus);
    }
  }
}

CLDB_TEST(random_invalid_arguments) {
  cldb_random_t rnd;

  cldb_random_init(NULL, 1);
  CLDB_EXPECT_EQ_UINT(0, cldb_random_next(NULL));
  CLDB_EXPECT_EQ_UINT(0, cldb_random_skewed(NULL, 10));

  cldb_random_init(&rnd, 301);
  CLDB_EXPECT_EQ_UINT(0, cldb_random_skewed(&rnd, 32));
  CLDB_EXPECT_EQ_UINT(301, rnd.seed);
  CLDB_EXPECT_EQ_UINT(0, cldb_random_uniform(&rnd, 0));
  CLDB_EXPECT_EQ_UINT(301, rnd.seed);
  CLDB_EXPECT_FALSE(cldb_random_one_in(&rnd, 0));
  CLDB_EXPECT_EQ_UINT(301, rnd.seed);
}

void cldb_register_random_tests(void) {
  CLDB_RUN_TEST(random_known_sequence);
  CLDB_RUN_TEST(random_bad_seeds);
  CLDB_RUN_TEST(random_state_matches_next);
  CLDB_RUN_TEST(random_uniform_range);
  CLDB_RUN_TEST(random_one_in_frequency);
  CLDB_RUN_TEST(random_skewed_distribution);
  CLDB_RUN_TEST(random_skewed_repeatable);
  CLDB_RUN_TEST(random_output_range);
  CLDB_RUN_TEST(random_invalid_arguments);
}
