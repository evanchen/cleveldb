#include "db/dbformat.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "util/coding.h"

/* 对应上游 db/dbformat.cc；编码原语和比较器接口复用 util 模块。 */

enum { kLookupKeyOverhead = 13 };

/* ValueType 只有两个合法值；对应上游 PackSequenceAndType 中的断言。 */
static bool value_type_is_valid(cldb_value_type_t type) {
  return type == CLDB_VALUE_TYPE_DELETION ||
         type == CLDB_VALUE_TYPE_VALUE;
}

/* 打包 tag；对应上游 PackSequenceAndType。调用前必须校验范围。 */
static uint64_t pack_sequence_and_type(cldb_sequence_number_t sequence,
                                       cldb_value_type_t type) {
  CLDB_ASSERT(sequence <= CLDB_MAX_SEQUENCE_NUMBER);
  CLDB_ASSERT(value_type_is_valid(type));
  return (sequence << 8) | (uint64_t)(unsigned int)type;
}

bool cldb_append_internal_key(cldb_buf_t *result, cldb_slice_t user_key,
                              cldb_sequence_number_t sequence,
                              cldb_value_type_t type) {
  size_t old_size;
  size_t total;
  size_t new_size;
  size_t offset = 0;
  bool source_internal = false;
  const char *source = user_key.data;

  if (result == NULL || sequence > CLDB_MAX_SEQUENCE_NUMBER ||
      !value_type_is_valid(type) ||
      (user_key.data == NULL && user_key.size > 0)) {
    return false;
  }
  if (user_key.size > SIZE_MAX - CLDB_INTERNAL_KEY_TAG_SIZE) {
    return false;
  }
  total = user_key.size + CLDB_INTERNAL_KEY_TAG_SIZE;
  if (result->size > SIZE_MAX - total) {
    return false;
  }
  new_size = result->size + total;
  if (new_size == SIZE_MAX) {
    return false; /* 还需一个 NUL 字节 */
  }

  /* 记录自引用偏移；扩容可能搬移 result->data。 */
  if (result->data != NULL && source != NULL) {
    uintptr_t base = (uintptr_t)result->data;
    uintptr_t from = (uintptr_t)source;

    if (from >= base && from - base < (uintptr_t)result->capacity) {
      offset = (size_t)(from - base);
      if (user_key.size > result->capacity - offset) {
        return false;
      }
      source_internal = true;
    }
  }

  old_size = result->size;
  if (!cldb_buf_resize(result, new_size)) {
    return false;
  }
  if (source_internal) {
    source = result->data + offset;
  }
  if (user_key.size > 0) {
    memmove(result->data + old_size, source, user_key.size);
  }
  cldb_encode_fixed64(result->data + old_size + user_key.size,
                      pack_sequence_and_type(sequence, type));
  return true;
}

bool cldb_parse_internal_key(cldb_slice_t internal_key,
                             cldb_parsed_internal_key_t *result) {
  cldb_parsed_internal_key_t parsed;
  uint64_t tag;
  unsigned int type;

  if (result == NULL ||
      internal_key.size < CLDB_INTERNAL_KEY_TAG_SIZE ||
      (internal_key.data == NULL && internal_key.size > 0)) {
    return false;
  }
  tag = cldb_decode_fixed64(
      internal_key.data + internal_key.size - CLDB_INTERNAL_KEY_TAG_SIZE);
  type = (unsigned int)(tag & UINT64_C(0xff));
  if (type > (unsigned int)CLDB_VALUE_TYPE_VALUE) {
    return false;
  }

  parsed.user_key = cldb_slice_make(
      internal_key.data,
      internal_key.size - CLDB_INTERNAL_KEY_TAG_SIZE);
  parsed.sequence = tag >> 8;
  parsed.type = (cldb_value_type_t)type;
  *result = parsed;
  return true;
}

