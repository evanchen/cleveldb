#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "db/dbformat.h"
#include "db/memtable.h"
#include "db/snapshot.h"
#include "db/write_batch_internal.h"
#include "harness.h"
#include "port/port_atomic.h"
#include "port/port_thread.h"
#include "util/random.h"

/* 内存引擎集成测试：随机操作与有序参考模型逐项对照；WriteBatch 的
 * sequence 边界；单写多读下固定快照读取保持稳定。 */

static const char *const kMemoryEngineKeys[] = {
    "alpha", "beta", "gamma", "delta", "a", "aa", "b", "z"};

enum {
  kMemoryEngineKeyCount =
      (int)(sizeof(kMemoryEngineKeys) / sizeof(kMemoryEngineKeys[0])),
  kModelCapacity = 700,
  kModelValueCapacity = 16,
  kModelValueMaxLength = 8,
  kModelSnapshotCapacity = 8,
  kRandomModelSteps = 600,
  kRandomModelRuns = 4,
  kConcurrentReaderCount = 3,
  kConcurrentWriterWrites = 500,
  kConcurrentBatchSize = 4
};

static cldb_slice_t memory_engine_key(size_t index) {
  CLDB_ASSERT(index < (size_t)kMemoryEngineKeyCount);
  return cldb_slice_make(kMemoryEngineKeys[index],
                         strlen(kMemoryEngineKeys[index]));
}

static cldb_slice_t literal(const char *text) {
  return cldb_slice_make(text, strlen(text));
}

/* 参考模型按 InternalKeyComparator 的顺序保存全部版本：user key 升序，
 * 同 key 的 sequence/type tag 降序。key 仅引用调用者的静态字符串。 */
typedef struct model_entry {
  cldb_slice_t user_key;          /* 仅引用；必须比模型活得久 */
  cldb_sequence_number_t sequence; /* 该版本写入时的 sequence */
  cldb_value_type_t type;         /* Value 或 Deletion */
  char value[kModelValueCapacity]; /* 值副本；Deletion 忽略 */
  size_t value_size;              /* 值长度；Deletion 为 0 */
} model_entry_t;

typedef struct model {
  model_entry_t entries[kModelCapacity]; /* 按内部键顺序排列 */
  size_t count;                          /* 已保存版本数 */
} model_t;

typedef struct expected_read {
  bool found;        /* 是否存在 <= 快照的版本 */
  bool deleted;      /* 最新可见版本是否为 Deletion */
  cldb_slice_t value; /* 最新可见 Value；Deletion/未命中为空 */
} expected_read_t;

static int model_entry_compare(const model_entry_t *a,
                               const model_entry_t *b) {
  int key_order = cldb_comparator_compare(cldb_bytewise_comparator(),
                                          a->user_key, b->user_key);
  uint64_t a_tag;
  uint64_t b_tag;

  if (key_order != 0) {
    return key_order;
  }
  a_tag = (a->sequence << 8) | (uint64_t)a->type;
  b_tag = (b->sequence << 8) | (uint64_t)b->type;
  if (a_tag > b_tag) {
    return -1;
  }
  if (a_tag < b_tag) {
    return 1;
  }
  return 0;
}

/* 复制并保持有序；重复内部键或容量不足返回 false。 */
static bool model_add_record(model_t *model, cldb_slice_t key,
                             cldb_sequence_number_t sequence,
                             cldb_value_type_t type, cldb_slice_t value) {
  model_entry_t entry;
  size_t position = 0;
  size_t count;

  if (model == NULL || !cldb_slice_is_valid(key) ||
      !cldb_slice_is_valid(value) ||
      (type != CLDB_VALUE_TYPE_VALUE &&
       type != CLDB_VALUE_TYPE_DELETION) ||
      value.size > (size_t)kModelValueCapacity ||
      sequence > CLDB_MAX_SEQUENCE_NUMBER ||
      model->count >= (size_t)kModelCapacity) {
    return false;
  }
  memset(&entry, 0, sizeof(entry));
  entry.user_key = key;
  entry.sequence = sequence;
  entry.type = type;
  entry.value_size =
      type == CLDB_VALUE_TYPE_VALUE ? value.size : 0;
  if (entry.value_size > 0) {
    memcpy(entry.value, value.data, entry.value_size);
  }

  count = model->count;
  while (position < count &&
         model_entry_compare(&model->entries[position], &entry) < 0) {
    ++position;
  }
  if (position < count &&
      model_entry_compare(&model->entries[position], &entry) == 0) {
    return false;
  }
  if (position < count) {
    memmove(&model->entries[position + 1U], &model->entries[position],
            (count - position) * sizeof(model_entry_t));
  }
  model->entries[position] = entry;
  model->count = count + 1U;
  return true;
}

