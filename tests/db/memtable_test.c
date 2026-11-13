#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "db/dbformat.h"
#include "db/memtable.h"
#include "db/write_batch_internal.h"
#include "harness.h"
#include "status_test_util.h"

/* MemTable 行为测试；对应上游 db/memtable.cc 的语义。上游基线没有
 * memtable_test.cc，WriteBatch 集成部分按其 db/write_batch_test.cc 的
 * Multiple 用例构造。 */

typedef struct {
  const char *user_key;
  cldb_sequence_number_t sequence;
  cldb_value_type_t type;
  const char *value;
} expected_entry_t;

static const expected_entry_t kIteratorEntries[] = {
    {"a", UINT64_C(4), CLDB_VALUE_TYPE_VALUE, "a4"},
    {"a", UINT64_C(1), CLDB_VALUE_TYPE_VALUE, "a1"},
    {"b", UINT64_C(3), CLDB_VALUE_TYPE_VALUE, "b3"},
    {"b", UINT64_C(2), CLDB_VALUE_TYPE_VALUE, "b2"},
    {"c", UINT64_C(5), CLDB_VALUE_TYPE_DELETION, ""},
};

enum { kIteratorEntryCount =
           (int)(sizeof(kIteratorEntries) / sizeof(kIteratorEntries[0])) };

static const expected_entry_t kBatchEntries[] = {
    {"baz", UINT64_C(102), CLDB_VALUE_TYPE_VALUE, "boo"},
    {"box", UINT64_C(101), CLDB_VALUE_TYPE_DELETION, ""},
    {"foo", UINT64_C(100), CLDB_VALUE_TYPE_VALUE, "bar"},
};

enum { kBatchEntryCount =
           (int)(sizeof(kBatchEntries) / sizeof(kBatchEntries[0])) };

static cldb_slice_t literal(const char *text) {
  return cldb_slice_make(text, strlen(text));
}

static cldb_memtable_t *create_memtable(
    cldb_internal_key_comparator_t *comparator) {
  cldb_internal_key_comparator_init(comparator,
                                    cldb_bytewise_comparator());
  return cldb_memtable_create(comparator);
}

static cldb_status_t add_value(cldb_memtable_t *memtable,
                               cldb_sequence_number_t sequence,
                               const char *key, const char *value) {
  return cldb_memtable_add(memtable, sequence, CLDB_VALUE_TYPE_VALUE,
                           literal(key), literal(value));
}

static cldb_status_t add_deletion(cldb_memtable_t *memtable,
                                  cldb_sequence_number_t sequence,
                                  const char *key) {
  return cldb_memtable_add(memtable, sequence, CLDB_VALUE_TYPE_DELETION,
                           literal(key), cldb_slice_empty());
}

/* 按快照查找并核对值；LookupKey 始终在同一测试内释放。 */
static void expect_value(cldb_memtable_t *memtable, const char *user_key,
                         cldb_sequence_number_t snapshot,
                         const char *expected) {
  cldb_lookup_key_t lookup;
  cldb_slice_t value = cldb_slice_empty();
  cldb_status_t status = cldb_status_ok();
  bool found;

  if (!cldb_lookup_key_init(&lookup, literal(user_key), snapshot)) {
    CLDB_EXPECT_TRUE(false);
    return;
  }
  found = cldb_memtable_get(memtable, &lookup, &value, &status);
  CLDB_EXPECT_TRUE(found);
  CLDB_EXPECT_OK(status);
  CLDB_EXPECT_EQ_UINT(strlen(expected), value.size);
  CLDB_EXPECT_TRUE(cldb_slice_equal(value, literal(expected)));
  cldb_lookup_key_destroy(&lookup);
}

static void expect_deleted(cldb_memtable_t *memtable, const char *user_key,
                           cldb_sequence_number_t snapshot) {
  cldb_lookup_key_t lookup;
  cldb_slice_t value = cldb_slice_empty();
  cldb_status_t status = cldb_status_ok();
  bool found;

  if (!cldb_lookup_key_init(&lookup, literal(user_key), snapshot)) {
    CLDB_EXPECT_TRUE(false);
    return;
  }
  found = cldb_memtable_get(memtable, &lookup, &value, &status);
  CLDB_EXPECT_TRUE(found);
  CLDB_EXPECT_STATUS(CLDB_STATUS_NOT_FOUND, status);
  CLDB_EXPECT_EQ_UINT(0, value.size);
  cldb_lookup_key_destroy(&lookup);
}

