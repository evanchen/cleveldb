#include "cleveldb/comparator.h"

#include <stdint.h>

#include "port/port.h"

/* 对应上游 util/comparator.cc 的 BytewiseComparatorImpl；C 版把四个
 * 方法放进 static const vtable，单例本身就是静态常量，无需析构。 */

static const char kBytewiseName[] = "leveldb.BytewiseComparator";

static int bytewise_compare(const cldb_comparator_t *comparator,
                            cldb_slice_t a, cldb_slice_t b) {
  (void)comparator;
  return cldb_slice_compare(a, b);
}

static const char *bytewise_name(const cldb_comparator_t *comparator) {
  (void)comparator;
  return kBytewiseName;
}

/* 找公共前缀；若首个不同字节还能加一且仍小于 limit 的对应字节，则
 * 加一后截断。必须严格小于，否则结果可能等于或越过 limit。 */
static void bytewise_find_shortest_separator(
    const cldb_comparator_t *comparator, char *start, size_t *start_size,
    cldb_slice_t limit) {
  size_t min_length;
  size_t diff_index = 0;
  uint8_t diff_byte;
  uint8_t limit_byte;

  (void)comparator;
  CLDB_ASSERT(start_size != NULL);
  CLDB_ASSERT(start != NULL || *start_size == 0);
  CLDB_ASSERT(limit.size == 0 || limit.data != NULL);

  min_length = *start_size < limit.size ? *start_size : limit.size;
  while (diff_index < min_length &&
         (uint8_t)start[diff_index] == (uint8_t)limit.data[diff_index]) {
    ++diff_index;
  }
  if (diff_index >= min_length) {
    return; /* 一方是另一方的前缀，不能缩短 */
  }

  diff_byte = (uint8_t)start[diff_index];
  limit_byte = (uint8_t)limit.data[diff_index];
  if (diff_byte < UINT8_MAX && (uint8_t)(diff_byte + 1) < limit_byte) {
    start[diff_index] = (char)(diff_byte + 1);
    *start_size = diff_index + 1;
    CLDB_ASSERT(cldb_slice_compare(cldb_slice_make(start, *start_size),
                                  limit) < 0);
  }
}

/* 找第一个非 0xff 字节，加一后截断；全 0xff 时无法生成更大的短串。 */
static void bytewise_find_short_successor(const cldb_comparator_t *comparator,
                                          char *key, size_t *key_size) {
  size_t i;

  (void)comparator;
  CLDB_ASSERT(key_size != NULL);
  CLDB_ASSERT(key != NULL || *key_size == 0);
  for (i = 0; i < *key_size; ++i) {
    uint8_t byte = (uint8_t)key[i];

    if (byte != UINT8_MAX) {
      key[i] = (char)(byte + 1);
      *key_size = i + 1;
      return;
    }
  }
}

static const cldb_comparator_vtbl_t kBytewiseVtbl = {
    .compare = bytewise_compare,
    .name = bytewise_name,
    .find_shortest_separator = bytewise_find_shortest_separator,
    .find_short_successor = bytewise_find_short_successor,
};

static const cldb_comparator_t kBytewiseComparator = {
    .vtbl = &kBytewiseVtbl,
};

const cldb_comparator_t *cldb_bytewise_comparator(void) {
  return &kBytewiseComparator;
}