/* 按快照求期望：同 key 中第一个 sequence <= snapshot 的条目即最新可见版本。 */
static expected_read_t model_read(const model_t *model, cldb_slice_t key,
                                  cldb_sequence_number_t snapshot) {
  expected_read_t expected;
  size_t i;

  expected.found = false;
  expected.deleted = false;
  expected.value = cldb_slice_empty();
  for (i = 0; i < model->count; ++i) {
    const model_entry_t *entry = &model->entries[i];

    if (entry->sequence <= snapshot &&
        cldb_comparator_compare(cldb_bytewise_comparator(), entry->user_key,
                                key) == 0) {
      expected.found = true;
      expected.deleted = entry->type == CLDB_VALUE_TYPE_DELETION;
      if (!expected.deleted) {
        expected.value =
            cldb_slice_make(entry->value, entry->value_size);
      }
      return expected;
    }
  }
  return expected;
}

/* 用 LookupKey 查询并核对模型给出的结果。 */
static bool memtable_get_matches(const cldb_memtable_t *memtable,
                                 const model_t *model, cldb_slice_t key,
                                 cldb_sequence_number_t snapshot) {
  cldb_lookup_key_t lookup;
  cldb_slice_t value = cldb_slice_empty();
  cldb_status_t status = cldb_status_ok();
  expected_read_t expected = model_read(model, key, snapshot);
  bool found;
  bool matches;

  if (!cldb_lookup_key_init(&lookup, key, snapshot)) {
    return false;
  }
  found = cldb_memtable_get(memtable, &lookup, &value, &status);
  matches = found == expected.found;
  if (matches && expected.found) {
    if (expected.deleted) {
      matches = cldb_status_is_not_found(status) && value.size == 0;
    } else {
      matches = cldb_status_is_ok(status) &&
                cldb_slice_equal(value, expected.value);
    }
  } else if (matches) {
    matches = cldb_status_is_ok(status) && value.size == 0;
  }
  cldb_lookup_key_destroy(&lookup);
  return matches;
}

static bool iterator_entry_matches(const cldb_iterator_t *iterator,
                                   const model_entry_t *expected) {
  cldb_slice_t key = cldb_iter_key(iterator);
  cldb_slice_t value = cldb_iter_value(iterator);
  cldb_parsed_internal_key_t parsed;
  cldb_slice_t expected_value;

  if (!cldb_parse_internal_key(key, &parsed) ||
      key.size != expected->user_key.size + CLDB_INTERNAL_KEY_TAG_SIZE ||
      !cldb_slice_equal(parsed.user_key, expected->user_key) ||
      parsed.sequence != expected->sequence ||
      parsed.type != expected->type) {
    return false;
  }
  expected_value =
      cldb_slice_make(expected->value, expected->value_size);
  return cldb_slice_equal(value, expected_value);
}

/* 正反向遍历全部条目，逐项核对模型顺序和值。 */
static bool model_iteration_matches(const cldb_memtable_t *memtable,
                                    const model_t *model) {
  cldb_iterator_t *iterator = cldb_memtable_new_iterator(memtable);
  bool ok = true;
  size_t i = 0;

  if (iterator == NULL) {
    return false;
  }
  cldb_iter_seek_to_first(iterator);
  while (cldb_iter_valid(iterator)) {
    if (i >= model->count ||
        !iterator_entry_matches(iterator, &model->entries[i])) {
      ok = false;
      break;
    }
    ++i;
    cldb_iter_next(iterator);
  }
  if (i != model->count ||
      !cldb_status_is_ok(cldb_iter_status(iterator))) {
    ok = false;
  }

  if (ok) {
    cldb_iter_seek_to_last(iterator);
    i = model->count;
    while (cldb_iter_valid(iterator)) {
      if (i == 0) {
        ok = false;
        break;
      }
      --i;
      if (!iterator_entry_matches(iterator, &model->entries[i])) {
        ok = false;
        break;
      }
      cldb_iter_prev(iterator);
    }
    if (i != 0 || !cldb_status_is_ok(cldb_iter_status(iterator))) {
      ok = false;
    }
  }
  cldb_iter_destroy(iterator);
  return ok;
}

static void fill_random_value(cldb_random_t *rnd,
                              cldb_sequence_number_t sequence, char *value,
                              size_t *value_size) {
  uint32_t length =
      cldb_random_uniform(rnd, (uint32_t)kModelValueMaxLength);
  size_t i;

  *value_size = (size_t)length;
  for (i = 0; i < *value_size; ++i) {
    value[i] = (char)('a' +
                     (int)((sequence + (uint64_t)i) % UINT64_C(26)));
  }
}

static bool random_put(cldb_memtable_t *memtable, model_t *model,
                       cldb_random_t *rnd, size_t key_index,
                       cldb_sequence_number_t sequence) {
  char value[kModelValueCapacity];
  size_t value_size;
  cldb_slice_t key = memory_engine_key(key_index);

  fill_random_value(rnd, sequence, value, &value_size);
  if (!cldb_status_is_ok(
          cldb_memtable_add(memtable, sequence, CLDB_VALUE_TYPE_VALUE, key,
                            cldb_slice_make(value, value_size)))) {
    return false;
  }
  return model_add_record(model, key, sequence, CLDB_VALUE_TYPE_VALUE,
                          cldb_slice_make(value, value_size));
}