static void expect_missing(cldb_memtable_t *memtable, const char *user_key,
                           cldb_sequence_number_t snapshot) {
  cldb_lookup_key_t lookup;
  cldb_slice_t value = cldb_slice_empty();
  cldb_status_t status = cldb_status_ok();
  bool found;

  if (!cldb_lookup_key_init(&lookup, literal(user_key), snapshot)) {
    CLDB_EXPECT_TRUE(false);
    return;
  }
  found = cldb_memtable_get(memtable, &lookup, &value, &status);
  CLDB_EXPECT_FALSE(found);
  CLDB_EXPECT_OK(status);
  CLDB_EXPECT_EQ_UINT(0, value.size);
  cldb_lookup_key_destroy(&lookup);
}

/* 核对迭代器当前位置的内部键与 value；条目损坏时返回 false。 */
static bool check_iterator_entry(const cldb_iterator_t *iterator,
                                 const expected_entry_t *expected) {
  cldb_slice_t key;
  cldb_slice_t value;
  cldb_parsed_internal_key_t parsed;

  if (!cldb_iter_valid(iterator)) {
    CLDB_EXPECT_TRUE(false);
    return false;
  }
  key = cldb_iter_key(iterator);
  value = cldb_iter_value(iterator);
  if (!cldb_parse_internal_key(key, &parsed)) {
    CLDB_EXPECT_TRUE(false);
    return false;
  }
  CLDB_EXPECT_TRUE(cldb_slice_equal(parsed.user_key,
                                    literal(expected->user_key)));
  CLDB_EXPECT_EQ_UINT(expected->sequence, parsed.sequence);
  CLDB_EXPECT_EQ_INT((int)expected->type, (int)parsed.type);
  CLDB_EXPECT_EQ_UINT(strlen(expected->user_key) +
                          CLDB_INTERNAL_KEY_TAG_SIZE,
                      key.size);
  CLDB_EXPECT_TRUE(cldb_slice_equal(value, literal(expected->value)));
  CLDB_EXPECT_OK(cldb_iter_status(iterator));
  return true;
}

static void add_iterator_entries(cldb_memtable_t *memtable) {
  CLDB_ASSERT_OK(add_value(memtable, UINT64_C(2), "b", "b2"));
  CLDB_ASSERT_OK(add_value(memtable, UINT64_C(1), "a", "a1"));
  CLDB_ASSERT_OK(add_value(memtable, UINT64_C(3), "b", "b3"));
  CLDB_ASSERT_OK(add_value(memtable, UINT64_C(4), "a", "a4"));
  CLDB_ASSERT_OK(add_deletion(memtable, UINT64_C(5), "c"));
}

static void seek_internal(cldb_iterator_t *iterator, cldb_buf_t *target,
                          const char *user_key, cldb_sequence_number_t seq,
                          cldb_value_type_t type) {
  cldb_buf_clear(target);
  CLDB_ASSERT_TRUE(
      cldb_append_internal_key(target, literal(user_key), seq, type));
  cldb_iter_seek(iterator, cldb_buf_slice(target));
}

CLDB_TEST(memtable_add_get_versions) {
  cldb_internal_key_comparator_t comparator;
  cldb_memtable_t *memtable = create_memtable(&comparator);

  CLDB_ASSERT_TRUE(memtable != NULL);
  CLDB_ASSERT_OK(add_value(memtable, UINT64_C(10), "foo", "v1"));
  CLDB_ASSERT_OK(add_value(memtable, UINT64_C(20), "foo", "v2"));
  CLDB_ASSERT_OK(add_deletion(memtable, UINT64_C(30), "foo"));

  expect_missing(memtable, "foo", UINT64_C(9));
  expect_value(memtable, "foo", UINT64_C(10), "v1");
  expect_value(memtable, "foo", UINT64_C(19), "v1");
  expect_value(memtable, "foo", UINT64_C(20), "v2");
  expect_value(memtable, "foo", UINT64_C(29), "v2");
  expect_deleted(memtable, "foo", UINT64_C(30));
  expect_missing(memtable, "bar", UINT64_C(30));

  cldb_memtable_unref(memtable);
}

