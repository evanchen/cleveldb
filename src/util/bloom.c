#include "cleveldb/filter_policy.h"

#include <stdint.h>
#include <stdlib.h>

#include "util/hash.h"

/* 内置 Bloom 过滤器；对应上游 util/bloom.cc。过滤器末尾保存探测数 k，
 * 前面是位数组。k 越大误判率越低但查询越慢；bits_per_key 决定空间/误判率
 * 的权衡。格式与名称必须与上游一致，否则无法读取上游生成的 SSTable。 */

static const char kBloomName[] = "leveldb.BuiltinBloomFilter2";
static const uint32_t kBloomHashSeed = UINT32_C(0xbc9f1d34);

typedef struct cldb_bloom_filter_policy {
  cldb_filter_policy_t base; /* 必须为首成员，便于 vtable 回调转回本类型 */
  size_t bits_per_key; /* 每个 key 的目标位数；构造后只读 */
  size_t k; /* 探测次数，构造时 clamp 到 [1,30] */
} cldb_bloom_filter_policy_t;

static const char *bloom_name(const cldb_filter_policy_t *policy) {
  (void)policy;
  return kBloomName;
}

/* 对应上游 BloomFilterPolicy::CreateFilter。先追加 bytes 个零字节，再用
 * 双哈希逐 key 置位，最后追加一字节 k；整个过滤器只调用一次 sink。 */
static cldb_status_t bloom_create_filter(const cldb_filter_policy_t *policy,
                                         const cldb_slice_t *keys, size_t n,
                                         cldb_filter_sink_t *dst) {
  const cldb_bloom_filter_policy_t *bloom =
      (const cldb_bloom_filter_policy_t *)policy;
  size_t bits;
  size_t bytes;
  size_t filter_size;
  size_t i;
  char *array;

  if (keys == NULL && n != 0) {
    return cldb_status_invalid_argument("bloom.create_filter", "keys is NULL");
  }
  if (dst == NULL || dst->append == NULL) {
    return cldb_status_invalid_argument("bloom.create_filter",
                                        "destination is invalid");
  }
  if (bloom->bits_per_key != 0 && n > SIZE_MAX / bloom->bits_per_key) {
    return cldb_status_invalid_argument("bloom.create_filter",
                                        "filter size overflows");
  }

  /* n * bits_per_key 已确认不溢出；最少 64 位，再向上取整到字节。 */
  bits = n * bloom->bits_per_key;
  if (bits < 64) {
    bits = 64;
  }
  if (bits > SIZE_MAX - 7) {
    return cldb_status_invalid_argument("bloom.create_filter",
                                        "filter size overflows");
  }
  bytes = (bits + 7) / 8;
  bits = bytes * 8;
  filter_size = bytes + 1; /* 位数组 + 末尾 k */

  array = dst->append(dst->arg, NULL, filter_size);
  if (array == NULL) {
    return cldb_status_io("bloom.create_filter", "append failed");
  }
  for (i = 0; i < n; ++i) {
    uint32_t h = cldb_hash(keys[i].data, keys[i].size, kBloomHashSeed);
    /* 对应上游：循环右移 17 位，等价 rotl(h, 15)。 */
    uint32_t delta = (h >> 17) | (h << 15);
    size_t j;

    for (j = 0; j < bloom->k; ++j) {
      size_t bitpos = (size_t)h % bits;
      uint8_t mask = (uint8_t)(1u << (unsigned)(bitpos % 8));

      array[bitpos / 8] = (char)((uint8_t)array[bitpos / 8] | mask);
      h += delta;
    }
  }
  array[bytes] = (char)bloom->k;
  return cldb_status_ok();
}

/* 对应上游 BloomFilterPolicy::KeyMayMatch。损坏或未知编码保守返回 true，
 * 避免把实际存在的数据误判为不存在；只有确认某一位未设置才返回 false。 */
static bool bloom_key_may_match(const cldb_filter_policy_t *policy,
                                cldb_slice_t key, cldb_slice_t filter) {
  const char *array = filter.data;
  size_t len = filter.size;
  size_t bits;
  size_t k;
  size_t j;
  uint32_t h;
  uint32_t delta;

  (void)policy;
  if (len < 2) {
    return false;
  }
  /* 防御超长过滤器的长度乘法；保守放行而不是把结果当成不存在。 */
  if (len - 1 > SIZE_MAX / 8) {
    return true;
  }
  bits = (len - 1) * 8;
  k = (size_t)(uint8_t)array[len - 1];
  if (k > 30) {
    /* 上游保留 k>30 给未来编码；当前实现无法判断，按可能匹配处理。 */
    return true;
  }

  h = cldb_hash(key.data, key.size, kBloomHashSeed);
  delta = (h >> 17) | (h << 15);
  for (j = 0; j < k; ++j) {
    size_t bitpos = (size_t)h % bits;
    uint8_t mask = (uint8_t)(1u << (unsigned)(bitpos % 8));

    if (((uint8_t)array[bitpos / 8] & mask) == 0) {
      return false;
    }
    h += delta;
  }
  return true;
}

static void bloom_destroy(cldb_filter_policy_t *policy) {
  free((cldb_bloom_filter_policy_t *)policy);
}

static const cldb_filter_policy_vtbl_t kBloomVtbl = {
    .name = bloom_name,
    .create_filter = bloom_create_filter,
    .key_may_match = bloom_key_may_match,
    .destroy = bloom_destroy,
};

cldb_filter_policy_t *cldb_new_bloom_filter_policy(int bits_per_key) {
  cldb_bloom_filter_policy_t *bloom;
  size_t k;

  if (bits_per_key < 0) {
    return NULL;
  }
  bloom = malloc(sizeof(*bloom));
  if (bloom == NULL) {
    return NULL;
  }
  bloom->base.vtbl = &kBloomVtbl;
  bloom->bits_per_key = (size_t)bits_per_key;

  /* 0.69 约等于 ln(2)，对应最优探测数 k = (m/n)ln(2)。 */
  k = (size_t)((double)bits_per_key * 0.69);
  if (k < 1) {
    k = 1;
  }
  if (k > 30) {
    k = 30;
  }
  bloom->k = k;
  return &bloom->base;
}