static bool random_delete(cldb_memtable_t *memtable, model_t *model,
                          size_t key_index,
                          cldb_sequence_number_t sequence) {
  cldb_slice_t key = memory_engine_key(key_index);

  if (!cldb_status_is_ok(cldb_memtable_add(
          memtable, sequence, CLDB_VALUE_TYPE_DELETION, key,
          cldb_slice_empty()))) {
    return false;
  }
  return model_add_record(model, key, sequence, CLDB_VALUE_TYPE_DELETION,
                          cldb_slice_empty());
}

/* 跑一轮随机模型；失败时已分配资源仍全部释放。 */
static bool run_random_model(uint32_t seed) {
  cldb_internal_key_comparator_t comparator;
  cldb_memtable_t *memtable;
  cldb_snapshot_list_t snapshot_list;
  cldb_snapshot_t *live_snapshots[kModelSnapshotCapacity];
  cldb_random_t rnd;
  model_t model;
  cldb_sequence_number_t last_sequence = 0;
  size_t snapshot_count = 0;
  size_t step;
  size_t i;
  bool ok = true;

  memset(&model, 0, sizeof(model));
  cldb_internal_key_comparator_init(&comparator,
                                    cldb_bytewise_comparator());
  memtable = cldb_memtable_create(&comparator);
  if (memtable == NULL) {
    return false;
  }
  cldb_snapshot_list_init(&snapshot_list);
  cldb_random_init(&rnd, seed);

  for (step = 0; step < (size_t)kRandomModelSteps && ok; ++step) {
    uint32_t op;
    bool wrote = false;

    if (snapshot_count < (size_t)kModelSnapshotCapacity &&
        cldb_random_one_in(&rnd, UINT32_C(16))) {
      cldb_snapshot_t *snapshot =
          cldb_snapshot_list_new(&snapshot_list, last_sequence);

      if (snapshot == NULL) {
        ok = false;
        break;
      }
      live_snapshots[snapshot_count++] = snapshot;
    }
    if (snapshot_count > 0 &&
        cldb_random_one_in(&rnd, UINT32_C(64))) {
      size_t index =
          (size_t)cldb_random_uniform(&rnd, (uint32_t)snapshot_count);

      cldb_snapshot_list_delete(&snapshot_list, live_snapshots[index]);
      live_snapshots[index] = live_snapshots[snapshot_count - 1U];
      --snapshot_count;
    }

    op = cldb_random_uniform(&rnd, UINT32_C(10));
    if (op < UINT32_C(4)) {
      size_t key_index = (size_t)cldb_random_uniform(
          &rnd, (uint32_t)kMemoryEngineKeyCount);
      cldb_sequence_number_t sequence = last_sequence + 1U;

      if (!random_put(memtable, &model, &rnd, key_index, sequence)) {
        ok = false;
        break;
      }
      last_sequence = sequence;
      wrote = true;
    } else if (op < UINT32_C(6)) {
      size_t key_index = (size_t)cldb_random_uniform(
          &rnd, (uint32_t)kMemoryEngineKeyCount);
      cldb_sequence_number_t sequence = last_sequence + 1U;

      if (!random_delete(memtable, &model, key_index, sequence)) {
        ok = false;
        break;
      }
      last_sequence = sequence;
      wrote = true;
    } else if (op < UINT32_C(9)) {
      cldb_sequence_number_t snapshot = last_sequence;
      size_t key_index = (size_t)cldb_random_uniform(
          &rnd, (uint32_t)kMemoryEngineKeyCount);

      if (snapshot_count > 0 && cldb_random_one_in(&rnd, UINT32_C(2))) {
        size_t index =
            (size_t)cldb_random_uniform(&rnd, (uint32_t)snapshot_count);

        snapshot = cldb_snapshot_sequence(live_snapshots[index]);
      }
      if (!memtable_get_matches(memtable, &model,
                                memory_engine_key(key_index), snapshot)) {
        ok = false;
        break;
      }
    } else if (!model_iteration_matches(memtable, &model)) {
      ok = false;
      break;
    }

    /* 每次写入后抽查一个存活快照，验证固定 sequence 的读取不受新写影响。 */
    if (ok && wrote && snapshot_count > 0) {
      size_t snapshot_index =
          (size_t)cldb_random_uniform(&rnd, (uint32_t)snapshot_count);
      size_t key_index = (size_t)cldb_random_uniform(
          &rnd, (uint32_t)kMemoryEngineKeyCount);
      cldb_sequence_number_t snapshot =
          cldb_snapshot_sequence(live_snapshots[snapshot_index]);

      if (!memtable_get_matches(memtable, &model,
                                memory_engine_key(key_index), snapshot)) {
        ok = false;
        break;
      }
    }
  }

  /* 最终再用所有存活快照扫一遍全部 key。 */
  for (i = 0; i < snapshot_count && ok; ++i) {
    size_t key_index;

    for (key_index = 0;
         key_index < (size_t)kMemoryEngineKeyCount; ++key_index) {
      if (!memtable_get_matches(
              memtable, &model, memory_engine_key(key_index),
              cldb_snapshot_sequence(live_snapshots[i]))) {
        ok = false;
        break;
      }
    }
  }

  if (ok) {
    const cldb_snapshot_t *oldest =
        cldb_snapshot_list_oldest(&snapshot_list);
    const cldb_snapshot_t *newest =
        cldb_snapshot_list_newest(&snapshot_list);

    if (snapshot_count == 0) {
      ok = cldb_snapshot_list_empty(&snapshot_list);
    } else {
      ok = oldest != NULL && newest != NULL &&
           cldb_snapshot_sequence(oldest) <=
               cldb_snapshot_sequence(newest);
    }
  }

  for (i = 0; i < snapshot_count; ++i) {
    cldb_snapshot_list_delete(&snapshot_list, live_snapshots[i]);
  }
  if (!cldb_snapshot_list_empty(&snapshot_list)) {
    ok = false;
  }
  cldb_memtable_unref(memtable);
  return ok;
}