CLDB_TEST(memtable_get_user_key_boundaries) {
  cldb_internal_key_comparator_t comparator;
  cldb_memtable_t *memtable = create_memtable(&comparator);

  CLDB_ASSERT_TRUE(memtable != NULL);
  CLDB_ASSERT_OK(add_value(memtable, UINT64_C(1), "", "empty"));
  CLDB_ASSERT_OK(add_value(memtable, UINT64_C(1), "fo", "v1"));
  CLDB_ASSERT_OK(add_value(memtable, UINT64_C(2), "foo", "v2"));

  expect_value(memtable, "", UINT64_C(1), "empty");
  expect_value(memtable, "fo", UINT64_C(2), "v1");
  expect_value(memtable, "foo", UINT64_C(2), "v2");
  expect_missing(memtable, "foo", UINT64_C(1));
  /* Seek 落在更大的 user key 上，不能误当作命中。 */
  expect_missing(memtable, "f", UINT64_C(2));
  expect_missing(memtable, "foa", UINT64_C(2));
  expect_missing(memtable, "fp", UINT64_C(2));

  cldb_memtable_unref(memtable);
}

CLDB_TEST(memtable_add_invalid_arguments) {
  cldb_internal_key_comparator_t comparator;
  cldb_internal_key_comparator_t bad_comparator;
  cldb_memtable_t *memtable;
  cldb_lookup_key_t lookup;
  cldb_slice_t value = cldb_slice_empty();
  cldb_status_t status = cldb_status_ok();
  cldb_slice_t bad_key = {NULL, 1};
  cldb_slice_t bad_value = {NULL, 1};

  memset(&bad_comparator, 0, sizeof(bad_comparator));
  CLDB_EXPECT_EQ_PTR(NULL, cldb_memtable_create(NULL));
  CLDB_EXPECT_EQ_PTR(NULL, cldb_memtable_create(&bad_comparator));

  memtable = create_memtable(&comparator);
  CLDB_ASSERT_TRUE(memtable != NULL);
  CLDB_EXPECT_EQ_UINT(0, cldb_memtable_approximate_memory_usage(NULL));
  CLDB_EXPECT_EQ_PTR(NULL, cldb_memtable_new_iterator(NULL));
  cldb_memtable_ref(NULL);
  cldb_memtable_unref(NULL);

  CLDB_EXPECT_STATUS(
      CLDB_STATUS_INVALID_ARGUMENT,
      cldb_memtable_add(NULL, UINT64_C(1), CLDB_VALUE_TYPE_VALUE,
                        literal("k"), literal("v")));
  CLDB_EXPECT_STATUS(
      CLDB_STATUS_INVALID_ARGUMENT,
      cldb_memtable_add(memtable, CLDB_MAX_SEQUENCE_NUMBER + UINT64_C(1),
                        CLDB_VALUE_TYPE_VALUE, literal("k"),
                        literal("v")));
  CLDB_EXPECT_STATUS(
      CLDB_STATUS_INVALID_ARGUMENT,
      cldb_memtable_add(memtable, UINT64_C(1),
                        (cldb_value_type_t)7, literal("k"),
                        literal("v")));
  CLDB_EXPECT_STATUS(
      CLDB_STATUS_INVALID_ARGUMENT,
      cldb_memtable_add(memtable, UINT64_C(1), CLDB_VALUE_TYPE_VALUE,
                        bad_key, literal("v")));
  CLDB_EXPECT_STATUS(
      CLDB_STATUS_INVALID_ARGUMENT,
      cldb_memtable_add(memtable, UINT64_C(1), CLDB_VALUE_TYPE_VALUE,
                        literal("k"), bad_value));

  CLDB_EXPECT_OK(cldb_memtable_add(memtable, UINT64_C(1),
                                   CLDB_VALUE_TYPE_VALUE, cldb_slice_empty(),
                                   cldb_slice_empty()));
  expect_value(memtable, "", UINT64_C(1), "");

  CLDB_ASSERT_OK(add_value(memtable, UINT64_C(5), "dup", "v"));
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     add_value(memtable, UINT64_C(5), "dup", "v"));
  CLDB_EXPECT_OK(add_deletion(memtable, UINT64_C(5), "dup"));

  CLDB_ASSERT_TRUE(cldb_lookup_key_init(&lookup, literal("dup"),
                                        UINT64_C(5)));
  CLDB_EXPECT_FALSE(cldb_memtable_get(NULL, &lookup, &value, &status));
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT, status);
  status = cldb_status_ok();
  CLDB_EXPECT_FALSE(cldb_memtable_get(memtable, NULL, &value, &status));
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT, status);
  status = cldb_status_ok();
  CLDB_EXPECT_FALSE(
      cldb_memtable_get(memtable, &lookup, NULL, &status));
  CLDB_EXPECT_FALSE(
      cldb_memtable_get(memtable, &lookup, &value, NULL));

  cldb_lookup_key_destroy(&lookup);
  cldb_memtable_unref(memtable);
}