/* 内部键比较器回调；对应上游 InternalKeyComparator::Compare。 */
static int internal_key_compare(const cldb_comparator_t *comparator,
                                cldb_slice_t a, cldb_slice_t b) {
  const cldb_internal_key_comparator_t *self =
      (const cldb_internal_key_comparator_t *)comparator;
  int result;

  CLDB_ASSERT(self->user_comparator != NULL);
  if (a.size < CLDB_INTERNAL_KEY_TAG_SIZE ||
      b.size < CLDB_INTERNAL_KEY_TAG_SIZE) {
    CLDB_ASSERT(false);
    return cldb_slice_compare(a, b);
  }

  result = cldb_comparator_compare(self->user_comparator,
                                   cldb_extract_user_key(a),
                                   cldb_extract_user_key(b));
  if (result == 0) {
    uint64_t atag = cldb_decode_fixed64(
        a.data + a.size - CLDB_INTERNAL_KEY_TAG_SIZE);
    uint64_t btag = cldb_decode_fixed64(
        b.data + b.size - CLDB_INTERNAL_KEY_TAG_SIZE);

    /* tag 含 sequence 和 type，数值大的排在前面。 */
    if (atag > btag) {
      result = -1;
    } else if (atag < btag) {
      result = 1;
    }
  }
  return result;
}

static const char *internal_key_name(const cldb_comparator_t *comparator) {
  static const char kName[] = "leveldb.InternalKeyComparator";

  (void)comparator;
  return kName;
}

/* 用户键缩短后追加最大 tag；对应上游 FindShortestSeparator。 */
static void internal_key_find_shortest_separator(
    const cldb_comparator_t *comparator, char *start, size_t *start_size,
    cldb_slice_t limit) {
  const cldb_internal_key_comparator_t *self =
      (const cldb_internal_key_comparator_t *)comparator;
  cldb_buf_t tmp;
  cldb_slice_t user_start;
  cldb_slice_t user_limit;
  size_t user_size;

  CLDB_ASSERT(self->user_comparator != NULL);
  if (start == NULL || start_size == NULL ||
      *start_size < CLDB_INTERNAL_KEY_TAG_SIZE ||
      limit.size < CLDB_INTERNAL_KEY_TAG_SIZE ||
      (limit.data == NULL && limit.size > 0)) {
    CLDB_ASSERT(false);
    return;
  }

  user_size = *start_size - CLDB_INTERNAL_KEY_TAG_SIZE;
  user_start = cldb_slice_make(start, user_size);
  user_limit = cldb_slice_make(
      limit.data, limit.size - CLDB_INTERNAL_KEY_TAG_SIZE);

  cldb_buf_init(&tmp);
  if (!cldb_buf_resize(&tmp, user_size)) {
    return; /* 缩短只是优化，分配失败时保持原键 */
  }
  if (user_size > 0) {
    memcpy(tmp.data, start, user_size);
  }
  cldb_comparator_find_shortest_separator(self->user_comparator, tmp.data,
                                          &tmp.size, user_limit);
  if (tmp.size < user_size &&
      cldb_comparator_compare(self->user_comparator, user_start,
                              cldb_buf_slice(&tmp)) < 0) {
    size_t new_user_size = tmp.size;

    if (new_user_size + CLDB_INTERNAL_KEY_TAG_SIZE <= *start_size) {
      if (new_user_size > 0) {
        memcpy(start, tmp.data, new_user_size);
      }
      cldb_encode_fixed64(
          start + new_user_size,
          pack_sequence_and_type(CLDB_MAX_SEQUENCE_NUMBER,
                                 CLDB_VALUE_TYPE_FOR_SEEK));
      *start_size = new_user_size + CLDB_INTERNAL_KEY_TAG_SIZE;
    }
  }
  cldb_buf_free(&tmp);
}