CLDB_TEST(memory_engine_random_model) {
  size_t run;

  for (run = 0; run < (size_t)kRandomModelRuns; ++run) {
    CLDB_EXPECT_TRUE(run_random_model(
        UINT32_C(1234) + (uint32_t)run * UINT32_C(97)));
  }
}

static const char *const kBatchLookupKeys[] = {
    "a", "b", "c", "x", "y", "missing"};

enum {
  kBatchLookupKeyCount =
      (int)(sizeof(kBatchLookupKeys) / sizeof(kBatchLookupKeys[0]))
};

/* 在给定快照集合上核对每个 lookup key。 */
static void expect_gets_at_snapshots(
    const cldb_memtable_t *memtable, const model_t *model,
    const cldb_sequence_number_t *snapshots, size_t snapshot_count) {
  size_t i;
  size_t key_index;

  for (i = 0; i < snapshot_count; ++i) {
    for (key_index = 0; key_index < (size_t)kBatchLookupKeyCount;
         ++key_index) {
      CLDB_EXPECT_TRUE(memtable_get_matches(
          memtable, model, literal(kBatchLookupKeys[key_index]),
          snapshots[i]));
    }
  }
}

CLDB_TEST(write_batch_sequence_boundaries) {
  static const cldb_sequence_number_t first_batch_snapshots[] = {
      0, 1, 4, 5, 6, 7, 8, 9, 10};
  static const cldb_sequence_number_t last_batch_snapshots[] = {
      CLDB_MAX_SEQUENCE_NUMBER - 2, CLDB_MAX_SEQUENCE_NUMBER - 1,
      CLDB_MAX_SEQUENCE_NUMBER};
  cldb_internal_key_comparator_t comparator;
  cldb_memtable_t *memtable;
  cldb_write_batch_t *batch;
  cldb_snapshot_list_t snapshots;
  cldb_snapshot_t *old_snapshot;
  cldb_snapshot_t *final_snapshot;
  cldb_snapshot_t *max_snapshot;
  model_t model;

  memset(&model, 0, sizeof(model));
  cldb_internal_key_comparator_init(&comparator,
                                    cldb_bytewise_comparator());
  memtable = cldb_memtable_create(&comparator);
  CLDB_ASSERT_TRUE(memtable != NULL);
  batch = cldb_write_batch_create();
  CLDB_ASSERT_TRUE(batch != NULL);
  cldb_snapshot_list_init(&snapshots);

  CLDB_ASSERT_TRUE(cldb_status_is_ok(cldb_memtable_add(
      memtable, UINT64_C(1), CLDB_VALUE_TYPE_VALUE, literal("a"),
      literal("old"))));
  CLDB_ASSERT_TRUE(model_add_record(&model, literal("a"), UINT64_C(1),
                                    CLDB_VALUE_TYPE_VALUE, literal("old")));
  old_snapshot = cldb_snapshot_list_new(&snapshots, UINT64_C(1));
  CLDB_ASSERT_TRUE(old_snapshot != NULL);

  /* seq 5..8：覆盖同 key 新值、删除、不同 key 以及批内再次删除。 */
  CLDB_ASSERT_TRUE(cldb_write_batch_set_sequence(batch, UINT64_C(5)));
  CLDB_ASSERT_TRUE(cldb_write_batch_put(batch, literal("a"),
                                        literal("batch-a")));
  CLDB_ASSERT_TRUE(cldb_write_batch_delete(batch, literal("b")));
  CLDB_ASSERT_TRUE(cldb_write_batch_put(batch, literal("c"),
                                        literal("seven")));
  CLDB_ASSERT_TRUE(cldb_write_batch_delete(batch, literal("a")));
  CLDB_ASSERT_TRUE(
      cldb_status_is_ok(cldb_write_batch_insert_into(batch, memtable)));
  CLDB_EXPECT_EQ_UINT(5, cldb_write_batch_sequence(batch));
  CLDB_EXPECT_EQ_UINT(4, cldb_write_batch_count(batch));

  CLDB_ASSERT_TRUE(model_add_record(&model, literal("a"), UINT64_C(5),
                                    CLDB_VALUE_TYPE_VALUE,
                                    literal("batch-a")));
  CLDB_ASSERT_TRUE(model_add_record(&model, literal("b"), UINT64_C(6),
                                    CLDB_VALUE_TYPE_DELETION,
                                    cldb_slice_empty()));
  CLDB_ASSERT_TRUE(model_add_record(&model, literal("c"), UINT64_C(7),
                                    CLDB_VALUE_TYPE_VALUE,
                                    literal("seven")));
  CLDB_ASSERT_TRUE(model_add_record(&model, literal("a"), UINT64_C(8),
                                    CLDB_VALUE_TYPE_DELETION,
                                    cldb_slice_empty()));
  final_snapshot = cldb_snapshot_list_new(&snapshots, UINT64_C(8));
  CLDB_ASSERT_TRUE(final_snapshot != NULL);
  CLDB_EXPECT_EQ_PTR(old_snapshot,
                     cldb_snapshot_list_oldest(&snapshots));
  CLDB_EXPECT_EQ_PTR(final_snapshot,
                     cldb_snapshot_list_newest(&snapshots));

  expect_gets_at_snapshots(
      memtable, &model, first_batch_snapshots,
      sizeof(first_batch_snapshots) / sizeof(first_batch_snapshots[0]));
  CLDB_EXPECT_TRUE(model_iteration_matches(memtable, &model));

  /* 第二批从 max-1 开始，覆盖 sequence 上界以及最后一条使用 max。 */
  CLDB_ASSERT_TRUE(cldb_write_batch_clear(batch));
  CLDB_ASSERT_TRUE(cldb_write_batch_set_sequence(
      batch, CLDB_MAX_SEQUENCE_NUMBER - UINT64_C(1)));
  CLDB_ASSERT_TRUE(cldb_write_batch_put(batch, literal("x"),
                                        literal("x-max-1")));
  CLDB_ASSERT_TRUE(cldb_write_batch_put(batch, literal("y"),
                                        literal("y-max")));
  CLDB_ASSERT_TRUE(
      cldb_status_is_ok(cldb_write_batch_insert_into(batch, memtable)));
  CLDB_EXPECT_EQ_UINT(CLDB_MAX_SEQUENCE_NUMBER - UINT64_C(1),
                      cldb_write_batch_sequence(batch));
  CLDB_EXPECT_EQ_UINT(2, cldb_write_batch_count(batch));

  CLDB_ASSERT_TRUE(model_add_record(
      &model, literal("x"), CLDB_MAX_SEQUENCE_NUMBER - UINT64_C(1),
      CLDB_VALUE_TYPE_VALUE, literal("x-max-1")));
  CLDB_ASSERT_TRUE(model_add_record(
      &model, literal("y"), CLDB_MAX_SEQUENCE_NUMBER,
      CLDB_VALUE_TYPE_VALUE, literal("y-max")));
  expect_gets_at_snapshots(
      memtable, &model, last_batch_snapshots,
      sizeof(last_batch_snapshots) / sizeof(last_batch_snapshots[0]));
  CLDB_EXPECT_TRUE(model_iteration_matches(memtable, &model));

  max_snapshot =
      cldb_snapshot_list_new(&snapshots, CLDB_MAX_SEQUENCE_NUMBER);
  CLDB_ASSERT_TRUE(max_snapshot != NULL);
  CLDB_EXPECT_EQ_PTR(old_snapshot,
                     cldb_snapshot_list_oldest(&snapshots));
  CLDB_EXPECT_EQ_PTR(max_snapshot,
                     cldb_snapshot_list_newest(&snapshots));

  cldb_snapshot_list_delete(&snapshots, old_snapshot);
  CLDB_EXPECT_EQ_PTR(final_snapshot,
                     cldb_snapshot_list_oldest(&snapshots));
  cldb_snapshot_list_delete(&snapshots, final_snapshot);
  CLDB_EXPECT_EQ_PTR(max_snapshot,
                     cldb_snapshot_list_oldest(&snapshots));
  cldb_snapshot_list_delete(&snapshots, max_snapshot);
  CLDB_EXPECT_TRUE(cldb_snapshot_list_empty(&snapshots));

  cldb_write_batch_destroy(batch);
  cldb_memtable_unref(memtable);
}