CLDB_TEST(memtable_ref_unref_keeps_object_alive) {
  cldb_internal_key_comparator_t comparator;
  cldb_memtable_t *memtable = create_memtable(&comparator);

  CLDB_ASSERT_TRUE(memtable != NULL);
  cldb_memtable_ref(memtable);
  cldb_memtable_unref(memtable); /* 归还新增引用，创建者引用仍有效 */
  CLDB_ASSERT_OK(add_value(memtable, UINT64_C(1), "alive", "yes"));
  expect_value(memtable, "alive", UINT64_C(1), "yes");
  cldb_memtable_unref(memtable);
}

CLDB_TEST(memtable_approximate_memory_usage_grows) {
  cldb_internal_key_comparator_t comparator;
  cldb_memtable_t *memtable = create_memtable(&comparator);
  char key[32];
  char value[32];
  size_t previous;
  size_t initial;
  int i;

  CLDB_ASSERT_TRUE(memtable != NULL);
  initial = cldb_memtable_approximate_memory_usage(memtable);
  CLDB_EXPECT_TRUE(initial > 0);
  previous = initial;
  for (i = 0; i < 500; ++i) {
    size_t current;

    (void)snprintf(key, sizeof(key), "key-%03d", i);
    (void)snprintf(value, sizeof(value), "value-%03d", i);
    CLDB_ASSERT_OK(add_value(memtable, (cldb_sequence_number_t)i, key,
                             value));
    current = cldb_memtable_approximate_memory_usage(memtable);
    CLDB_EXPECT_TRUE(current >= previous);
    previous = current;
  }
  CLDB_EXPECT_TRUE(previous > initial);
  cldb_memtable_unref(memtable);
}

CLDB_TEST(memtable_iterator_order_and_seek) {
  cldb_internal_key_comparator_t comparator;
  cldb_memtable_t *memtable = create_memtable(&comparator);
  cldb_iterator_t *iterator;
  cldb_buf_t target;
  int i;

  CLDB_ASSERT_TRUE(memtable != NULL);
  add_iterator_entries(memtable);
  iterator = cldb_memtable_new_iterator(memtable);
  CLDB_ASSERT_TRUE(iterator != NULL);
  cldb_buf_init(&target);

  cldb_iter_seek_to_first(iterator);
  for (i = 0; i < kIteratorEntryCount; ++i) {
    CLDB_ASSERT_TRUE(check_iterator_entry(iterator,
                                          &kIteratorEntries[i]));
    cldb_iter_next(iterator);
  }
  CLDB_EXPECT_FALSE(cldb_iter_valid(iterator));
  CLDB_EXPECT_OK(cldb_iter_status(iterator));

  cldb_iter_seek_to_last(iterator);
  for (i = kIteratorEntryCount; i > 0; --i) {
    CLDB_ASSERT_TRUE(check_iterator_entry(iterator,
                                          &kIteratorEntries[i - 1]));
    if (i > 1) {
      cldb_iter_prev(iterator);
    }
  }
  cldb_iter_prev(iterator);
  CLDB_EXPECT_FALSE(cldb_iter_valid(iterator));
  CLDB_EXPECT_OK(cldb_iter_status(iterator));

  /* 同 user key 的 tag 降序决定定位结果。 */
  seek_internal(iterator, &target, "b", UINT64_C(4),
                CLDB_VALUE_TYPE_VALUE);
  CLDB_ASSERT_TRUE(check_iterator_entry(iterator,
                                        &kIteratorEntries[2]));
  seek_internal(iterator, &target, "c", UINT64_C(5),
                CLDB_VALUE_TYPE_DELETION);
  CLDB_ASSERT_TRUE(check_iterator_entry(iterator,
                                        &kIteratorEntries[4]));
  seek_internal(iterator, &target, "c", UINT64_C(4),
                CLDB_VALUE_TYPE_VALUE);
  CLDB_EXPECT_FALSE(cldb_iter_valid(iterator));
  CLDB_EXPECT_OK(cldb_iter_status(iterator));

  cldb_iter_seek(iterator, cldb_slice_make("x", 1));
  CLDB_EXPECT_FALSE(cldb_iter_valid(iterator));
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_iter_status(iterator));
  cldb_iter_seek_to_first(iterator);
  CLDB_EXPECT_OK(cldb_iter_status(iterator));
  CLDB_ASSERT_TRUE(check_iterator_entry(iterator,
                                        &kIteratorEntries[0]));

  cldb_buf_free(&target);
  cldb_iter_destroy(iterator);
  cldb_memtable_unref(memtable);
}