/* 用户键变大且变短后追加最大 tag；对应上游 FindShortSuccessor。 */
static void internal_key_find_short_successor(
    const cldb_comparator_t *comparator, char *key, size_t *key_size) {
  const cldb_internal_key_comparator_t *self =
      (const cldb_internal_key_comparator_t *)comparator;
  cldb_buf_t tmp;
  cldb_slice_t user_key;
  size_t user_size;

  CLDB_ASSERT(self->user_comparator != NULL);
  if (key == NULL || key_size == NULL ||
      *key_size < CLDB_INTERNAL_KEY_TAG_SIZE) {
    CLDB_ASSERT(false);
    return;
  }

  user_size = *key_size - CLDB_INTERNAL_KEY_TAG_SIZE;
  user_key = cldb_slice_make(key, user_size);
  cldb_buf_init(&tmp);
  if (!cldb_buf_resize(&tmp, user_size)) {
    return;
  }
  if (user_size > 0) {
    memcpy(tmp.data, key, user_size);
  }
  cldb_comparator_find_short_successor(self->user_comparator, tmp.data,
                                       &tmp.size);
  if (tmp.size < user_size &&
      cldb_comparator_compare(self->user_comparator, user_key,
                              cldb_buf_slice(&tmp)) < 0) {
    size_t new_user_size = tmp.size;

    if (new_user_size + CLDB_INTERNAL_KEY_TAG_SIZE <= *key_size) {
      if (new_user_size > 0) {
        memcpy(key, tmp.data, new_user_size);
      }
      cldb_encode_fixed64(
          key + new_user_size,
          pack_sequence_and_type(CLDB_MAX_SEQUENCE_NUMBER,
                                 CLDB_VALUE_TYPE_FOR_SEEK));
      *key_size = new_user_size + CLDB_INTERNAL_KEY_TAG_SIZE;
    }
  }
  cldb_buf_free(&tmp);
}

static const cldb_comparator_vtbl_t kInternalKeyComparatorVtbl = {
    .compare = internal_key_compare,
    .name = internal_key_name,
    .find_shortest_separator = internal_key_find_shortest_separator,
    .find_short_successor = internal_key_find_short_successor,
};

void cldb_internal_key_comparator_init(
    cldb_internal_key_comparator_t *comparator,
    const cldb_comparator_t *user_comparator) {
  CLDB_ASSERT(comparator != NULL);
  CLDB_ASSERT(user_comparator != NULL);
  if (comparator == NULL) {
    return;
  }
  comparator->base.vtbl = &kInternalKeyComparatorVtbl;
  comparator->user_comparator = user_comparator;
}

/* 过滤器策略回调；对应上游 InternalFilterPolicy。 */
static const char *internal_filter_name(const cldb_filter_policy_t *policy) {
  const cldb_internal_filter_policy_t *self =
      (const cldb_internal_filter_policy_t *)policy;

  CLDB_ASSERT(self->user_policy != NULL);
  return cldb_filter_policy_name(self->user_policy);
}

static cldb_status_t internal_filter_create(
    const cldb_filter_policy_t *policy, const cldb_slice_t *keys, size_t n,
    cldb_filter_sink_t *dst) {
  const cldb_internal_filter_policy_t *self =
      (const cldb_internal_filter_policy_t *)policy;
  cldb_status_t status;
  cldb_slice_t *user_keys = NULL;
  size_t i;

  CLDB_ASSERT(self->user_policy != NULL);
  if (self->user_policy == NULL) {
    return cldb_status_invalid_argument("InternalFilterPolicy::CreateFilter",
                                        "user policy is NULL");
  }
  if ((keys == NULL && n > 0) || dst == NULL || dst->append == NULL) {
    return cldb_status_invalid_argument("InternalFilterPolicy::CreateFilter",
                                        "invalid argument");
  }
  if (n == 0) {
    return cldb_filter_policy_create_filter(self->user_policy, keys, 0, dst);
  }
  if (n > SIZE_MAX / sizeof(cldb_slice_t)) {
    return cldb_status_invalid_argument("InternalFilterPolicy::CreateFilter",
                                        "key count overflows");
  }

  user_keys = malloc(n * sizeof(cldb_slice_t));
  if (user_keys == NULL) {
    return cldb_status_io("InternalFilterPolicy::CreateFilter",
                          "out of memory");
  }
  for (i = 0; i < n; ++i) {
    if (keys[i].size < CLDB_INTERNAL_KEY_TAG_SIZE ||
        (keys[i].data == NULL && keys[i].size > 0)) {
      status = cldb_status_invalid_argument(
          "InternalFilterPolicy::CreateFilter", "invalid internal key");
      goto cleanup;
    }
    user_keys[i] = cldb_extract_user_key(keys[i]);
  }
  status = cldb_filter_policy_create_filter(self->user_policy, user_keys, n,
                                            dst);

cleanup:
  free(user_keys);
  return status;
}