/* 并发读者状态；error 只由本线程写入，主线程 join 后读取。 */
typedef struct concurrent_reader_state {
  const cldb_memtable_t *memtable;              /* 与写者共享，只读 */
  const cldb_internal_key_comparator_t *comparator; /* 仅用于校验迭代顺序 */
  cldb_sequence_number_t snapshot_sequence;      /* 固定读取快照 */
  cldb_atomic_u32_t *quit;                       /* release 置 1 后退出 */
  cldb_atomic_u32_t *readers_ready; /* 每名读者完成首轮后 release 加一 */
  uint32_t error;                   /* 非 0 表示本线程校验失败 */
} concurrent_reader_state_t;

/* 并发写者状态；只由单写线程使用 memtable 与 batch。 */
typedef struct concurrent_writer_state {
  cldb_memtable_t *memtable; /* 单写者 */
  cldb_write_batch_t *batch; /* 单写者复用 */
  cldb_atomic_u32_t *quit;   /* 完成后 release 置 1 */
  cldb_atomic_u32_t *readers_ready; /* acquire 等待全部读者首轮完成 */
  uint32_t reader_count;            /* 等待的读者数 */
  cldb_sequence_number_t next_sequence; /* 下一批的起始 sequence */
  uint32_t error;                       /* 非 0 表示写路径失败 */
} concurrent_writer_state_t;