CLDB_TEST(memtable_iterator_empty) {
  cldb_internal_key_comparator_t comparator;
  cldb_memtable_t *memtable = create_memtable(&comparator);
  cldb_iterator_t *iterator;
  cldb_buf_t target;

  CLDB_ASSERT_TRUE(memtable != NULL);
  iterator = cldb_memtable_new_iterator(memtable);
  CLDB_ASSERT_TRUE(iterator != NULL);
  CLDB_EXPECT_FALSE(cldb_iter_valid(iterator));
  CLDB_EXPECT_OK(cldb_iter_status(iterator));

  cldb_iter_seek_to_first(iterator);
  CLDB_EXPECT_FALSE(cldb_iter_valid(iterator));
  CLDB_EXPECT_OK(cldb_iter_status(iterator));
  cldb_iter_seek_to_last(iterator);
  CLDB_EXPECT_FALSE(cldb_iter_valid(iterator));
  CLDB_EXPECT_OK(cldb_iter_status(iterator));

  cldb_buf_init(&target);
  seek_internal(iterator, &target, "a", UINT64_C(1),
                CLDB_VALUE_TYPE_VALUE);
  CLDB_EXPECT_FALSE(cldb_iter_valid(iterator));
  CLDB_EXPECT_OK(cldb_iter_status(iterator));
  cldb_buf_free(&target);

  cldb_iter_destroy(iterator);
  cldb_memtable_unref(memtable);
}

CLDB_TEST(memtable_write_batch_insert_into) {
  cldb_internal_key_comparator_t comparator;
  cldb_memtable_t *memtable = create_memtable(&comparator);
  cldb_write_batch_t *batch;
  cldb_write_batch_t *duplicate_batch;
  cldb_iterator_t *iterator;
  cldb_status_t status;
  int i;

  CLDB_ASSERT_TRUE(memtable != NULL);
  batch = cldb_write_batch_create();
  CLDB_ASSERT_TRUE(batch != NULL);
  CLDB_ASSERT_TRUE(cldb_write_batch_set_sequence(batch, UINT64_C(100)));
  CLDB_ASSERT_TRUE(
      cldb_write_batch_put(batch, literal("foo"), literal("bar")));
  CLDB_ASSERT_TRUE(cldb_write_batch_delete(batch, literal("box")));
  CLDB_ASSERT_TRUE(
      cldb_write_batch_put(batch, literal("baz"), literal("boo")));
  CLDB_EXPECT_EQ_UINT(3, cldb_write_batch_count(batch));

  CLDB_ASSERT_OK(cldb_write_batch_insert_into(batch, memtable));
  CLDB_EXPECT_EQ_UINT(100, cldb_write_batch_sequence(batch));
  CLDB_EXPECT_EQ_UINT(3, cldb_write_batch_count(batch));

  iterator = cldb_memtable_new_iterator(memtable);
  CLDB_ASSERT_TRUE(iterator != NULL);
  cldb_iter_seek_to_first(iterator);
  for (i = 0; i < kBatchEntryCount; ++i) {
    CLDB_ASSERT_TRUE(check_iterator_entry(iterator, &kBatchEntries[i]));
    cldb_iter_next(iterator);
  }
  CLDB_EXPECT_FALSE(cldb_iter_valid(iterator));
  cldb_iter_destroy(iterator);

  duplicate_batch = cldb_write_batch_create();
  CLDB_ASSERT_TRUE(duplicate_batch != NULL);
  CLDB_ASSERT_TRUE(
      cldb_write_batch_set_sequence(duplicate_batch, UINT64_C(100)));
  CLDB_ASSERT_TRUE(cldb_write_batch_put(duplicate_batch, literal("foo"),
                                        literal("bar")));
  status = cldb_write_batch_insert_into(duplicate_batch, memtable);
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT, status);
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_write_batch_insert_into(batch, NULL));
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_write_batch_insert_into(NULL, memtable));

  cldb_write_batch_destroy(duplicate_batch);
  cldb_write_batch_destroy(batch);
  cldb_memtable_unref(memtable);
}

void cldb_register_memtable_tests(void) {
  CLDB_RUN_TEST(memtable_add_get_versions);
  CLDB_RUN_TEST(memtable_get_user_key_boundaries);
  CLDB_RUN_TEST(memtable_add_invalid_arguments);
  CLDB_RUN_TEST(memtable_ref_unref_keeps_object_alive);
  CLDB_RUN_TEST(memtable_approximate_memory_usage_grows);
  CLDB_RUN_TEST(memtable_iterator_order_and_seek);
  CLDB_RUN_TEST(memtable_iterator_empty);
  CLDB_RUN_TEST(memtable_write_batch_insert_into);
}