static bool internal_filter_key_may_match(
    const cldb_filter_policy_t *policy, cldb_slice_t key,
    cldb_slice_t filter) {
  const cldb_internal_filter_policy_t *self =
      (const cldb_internal_filter_policy_t *)policy;

  CLDB_ASSERT(self->user_policy != NULL);
  if (self->user_policy == NULL ||
      key.size < CLDB_INTERNAL_KEY_TAG_SIZE ||
      (key.data == NULL && key.size > 0)) {
    return false;
  }
  return cldb_filter_policy_key_may_match(self->user_policy,
                                          cldb_extract_user_key(key), filter);
}

/* 包装器不拥有 user_policy，destroy 只满足接口约定，不做释放。 */
static void internal_filter_destroy(cldb_filter_policy_t *policy) {
  (void)policy;
}

static const cldb_filter_policy_vtbl_t kInternalFilterPolicyVtbl = {
    .name = internal_filter_name,
    .create_filter = internal_filter_create,
    .key_may_match = internal_filter_key_may_match,
    .destroy = internal_filter_destroy,
};

void cldb_internal_filter_policy_init(
    cldb_internal_filter_policy_t *policy,
    const cldb_filter_policy_t *user_policy) {
  CLDB_ASSERT(policy != NULL);
  CLDB_ASSERT(user_policy != NULL);
  if (policy == NULL) {
    return;
  }
  policy->base.vtbl = &kInternalFilterPolicyVtbl;
  policy->user_policy = user_policy;
}

bool cldb_lookup_key_init(cldb_lookup_key_t *key, cldb_slice_t user_key,
                          cldb_sequence_number_t sequence) {
  size_t needed;
  uint32_t internal_size;
  char *write;

  if (key == NULL) {
    return false;
  }
  key->start = NULL;
  key->kstart = NULL;
  key->end = NULL;
  key->heap_allocated = false;

  if (sequence > CLDB_MAX_SEQUENCE_NUMBER ||
      (user_key.data == NULL && user_key.size > 0) ||
      user_key.size > SIZE_MAX - kLookupKeyOverhead ||
      user_key.size > (size_t)UINT32_MAX - CLDB_INTERNAL_KEY_TAG_SIZE) {
    return false;
  }

  needed = user_key.size + kLookupKeyOverhead;
  if (needed <= sizeof(key->space)) {
    key->start = key->space;
  } else {
    key->start = malloc(needed);
    if (key->start == NULL) {
      return false;
    }
    key->heap_allocated = true;
  }

  internal_size = (uint32_t)(user_key.size + CLDB_INTERNAL_KEY_TAG_SIZE);
  write = cldb_encode_varint32(key->start, internal_size);
  key->kstart = write;
  if (user_key.size > 0) {
    memcpy(write, user_key.data, user_key.size);
  }
  write += user_key.size;
  cldb_encode_fixed64(
      write, pack_sequence_and_type(sequence, CLDB_VALUE_TYPE_FOR_SEEK));
  key->end = write + CLDB_INTERNAL_KEY_TAG_SIZE;
  return true;
}

void cldb_lookup_key_destroy(cldb_lookup_key_t *key) {
  if (key == NULL) {
    return;
  }
  if (key->heap_allocated) {
    free(key->start);
  }
  key->start = NULL;
  key->kstart = NULL;
  key->end = NULL;
  key->heap_allocated = false;
}

cldb_slice_t cldb_lookup_key_memtable_key(const cldb_lookup_key_t *key) {
  if (key == NULL || key->start == NULL || key->end == NULL ||
      key->end < key->start) {
    return cldb_slice_empty();
  }
  return cldb_slice_make(key->start, (size_t)(key->end - key->start));
}

cldb_slice_t cldb_lookup_key_internal_key(const cldb_lookup_key_t *key) {
  if (key == NULL || key->kstart == NULL || key->end == NULL ||
      key->end <= key->kstart) {
    return cldb_slice_empty();
  }
  return cldb_slice_make(key->kstart, (size_t)(key->end - key->kstart));
}

cldb_slice_t cldb_lookup_key_user_key(const cldb_lookup_key_t *key) {
  cldb_slice_t internal;
  size_t user_size;

  internal = cldb_lookup_key_internal_key(key);
  if (internal.size < CLDB_INTERNAL_KEY_TAG_SIZE) {
    return cldb_slice_empty();
  }
  user_size = internal.size - CLDB_INTERNAL_KEY_TAG_SIZE;
  return cldb_slice_make(internal.data, user_size);
}