enum {
  kConcurrentReaderGetError = 1,
  kConcurrentReaderScanError = 2,
  kConcurrentWriterBatchError = 3,
  kConcurrentWriterSleepError = 4
};

/* 固定快照读取：偶数 key 是最初 Value，奇数 key 是最初 Deletion；
 * 写者只追加更大 sequence，因此结果必须始终不变。 */
static bool concurrent_reader_check_get(
    const concurrent_reader_state_t *state) {
  size_t i;

  for (i = 0; i < (size_t)kMemoryEngineKeyCount; ++i) {
    cldb_lookup_key_t lookup;
    cldb_slice_t value = cldb_slice_empty();
    cldb_status_t status = cldb_status_ok();
    bool found;

    if (!cldb_lookup_key_init(&lookup, memory_engine_key(i),
                              state->snapshot_sequence)) {
      return false;
    }
    found =
        cldb_memtable_get(state->memtable, &lookup, &value, &status);
    if (!found) {
      cldb_lookup_key_destroy(&lookup);
      return false;
    }
    if ((i % 2U) == 0U) {
      if (!cldb_status_is_ok(status) ||
          !cldb_slice_equal(value, literal("base"))) {
        cldb_lookup_key_destroy(&lookup);
        return false;
      }
    } else if (!cldb_status_is_not_found(status) || value.size != 0) {
      cldb_lookup_key_destroy(&lookup);
      return false;
    }
    cldb_lookup_key_destroy(&lookup);
  }
  return true;
}

/* 并发正向迭代：检查内部键可解析、严格递增、value 可解析，且每个初始
 * 条目都完整出现一次；新写入可能出现在任意位置，因此不要求具体条数。 */
static bool concurrent_reader_check_scan(
    const concurrent_reader_state_t *state) {
  cldb_iterator_t *iterator =
      cldb_memtable_new_iterator(state->memtable);
  cldb_slice_t previous = cldb_slice_empty();
  bool seen[kMemoryEngineKeyCount] = {false};
  bool has_previous = false;
  uint32_t count = 0;
  size_t seen_key;
  bool ok = true;

  if (iterator == NULL) {
    return false;
  }
  cldb_iter_seek_to_first(iterator);
  while (cldb_iter_valid(iterator)) {
    cldb_slice_t key = cldb_iter_key(iterator);
    cldb_slice_t value = cldb_iter_value(iterator);
    cldb_parsed_internal_key_t parsed;

    if (!cldb_parse_internal_key(key, &parsed) ||
        !cldb_slice_is_valid(value) ||
        (has_previous &&
         cldb_comparator_compare(&state->comparator->base, previous, key) >=
             0)) {
      ok = false;
      break;
    }
    if (parsed.sequence <= (cldb_sequence_number_t)kMemoryEngineKeyCount) {
      size_t baseline_key;
      bool matched = false;

      for (baseline_key = 0;
           baseline_key < (size_t)kMemoryEngineKeyCount;
           ++baseline_key) {
        cldb_value_type_t expected_type =
            (baseline_key % 2U) == 0U ? CLDB_VALUE_TYPE_VALUE
                                     : CLDB_VALUE_TYPE_DELETION;
        cldb_slice_t expected_value =
            expected_type == CLDB_VALUE_TYPE_VALUE
                ? literal("base")
                : cldb_slice_empty();

        if (cldb_slice_equal(parsed.user_key,
                             memory_engine_key(baseline_key))) {
          matched = true;
          if (seen[baseline_key] ||
              parsed.sequence !=
                  (cldb_sequence_number_t)baseline_key + 1U ||
              parsed.type != expected_type ||
              !cldb_slice_equal(value, expected_value)) {
            ok = false;
          }
          seen[baseline_key] = true;
          break;
        }
      }
      if (!matched) {
        ok = false;
      }
    }
    if (!ok) {
      break;
    }
    previous = key;
    has_previous = true;
    ++count;
    cldb_iter_next(iterator);
  }
  if (!cldb_status_is_ok(cldb_iter_status(iterator)) ||
      count < (uint32_t)kMemoryEngineKeyCount) {
    ok = false;
  }
  for (seen_key = 0; seen_key < (size_t)kMemoryEngineKeyCount;
       ++seen_key) {
    if (!seen[seen_key]) {
      ok = false;
    }
  }
  cldb_iter_destroy(iterator);
  return ok;
}

static void *concurrent_reader(void *arg) {
  concurrent_reader_state_t *state = arg;
  bool announced = false;

  for (;;) {
    if (!concurrent_reader_check_get(state)) {
      state->error = kConcurrentReaderGetError;
      break;
    }
    if (!concurrent_reader_check_scan(state)) {
      state->error = kConcurrentReaderScanError;
      break;
    }
    if (!announced) {
      cldb_atomic_u32_fetch_add(state->readers_ready, 1,
                                memory_order_release);
      announced = true;
    }
    if (cldb_atomic_u32_load(state->quit, memory_order_acquire) != 0) {
      break;
    }
  }
  if (!announced) {
    cldb_atomic_u32_fetch_add(state->readers_ready, 1,
                              memory_order_release);
  }
  return NULL;
}

/* 等全部读者至少完成一轮后，按固定大小分批写入，使读写确实重叠。 */
static bool concurrent_writer_run(concurrent_writer_state_t *state) {
  uint32_t written = 0;

  while (cldb_atomic_u32_load(state->readers_ready, memory_order_acquire) <
         state->reader_count) {
    if (!cldb_status_is_ok(cldb_sleep_for_micros(UINT64_C(100)))) {
      state->error = kConcurrentWriterSleepError;
      return false;
    }
  }

  while (written < (uint32_t)kConcurrentWriterWrites) {
    uint32_t remaining =
        (uint32_t)kConcurrentWriterWrites - written;
    uint32_t count = remaining < (uint32_t)kConcurrentBatchSize
                         ? remaining
                         : (uint32_t)kConcurrentBatchSize;
    uint32_t j;

    if (!cldb_write_batch_clear(state->batch) ||
        !cldb_write_batch_set_sequence(state->batch,
                                       state->next_sequence)) {
      state->error = kConcurrentWriterBatchError;
      return false;
    }
    for (j = 0; j < count; ++j) {
      uint32_t index = written + j;
      size_t key_index =
          (size_t)(index % (uint32_t)kMemoryEngineKeyCount);
      cldb_slice_t key = memory_engine_key(key_index);
      bool is_value = (index % 2U) == 0U;

      if (is_value) {
        if (!cldb_write_batch_put(state->batch, key, literal("new"))) {
          state->error = kConcurrentWriterBatchError;
          return false;
        }
      } else if (!cldb_write_batch_delete(state->batch, key)) {
        state->error = kConcurrentWriterBatchError;
        return false;
      }
    }
    if (!cldb_status_is_ok(
            cldb_write_batch_insert_into(state->batch, state->memtable))) {
      state->error = kConcurrentWriterBatchError;
      return false;
    }
    written += count;
    state->next_sequence += (cldb_sequence_number_t)count;
  }
  return true;
}

static void *concurrent_writer(void *arg) {
  concurrent_writer_state_t *state = arg;

  (void)concurrent_writer_run(state);
  cldb_atomic_u32_store(state->quit, 1, memory_order_release);
  return NULL;
}

static bool memtable_entry_count_is(const cldb_memtable_t *memtable,
                                    size_t expected) {
  cldb_iterator_t *iterator = cldb_memtable_new_iterator(memtable);
  size_t count = 0;
  bool ok;

  if (iterator == NULL) {
    return false;
  }
  cldb_iter_seek_to_first(iterator);
  while (cldb_iter_valid(iterator)) {
    ++count;
    cldb_iter_next(iterator);
  }
  ok = cldb_status_is_ok(cldb_iter_status(iterator)) &&
       count == expected;
  cldb_iter_destroy(iterator);
  return ok;
}

CLDB_TEST(memory_engine_concurrent_snapshot_reads) {
  cldb_internal_key_comparator_t comparator;
  cldb_memtable_t *memtable;
  cldb_snapshot_list_t snapshots;
  cldb_snapshot_t *snapshot;
  cldb_write_batch_t *batch;
  cldb_atomic_u32_t quit;
  cldb_atomic_u32_t readers_ready;
  cldb_thread_t writer_thread;
  cldb_thread_t reader_threads[kConcurrentReaderCount];
  concurrent_writer_state_t writer_state;
  concurrent_reader_state_t reader_states[kConcurrentReaderCount];
  bool reader_started[kConcurrentReaderCount] = {false, false, false};
  bool writer_started = false;
  bool ok = true;
  size_t i;

  cldb_internal_key_comparator_init(&comparator,
                                    cldb_bytewise_comparator());
  memtable = cldb_memtable_create(&comparator);
  CLDB_ASSERT_TRUE(memtable != NULL);
  batch = cldb_write_batch_create();
  CLDB_ASSERT_TRUE(batch != NULL);
  cldb_snapshot_list_init(&snapshots);

  for (i = 0; i < (size_t)kMemoryEngineKeyCount; ++i) {
    cldb_value_type_t type = (i % 2U) == 0U
                                 ? CLDB_VALUE_TYPE_VALUE
                                 : CLDB_VALUE_TYPE_DELETION;
    cldb_slice_t value = type == CLDB_VALUE_TYPE_VALUE
                             ? literal("base")
                             : cldb_slice_empty();

    CLDB_ASSERT_TRUE(cldb_status_is_ok(cldb_memtable_add(
        memtable, (cldb_sequence_number_t)i + 1U, type,
        memory_engine_key(i), value)));
  }
  snapshot = cldb_snapshot_list_new(
      &snapshots, (cldb_sequence_number_t)kMemoryEngineKeyCount);
  CLDB_ASSERT_TRUE(snapshot != NULL);

  cldb_atomic_u32_init(&quit, 0);
  cldb_atomic_u32_init(&readers_ready, 0);
  writer_state.memtable = memtable;
  writer_state.batch = batch;
  writer_state.quit = &quit;
  writer_state.readers_ready = &readers_ready;
  writer_state.reader_count = (uint32_t)kConcurrentReaderCount;
  writer_state.next_sequence =
      (cldb_sequence_number_t)kMemoryEngineKeyCount + 1U;
  writer_state.error = 0;

  for (i = 0; i < (size_t)kConcurrentReaderCount; ++i) {
    cldb_status_t status;

    reader_states[i].memtable = memtable;
    reader_states[i].comparator = &comparator;
    reader_states[i].snapshot_sequence =
        cldb_snapshot_sequence(snapshot);
    reader_states[i].quit = &quit;
    reader_states[i].readers_ready = &readers_ready;
    reader_states[i].error = 0;
    cldb_thread_init(&reader_threads[i]);
    status = cldb_thread_start(&reader_threads[i], concurrent_reader,
                               &reader_states[i]);
    if (!cldb_status_is_ok(status)) {
      CLDB_EXPECT_TRUE(cldb_status_is_ok(status));
      ok = false;
      break;
    }
    reader_started[i] = true;
  }

  if (ok) {
    cldb_status_t status;

    cldb_thread_init(&writer_thread);
    status = cldb_thread_start(&writer_thread, concurrent_writer,
                               &writer_state);
    if (cldb_status_is_ok(status)) {
      writer_started = true;
    } else {
      CLDB_EXPECT_TRUE(cldb_status_is_ok(status));
      ok = false;
    }
  }
  if (writer_started) {
    cldb_status_t status = cldb_thread_join(&writer_thread, NULL);

    if (!cldb_status_is_ok(status)) {
      CLDB_EXPECT_TRUE(cldb_status_is_ok(status));
      ok = false;
    }
  } else {
    cldb_atomic_u32_store(&quit, 1, memory_order_release);
  }
  cldb_atomic_u32_store(&quit, 1, memory_order_release);

  for (i = 0; i < (size_t)kConcurrentReaderCount; ++i) {
    cldb_status_t status;

    if (!reader_started[i]) {
      continue;
    }
    status = cldb_thread_join(&reader_threads[i], NULL);
    if (!cldb_status_is_ok(status)) {
      CLDB_EXPECT_TRUE(cldb_status_is_ok(status));
      ok = false;
    }
  }

  CLDB_EXPECT_EQ_UINT(0, writer_state.error);
  for (i = 0; i < (size_t)kConcurrentReaderCount; ++i) {
    CLDB_EXPECT_EQ_UINT(0, reader_states[i].error);
  }
  if (writer_state.error != 0) {
    ok = false;
  }
  for (i = 0; i < (size_t)kConcurrentReaderCount; ++i) {
    if (reader_states[i].error != 0) {
      ok = false;
    }
  }

  CLDB_EXPECT_TRUE(memtable_entry_count_is(
      memtable,
      (size_t)kMemoryEngineKeyCount +
          (size_t)kConcurrentWriterWrites));
  CLDB_EXPECT_EQ_UINT(
      kMemoryEngineKeyCount,
      cldb_snapshot_sequence(cldb_snapshot_list_oldest(&snapshots)));
  CLDB_EXPECT_EQ_UINT(
      kMemoryEngineKeyCount,
      cldb_snapshot_sequence(cldb_snapshot_list_newest(&snapshots)));

  cldb_snapshot_list_delete(&snapshots, snapshot);
  CLDB_EXPECT_TRUE(cldb_snapshot_list_empty(&snapshots));
  cldb_write_batch_destroy(batch);
  cldb_memtable_unref(memtable);
  CLDB_EXPECT_TRUE(ok);
}

void cldb_register_memory_engine_tests(void) {
  CLDB_RUN_TEST(memory_engine_random_model);
  CLDB_RUN_TEST(write_batch_sequence_boundaries);
  CLDB_RUN_TEST(memory_engine_concurrent_snapshot_reads);
}
