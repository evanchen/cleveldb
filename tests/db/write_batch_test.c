#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "cleveldb/buf.h"
#include "cleveldb/slice.h"
#include "cleveldb/status.h"
#include "cleveldb/write_batch.h"
#include "db/dbformat.h"
#include "db/write_batch_internal.h"
#include "harness.h"
#include "status_test_util.h"
#include "util/coding.h"

/* WriteBatch 编码、合并、遍历与错误路径；对应上游 db/write_batch_test.cc，
 * 并补充自追加、所有权和损坏输入用例。 */

enum { kMaxRecordedOps = 8 };

typedef struct recorded_op {
  bool is_put;
  uint64_t sequence;
  cldb_buf_t key;   /* 回调参数的副本 */
  cldb_buf_t value; /* Delete 时为空 */
} recorded_op_t;

typedef struct recording_handler {
  cldb_write_batch_handler_t base; /* 必须为首成员 */
  recorded_op_t ops[kMaxRecordedOps];
  size_t count;
  uint64_t next_sequence;
  size_t fail_at; /* 第几个回调注入失败；SIZE_MAX 表示不失败 */
} recording_handler_t;

/* 复制回调参数并记录模拟的递增 sequence。 */
static cldb_status_t record_op(recording_handler_t *self, bool is_put,
                               cldb_slice_t key, cldb_slice_t value) {
  recorded_op_t *op;

  if (self->count == self->fail_at) {
    return cldb_status_io("recording handler", "injected failure");
  }
  if (self->count >= kMaxRecordedOps) {
    return cldb_status_corruption("recording handler", "too many operations");
  }
  op = &self->ops[self->count];
  op->is_put = is_put;
  op->sequence = self->next_sequence;
  if (!cldb_buf_assign(&op->key, key.data, key.size) ||
      !cldb_buf_assign(&op->value, value.data, value.size)) {
    return cldb_status_io("recording handler", "copy failed");
  }
  ++self->count;
  ++self->next_sequence;
  return cldb_status_ok();
}

static cldb_status_t recording_put(cldb_write_batch_handler_t *handler,
                                   cldb_slice_t key, cldb_slice_t value) {
  return record_op((recording_handler_t *)handler, true, key, value);
}

static cldb_status_t recording_delete(cldb_write_batch_handler_t *handler,
                                     cldb_slice_t key) {
  return record_op((recording_handler_t *)handler, false, key,
                   cldb_slice_empty());
}

static const cldb_write_batch_handler_vtbl_t kRecordingHandlerVtbl = {
    .put = recording_put,
    .delete = recording_delete,
};

static void recording_init(recording_handler_t *self, uint64_t sequence) {
  size_t i;

  memset(self, 0, sizeof(*self));
  self->base.vtbl = &kRecordingHandlerVtbl;
  self->next_sequence = sequence;
  self->fail_at = SIZE_MAX;
  for (i = 0; i < kMaxRecordedOps; ++i) {
    cldb_buf_init(&self->ops[i].key);
    cldb_buf_init(&self->ops[i].value);
  }
}

static void recording_destroy(recording_handler_t *self) {
  size_t i;

  for (i = 0; i < kMaxRecordedOps; ++i) {
    cldb_buf_free(&self->ops[i].key);
    cldb_buf_free(&self->ops[i].value);
  }
}

static void recording_reset(recording_handler_t *self, uint64_t sequence) {
  recording_destroy(self);
  recording_init(self, sequence);
}

/* 检查第 index 条记录的副本内容。 */
static void expect_op(const recording_handler_t *self, size_t index,
                      bool is_put, uint64_t sequence, cldb_slice_t key,
                      cldb_slice_t value) {
  const recorded_op_t *op;

  CLDB_EXPECT_TRUE(index < self->count);
  if (index >= self->count) {
    return;
  }
  op = &self->ops[index];
  CLDB_EXPECT_EQ_INT(is_put, op->is_put);
  CLDB_EXPECT_EQ_UINT(sequence, op->sequence);
  CLDB_EXPECT_TRUE(cldb_slice_equal(key, cldb_buf_slice(&op->key)));
  CLDB_EXPECT_TRUE(cldb_slice_equal(value, cldb_buf_slice(&op->value)));
}

/* 用指定 count 和原始 payload 覆盖 batch 内容，sequence 为 0。 */
static void set_raw_contents(cldb_write_batch_t *batch, uint32_t count,
                             const char *payload, size_t payload_size) {
  cldb_buf_t raw;

  cldb_buf_init(&raw);
  CLDB_ASSERT_TRUE(cldb_buf_resize(
      &raw, (size_t)CLDB_WRITE_BATCH_HEADER_SIZE + payload_size));
  cldb_encode_fixed32(raw.data + 8, count);
  if (payload_size > 0) {
    memcpy(raw.data + CLDB_WRITE_BATCH_HEADER_SIZE, payload, payload_size);
  }
  CLDB_ASSERT_TRUE(cldb_write_batch_set_contents(batch, cldb_buf_slice(&raw)));
  cldb_buf_free(&raw);
}

/* 复制 contents，可选截掉末尾 remove 字节，并改写 count。 */
static void set_cloned_contents(cldb_write_batch_t *batch,
                                cldb_slice_t contents, size_t remove,
                                uint32_t count) {
  cldb_buf_t raw;

  cldb_buf_init(&raw);
  CLDB_ASSERT_TRUE(cldb_buf_assign(&raw, contents.data, contents.size));
  CLDB_ASSERT_TRUE(remove <= raw.size);
  CLDB_ASSERT_TRUE(cldb_buf_resize(&raw, raw.size - remove));
  cldb_encode_fixed32(raw.data + 8, count);
  CLDB_ASSERT_TRUE(cldb_write_batch_set_contents(batch, cldb_buf_slice(&raw)));
  cldb_buf_free(&raw);
}

CLDB_TEST(write_batch_empty_and_accessors) {
  static const char kZeroHeader[CLDB_WRITE_BATCH_HEADER_SIZE] = {0};
  cldb_write_batch_t *batch;
  cldb_write_batch_t stack_batch;
  recording_handler_t handler;
  cldb_slice_t contents;

  batch = cldb_write_batch_create();
  CLDB_ASSERT_TRUE(batch != NULL);
  CLDB_EXPECT_EQ_UINT(12, cldb_write_batch_byte_size(batch));
  CLDB_EXPECT_EQ_UINT(12, cldb_write_batch_approximate_size(batch));
  CLDB_EXPECT_EQ_UINT(0, cldb_write_batch_sequence(batch));
  CLDB_EXPECT_EQ_UINT(0, cldb_write_batch_count(batch));
  contents = cldb_write_batch_contents(batch);
  CLDB_ASSERT_EQ_UINT(sizeof(kZeroHeader), contents.size);
  CLDB_EXPECT_EQ_INT(0, memcmp(contents.data, kZeroHeader,
                               sizeof(kZeroHeader)));

  recording_init(&handler, 0);
  CLDB_EXPECT_OK(cldb_write_batch_iterate(batch, &handler.base));
  CLDB_EXPECT_EQ_UINT(0, handler.count);
  recording_destroy(&handler);
  cldb_write_batch_destroy(batch);

  CLDB_ASSERT_TRUE(cldb_write_batch_init(&stack_batch));
  CLDB_EXPECT_EQ_UINT(0, cldb_write_batch_count(&stack_batch));
  cldb_write_batch_free(&stack_batch);

  CLDB_EXPECT_EQ_UINT(0, cldb_write_batch_byte_size(NULL));
  CLDB_EXPECT_EQ_UINT(0, cldb_write_batch_approximate_size(NULL));
  CLDB_EXPECT_EQ_UINT(0, cldb_write_batch_sequence(NULL));
  CLDB_EXPECT_EQ_UINT(0, cldb_write_batch_count(NULL));
  CLDB_EXPECT_TRUE(cldb_slice_is_empty(cldb_write_batch_contents(NULL)));
  CLDB_EXPECT_FALSE(cldb_write_batch_clear(NULL));
  CLDB_EXPECT_FALSE(cldb_write_batch_init(NULL));
  cldb_write_batch_destroy(NULL);
}

CLDB_TEST(write_batch_encode_golden) {
  static const char kExpected[] = {
      0x64, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x03, 0x00, 0x00, 0x00,
      0x01, 0x03, 'f', 'o', 'o', 0x03, 'b', 'a', 'r',
      0x00, 0x03, 'b', 'o', 'x',
      0x01, 0x03, 'b', 'a', 'z', 0x03, 'b', 'o', 'o',
  };
  cldb_write_batch_t *batch;
  recording_handler_t handler;
  cldb_slice_t contents;

  batch = cldb_write_batch_create();
  CLDB_ASSERT_TRUE(batch != NULL);
  CLDB_ASSERT_TRUE(cldb_write_batch_set_sequence(batch, 100));
  CLDB_ASSERT_TRUE(cldb_write_batch_put(
      batch, cldb_slice_make("foo", 3), cldb_slice_make("bar", 3)));
  CLDB_ASSERT_TRUE(
      cldb_write_batch_delete(batch, cldb_slice_make("box", 3)));
  CLDB_ASSERT_TRUE(cldb_write_batch_put(
      batch, cldb_slice_make("baz", 3), cldb_slice_make("boo", 3)));
  CLDB_EXPECT_EQ_UINT(3, cldb_write_batch_count(batch));
  CLDB_EXPECT_EQ_UINT(100, cldb_write_batch_sequence(batch));
  CLDB_EXPECT_EQ_UINT(sizeof(kExpected), cldb_write_batch_byte_size(batch));
  CLDB_EXPECT_EQ_UINT(sizeof(kExpected),
                      cldb_write_batch_approximate_size(batch));
  contents = cldb_write_batch_contents(batch);
  CLDB_ASSERT_EQ_UINT(sizeof(kExpected), contents.size);
  CLDB_EXPECT_EQ_INT(0, memcmp(contents.data, kExpected, sizeof(kExpected)));

  recording_init(&handler, cldb_write_batch_sequence(batch));
  CLDB_EXPECT_OK(cldb_write_batch_iterate(batch, &handler.base));
  CLDB_EXPECT_EQ_UINT(3, handler.count);
  expect_op(&handler, 0, true, 100, cldb_slice_make("foo", 3),
            cldb_slice_make("bar", 3));
  expect_op(&handler, 1, false, 101, cldb_slice_make("box", 3),
            cldb_slice_empty());
  expect_op(&handler, 2, true, 102, cldb_slice_make("baz", 3),
            cldb_slice_make("boo", 3));
  CLDB_EXPECT_EQ_UINT(103, handler.next_sequence);
  recording_destroy(&handler);
  cldb_write_batch_destroy(batch);
}

CLDB_TEST(write_batch_empty_key_value) {
  cldb_write_batch_t *batch;
  recording_handler_t handler;

  batch = cldb_write_batch_create();
  CLDB_ASSERT_TRUE(batch != NULL);
  CLDB_ASSERT_TRUE(cldb_write_batch_set_sequence(batch, 1));
  CLDB_ASSERT_TRUE(cldb_write_batch_put(batch, cldb_slice_empty(),
                                        cldb_slice_empty()));
  CLDB_ASSERT_TRUE(cldb_write_batch_delete(batch, cldb_slice_empty()));
  CLDB_EXPECT_EQ_UINT(2, cldb_write_batch_count(batch));
  CLDB_EXPECT_EQ_UINT(17, cldb_write_batch_byte_size(batch));

  recording_init(&handler, 1);
  CLDB_EXPECT_OK(cldb_write_batch_iterate(batch, &handler.base));
  CLDB_EXPECT_EQ_UINT(2, handler.count);
  expect_op(&handler, 0, true, 1, cldb_slice_empty(), cldb_slice_empty());
  expect_op(&handler, 1, false, 2, cldb_slice_empty(), cldb_slice_empty());
  recording_destroy(&handler);
  cldb_write_batch_destroy(batch);
}

CLDB_TEST(write_batch_append_and_sequence) {
  cldb_write_batch_t *first;
  cldb_write_batch_t *second;
  recording_handler_t handler;
  size_t size0;
  size_t size1;
  size_t size2;
  size_t size3;
  size_t size4;

  first = cldb_write_batch_create();
  second = cldb_write_batch_create();
  CLDB_ASSERT_TRUE(first != NULL);
  CLDB_ASSERT_TRUE(second != NULL);
  CLDB_ASSERT_TRUE(cldb_write_batch_set_sequence(first, 200));
  CLDB_ASSERT_TRUE(cldb_write_batch_set_sequence(second, 300));

  size0 = cldb_write_batch_byte_size(first);
  CLDB_ASSERT_TRUE(cldb_write_batch_append(first, second));
  CLDB_EXPECT_EQ_UINT(0, cldb_write_batch_count(first));
  CLDB_EXPECT_EQ_UINT(200, cldb_write_batch_sequence(first));
  size1 = cldb_write_batch_byte_size(first);
  CLDB_EXPECT_EQ_UINT(size0, size1);

  CLDB_ASSERT_TRUE(cldb_write_batch_put(
      second, cldb_slice_make("a", 1), cldb_slice_make("va", 2)));
  CLDB_ASSERT_TRUE(cldb_write_batch_append(first, second));
  CLDB_EXPECT_EQ_UINT(1, cldb_write_batch_count(first));
  size2 = cldb_write_batch_byte_size(first);
  CLDB_EXPECT_TRUE(size2 > size1);

  CLDB_ASSERT_TRUE(cldb_write_batch_clear(second));
  CLDB_EXPECT_EQ_UINT(0, cldb_write_batch_sequence(second));
  CLDB_ASSERT_TRUE(cldb_write_batch_put(
      second, cldb_slice_make("b", 1), cldb_slice_make("vb", 2)));
  CLDB_ASSERT_TRUE(cldb_write_batch_append(first, second));
  CLDB_EXPECT_EQ_UINT(2, cldb_write_batch_count(first));
  size3 = cldb_write_batch_byte_size(first);
  CLDB_EXPECT_TRUE(size3 > size2);

  CLDB_ASSERT_TRUE(
      cldb_write_batch_delete(second, cldb_slice_make("foo", 3)));
  CLDB_ASSERT_TRUE(cldb_write_batch_append(first, second));
  CLDB_EXPECT_EQ_UINT(4, cldb_write_batch_count(first));
  CLDB_EXPECT_EQ_UINT(200, cldb_write_batch_sequence(first));
  size4 = cldb_write_batch_byte_size(first);
  CLDB_EXPECT_TRUE(size4 > size3);

  recording_init(&handler, cldb_write_batch_sequence(first));
  CLDB_EXPECT_OK(cldb_write_batch_iterate(first, &handler.base));
  CLDB_EXPECT_EQ_UINT(4, handler.count);
  expect_op(&handler, 0, true, 200, cldb_slice_make("a", 1),
            cldb_slice_make("va", 2));
  expect_op(&handler, 1, true, 201, cldb_slice_make("b", 1),
            cldb_slice_make("vb", 2));
  expect_op(&handler, 2, true, 202, cldb_slice_make("b", 1),
            cldb_slice_make("vb", 2));
  expect_op(&handler, 3, false, 203, cldb_slice_make("foo", 3),
            cldb_slice_empty());
  recording_destroy(&handler);

  CLDB_ASSERT_TRUE(cldb_write_batch_clear(first));
  CLDB_EXPECT_EQ_UINT(0, cldb_write_batch_count(first));
  CLDB_EXPECT_EQ_UINT(0, cldb_write_batch_sequence(first));
  CLDB_EXPECT_EQ_UINT(12, cldb_write_batch_byte_size(first));

  cldb_write_batch_destroy(first);
  cldb_write_batch_destroy(second);
}

CLDB_TEST(write_batch_append_self) {
  cldb_buf_t value;
  cldb_write_batch_t *batch;
  recording_handler_t handler;
  cldb_slice_t value_slice;
  size_t old_size;
  size_t old_capacity;
  size_t payload;
  size_t new_size;
  size_t i;

  cldb_buf_init(&value);
  CLDB_ASSERT_TRUE(cldb_buf_resize(&value, 1500));
  for (i = 0; i < value.size; ++i) {
    value.data[i] = (char)('a' + (int)(i % 26));
  }
  batch = cldb_write_batch_create();
  CLDB_ASSERT_TRUE(batch != NULL);
  CLDB_ASSERT_TRUE(cldb_write_batch_set_sequence(batch, 77));
  value_slice = cldb_buf_slice(&value);
  CLDB_ASSERT_TRUE(
      cldb_write_batch_put(batch, cldb_slice_make("k", 1), value_slice));
  old_size = cldb_write_batch_byte_size(batch);
  old_capacity = batch->rep.capacity;
  payload = old_size - CLDB_WRITE_BATCH_HEADER_SIZE;

  CLDB_ASSERT_TRUE(cldb_write_batch_append(batch, batch));
  new_size = cldb_write_batch_byte_size(batch);
  CLDB_EXPECT_EQ_UINT(old_size + payload, new_size);
  /* 新长度超过旧容量，确认自追加确实触发了缓冲区扩容。 */
  CLDB_EXPECT_TRUE(new_size > old_capacity);
  CLDB_EXPECT_EQ_UINT(2, cldb_write_batch_count(batch));
  CLDB_EXPECT_EQ_UINT(77, cldb_write_batch_sequence(batch));

  recording_init(&handler, 77);
  CLDB_EXPECT_OK(cldb_write_batch_iterate(batch, &handler.base));
  CLDB_EXPECT_EQ_UINT(2, handler.count);
  expect_op(&handler, 0, true, 77, cldb_slice_make("k", 1), value_slice);
  expect_op(&handler, 1, true, 78, cldb_slice_make("k", 1), value_slice);
  recording_destroy(&handler);
  cldb_buf_free(&value);
  cldb_write_batch_destroy(batch);
}

CLDB_TEST(write_batch_large_value) {
  cldb_buf_t value;
  cldb_write_batch_t *batch;
  recording_handler_t handler;
  cldb_slice_t value_slice;
  size_t i;

  cldb_buf_init(&value);
  CLDB_ASSERT_TRUE(cldb_buf_resize(&value, (size_t)1 << 20));
  for (i = 0; i < value.size; ++i) {
    value.data[i] = (char)(i % 251);
  }
  batch = cldb_write_batch_create();
  CLDB_ASSERT_TRUE(batch != NULL);
  value_slice = cldb_buf_slice(&value);
  CLDB_ASSERT_TRUE(
      cldb_write_batch_put(batch, cldb_slice_make("big", 3), value_slice));
  CLDB_EXPECT_EQ_UINT(1, cldb_write_batch_count(batch));
  CLDB_EXPECT_TRUE(cldb_write_batch_approximate_size(batch) > value.size);

  recording_init(&handler, 0);
  CLDB_EXPECT_OK(cldb_write_batch_iterate(batch, &handler.base));
  CLDB_EXPECT_EQ_UINT(1, handler.count);
  expect_op(&handler, 0, true, 0, cldb_slice_make("big", 3), value_slice);
  recording_destroy(&handler);
  cldb_buf_free(&value);
  cldb_write_batch_destroy(batch);
}

CLDB_TEST(write_batch_invalid_inputs) {
  char short_content[CLDB_WRITE_BATCH_HEADER_SIZE - 1] = {0};
  cldb_slice_t invalid = {NULL, 1};
  cldb_write_batch_t *batch;
  cldb_write_batch_t *source;

  batch = cldb_write_batch_create();
  source = cldb_write_batch_create();
  CLDB_ASSERT_TRUE(batch != NULL);
  CLDB_ASSERT_TRUE(source != NULL);

  CLDB_EXPECT_FALSE(cldb_write_batch_put(NULL, cldb_slice_empty(),
                                        cldb_slice_empty()));
  CLDB_EXPECT_FALSE(cldb_write_batch_put(
      batch, invalid, cldb_slice_make("v", 1)));
  CLDB_EXPECT_FALSE(cldb_write_batch_put(
      batch, cldb_slice_make("k", 1), invalid));
  CLDB_EXPECT_FALSE(cldb_write_batch_delete(NULL, cldb_slice_empty()));
  CLDB_EXPECT_FALSE(cldb_write_batch_delete(batch, invalid));
  CLDB_EXPECT_FALSE(cldb_write_batch_append(NULL, source));
  CLDB_EXPECT_FALSE(cldb_write_batch_append(batch, NULL));
  CLDB_EXPECT_EQ_UINT(12, cldb_write_batch_byte_size(batch));
  CLDB_EXPECT_EQ_UINT(0, cldb_write_batch_count(batch));

  CLDB_ASSERT_TRUE(cldb_write_batch_set_contents(
      source, cldb_slice_make(short_content, sizeof(short_content))));
  CLDB_EXPECT_FALSE(cldb_write_batch_append(batch, source));
  CLDB_EXPECT_EQ_UINT(12, cldb_write_batch_byte_size(batch));
  CLDB_EXPECT_EQ_UINT(0, cldb_write_batch_count(batch));

  CLDB_EXPECT_FALSE(cldb_write_batch_set_contents(NULL, cldb_slice_empty()));
  CLDB_EXPECT_FALSE(cldb_write_batch_set_contents(batch, invalid));
  CLDB_EXPECT_FALSE(cldb_write_batch_set_sequence(NULL, 0));

#if SIZE_MAX > UINT32_MAX
  {
    cldb_slice_t huge = cldb_slice_make("x", (size_t)UINT32_MAX + 1);

    CLDB_EXPECT_FALSE(cldb_write_batch_put(
        batch, cldb_slice_make("k", 1), huge));
    CLDB_EXPECT_FALSE(cldb_write_batch_delete(batch, huge));
    CLDB_EXPECT_EQ_UINT(12, cldb_write_batch_byte_size(batch));
    CLDB_EXPECT_EQ_UINT(0, cldb_write_batch_count(batch));
  }
#endif

  cldb_write_batch_destroy(batch);
  cldb_write_batch_destroy(source);
}

CLDB_TEST(write_batch_count_overflow) {
  char header[CLDB_WRITE_BATCH_HEADER_SIZE] = {0};
  cldb_write_batch_t *batch;
  cldb_write_batch_t *source;
  size_t at_limit_size;

  cldb_encode_fixed32(header + 8, UINT32_MAX - 1);
  batch = cldb_write_batch_create();
  source = cldb_write_batch_create();
  CLDB_ASSERT_TRUE(batch != NULL);
  CLDB_ASSERT_TRUE(source != NULL);
  CLDB_ASSERT_TRUE(cldb_write_batch_set_contents(
      batch, cldb_slice_make(header, sizeof(header))));
  CLDB_ASSERT_TRUE(cldb_write_batch_put(
      source, cldb_slice_make("k", 1), cldb_slice_make("v", 1)));

  /* max-1 + 1 恰好达到上限；再追加或 Put 都应拒绝且不改内容。 */
  CLDB_ASSERT_TRUE(cldb_write_batch_append(batch, source));
  CLDB_EXPECT_EQ_UINT(UINT32_MAX, cldb_write_batch_count(batch));
  at_limit_size = cldb_write_batch_byte_size(batch);
  CLDB_EXPECT_FALSE(cldb_write_batch_append(batch, source));
  CLDB_EXPECT_FALSE(cldb_write_batch_put(
      batch, cldb_slice_make("x", 1), cldb_slice_make("y", 1)));
  CLDB_EXPECT_EQ_UINT(UINT32_MAX, cldb_write_batch_count(batch));
  CLDB_EXPECT_EQ_UINT(at_limit_size, cldb_write_batch_byte_size(batch));
  CLDB_EXPECT_EQ_UINT(1, cldb_write_batch_count(source));

  CLDB_ASSERT_TRUE(cldb_write_batch_clear(batch));
  CLDB_EXPECT_EQ_UINT(0, cldb_write_batch_count(batch));
  CLDB_EXPECT_EQ_UINT(12, cldb_write_batch_byte_size(batch));
  cldb_write_batch_destroy(batch);
  cldb_write_batch_destroy(source);
}

CLDB_TEST(write_batch_set_contents_ownership) {
  cldb_slice_t invalid = {NULL, 1};
  cldb_buf_t saved;
  cldb_buf_t copied;
  cldb_write_batch_t *source;
  cldb_write_batch_t *target;
  recording_handler_t handler;
  cldb_slice_t contents;
  size_t i;

  source = cldb_write_batch_create();
  target = cldb_write_batch_create();
  CLDB_ASSERT_TRUE(source != NULL);
  CLDB_ASSERT_TRUE(target != NULL);
  CLDB_ASSERT_TRUE(cldb_write_batch_set_sequence(source, 9));
  CLDB_ASSERT_TRUE(cldb_write_batch_put(
      source, cldb_slice_make("k", 1), cldb_slice_make("v", 1)));
  contents = cldb_write_batch_contents(source);

  cldb_buf_init(&saved);
  cldb_buf_init(&copied);
  CLDB_ASSERT_TRUE(cldb_buf_assign(&saved, contents.data, contents.size));
  CLDB_ASSERT_TRUE(cldb_buf_assign(&copied, contents.data, contents.size));
  CLDB_ASSERT_TRUE(cldb_write_batch_set_contents(target,
                                                   cldb_buf_slice(&copied)));
  CLDB_EXPECT_EQ_UINT(saved.size, cldb_write_batch_byte_size(target));

  /* target 必须持有副本：销毁 source 并破坏 copied 后内容仍不变。 */
  cldb_write_batch_destroy(source);
  for (i = 0; i < copied.size; ++i) {
    copied.data[i] = (char)0xaa;
  }
  contents = cldb_write_batch_contents(target);
  CLDB_ASSERT_EQ_UINT(saved.size, contents.size);
  CLDB_EXPECT_EQ_INT(0, memcmp(contents.data, saved.data, saved.size));

  /* 允许以自身内容覆盖自身，结果不变。 */
  CLDB_ASSERT_TRUE(
      cldb_write_batch_set_contents(target, cldb_write_batch_contents(target)));
  contents = cldb_write_batch_contents(target);
  CLDB_EXPECT_EQ_UINT(saved.size, contents.size);
  CLDB_EXPECT_EQ_INT(0, memcmp(contents.data, saved.data, saved.size));

  /* 空内容可接受，但遍历时会报 Corruption。 */
  CLDB_ASSERT_TRUE(
      cldb_write_batch_set_contents(target, cldb_slice_empty()));
  CLDB_EXPECT_EQ_UINT(0, cldb_write_batch_byte_size(target));
  CLDB_EXPECT_EQ_UINT(0, cldb_write_batch_count(target));
  CLDB_EXPECT_TRUE(cldb_slice_is_empty(cldb_write_batch_contents(target)));
  recording_init(&handler, 0);
  CLDB_EXPECT_STATUS(CLDB_STATUS_CORRUPTION,
                     cldb_write_batch_iterate(target, &handler.base));
  CLDB_EXPECT_EQ_UINT(0, handler.count);
  recording_destroy(&handler);

  CLDB_ASSERT_TRUE(cldb_write_batch_clear(target));
  CLDB_EXPECT_EQ_UINT(12, cldb_write_batch_byte_size(target));
  CLDB_EXPECT_FALSE(cldb_write_batch_set_contents(target, invalid));
  CLDB_EXPECT_EQ_UINT(12, cldb_write_batch_byte_size(target));

  cldb_buf_free(&saved);
  cldb_buf_free(&copied);
  cldb_write_batch_destroy(target);
}

CLDB_TEST(write_batch_iterate_malformed) {
  static const char kUnknownTag[] = {0x02};
  static const char kBadPut[] = {0x01, 0x03, 'f', 'o'};
  static const char kBadVarint[] = {
      0x01, (char)0x80, (char)0x80, (char)0x80, (char)0x80, (char)0x80,
  };
  char short_content[CLDB_WRITE_BATCH_HEADER_SIZE - 1] = {0};
  cldb_write_batch_t *batch;
  cldb_write_batch_t *valid;
  recording_handler_t handler;
  cldb_slice_t valid_contents;

  batch = cldb_write_batch_create();
  valid = cldb_write_batch_create();
  CLDB_ASSERT_TRUE(batch != NULL);
  CLDB_ASSERT_TRUE(valid != NULL);
  CLDB_ASSERT_TRUE(cldb_write_batch_set_sequence(valid, 200));
  CLDB_ASSERT_TRUE(cldb_write_batch_put(
      valid, cldb_slice_make("foo", 3), cldb_slice_make("bar", 3)));
  CLDB_ASSERT_TRUE(
      cldb_write_batch_delete(valid, cldb_slice_make("box", 3)));
  valid_contents = cldb_write_batch_contents(valid);
  recording_init(&handler, 200);

  CLDB_ASSERT_TRUE(cldb_write_batch_set_contents(
      batch, cldb_slice_make(short_content, sizeof(short_content))));
  CLDB_EXPECT_STATUS(CLDB_STATUS_CORRUPTION,
                     cldb_write_batch_iterate(batch, &handler.base));
  CLDB_EXPECT_EQ_UINT(0, handler.count);

  recording_reset(&handler, 200);
  set_raw_contents(batch, 1, kUnknownTag, sizeof(kUnknownTag));
  CLDB_EXPECT_STATUS(CLDB_STATUS_CORRUPTION,
                     cldb_write_batch_iterate(batch, &handler.base));
  CLDB_EXPECT_EQ_UINT(0, handler.count);

  recording_reset(&handler, 200);
  set_raw_contents(batch, 1, kBadPut, sizeof(kBadPut));
  CLDB_EXPECT_STATUS(CLDB_STATUS_CORRUPTION,
                     cldb_write_batch_iterate(batch, &handler.base));
  CLDB_EXPECT_EQ_UINT(0, handler.count);

  recording_reset(&handler, 200);
  set_raw_contents(batch, 1, kBadVarint, sizeof(kBadVarint));
  CLDB_EXPECT_STATUS(CLDB_STATUS_CORRUPTION,
                     cldb_write_batch_iterate(batch, &handler.base));
  CLDB_EXPECT_EQ_UINT(0, handler.count);

  /* 截掉 Delete 记录最后一个字节；Put 回调已执行后才报告损坏。 */
  recording_reset(&handler, 200);
  set_cloned_contents(batch, valid_contents, 1,
                      cldb_write_batch_count(valid));
  CLDB_EXPECT_STATUS(CLDB_STATUS_CORRUPTION,
                     cldb_write_batch_iterate(batch, &handler.base));
  CLDB_EXPECT_EQ_UINT(1, handler.count);
  expect_op(&handler, 0, true, 200, cldb_slice_make("foo", 3),
            cldb_slice_make("bar", 3));

  /* 头里 count 改为 3，但只有两条记录。 */
  recording_reset(&handler, 200);
  set_cloned_contents(batch, valid_contents, 0, 3);
  CLDB_EXPECT_STATUS(CLDB_STATUS_CORRUPTION,
                     cldb_write_batch_iterate(batch, &handler.base));
  CLDB_EXPECT_EQ_UINT(2, handler.count);
  expect_op(&handler, 0, true, 200, cldb_slice_make("foo", 3),
            cldb_slice_make("bar", 3));
  expect_op(&handler, 1, false, 201, cldb_slice_make("box", 3),
            cldb_slice_empty());

  recording_destroy(&handler);
  cldb_write_batch_destroy(batch);
  cldb_write_batch_destroy(valid);
}

CLDB_TEST(write_batch_iterate_handler_errors) {
  static const cldb_write_batch_handler_vtbl_t kMissingPutVtbl = {
      .put = NULL,
      .delete = recording_delete,
  };
  cldb_write_batch_t *batch;
  recording_handler_t handler;
  cldb_write_batch_handler_t invalid_handler;
  cldb_status_t status;

  batch = cldb_write_batch_create();
  CLDB_ASSERT_TRUE(batch != NULL);
  CLDB_ASSERT_TRUE(cldb_write_batch_put(
      batch, cldb_slice_make("foo", 3), cldb_slice_make("bar", 3)));
  CLDB_ASSERT_TRUE(
      cldb_write_batch_delete(batch, cldb_slice_make("box", 3)));

  recording_init(&handler, 5);
  handler.fail_at = 0;
  status = cldb_write_batch_iterate(batch, &handler.base);
  CLDB_EXPECT_EQ_INT(CLDB_STATUS_IO_ERROR, cldb_status_code(status));
  CLDB_EXPECT_EQ_UINT(0, handler.count);
  CLDB_EXPECT_EQ_UINT(5, handler.next_sequence);

  recording_reset(&handler, 5);
  handler.fail_at = 1;
  status = cldb_write_batch_iterate(batch, &handler.base);
  CLDB_EXPECT_EQ_INT(CLDB_STATUS_IO_ERROR, cldb_status_code(status));
  CLDB_EXPECT_EQ_UINT(1, handler.count);
  CLDB_EXPECT_EQ_UINT(6, handler.next_sequence);
  expect_op(&handler, 0, true, 5, cldb_slice_make("foo", 3),
            cldb_slice_make("bar", 3));

  recording_reset(&handler, 5);
  CLDB_EXPECT_OK(cldb_write_batch_iterate(batch, &handler.base));
  CLDB_EXPECT_EQ_UINT(2, handler.count);
  expect_op(&handler, 0, true, 5, cldb_slice_make("foo", 3),
            cldb_slice_make("bar", 3));
  expect_op(&handler, 1, false, 6, cldb_slice_make("box", 3),
            cldb_slice_empty());
  recording_destroy(&handler);

  invalid_handler.vtbl = NULL;
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_write_batch_iterate(batch, &invalid_handler));
  invalid_handler.vtbl = &kMissingPutVtbl;
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_write_batch_iterate(batch, &invalid_handler));
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_write_batch_iterate(batch, NULL));
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_write_batch_iterate(NULL, &invalid_handler));
  cldb_write_batch_destroy(batch);
}

CLDB_TEST(write_batch_sequence_accessor) {
  char short_content[CLDB_WRITE_BATCH_HEADER_SIZE - 1] = {0};
  cldb_write_batch_t *batch;

  batch = cldb_write_batch_create();
  CLDB_ASSERT_TRUE(batch != NULL);
  CLDB_EXPECT_EQ_UINT(0, cldb_write_batch_sequence(batch));
  CLDB_ASSERT_TRUE(
      cldb_write_batch_set_sequence(batch, CLDB_MAX_SEQUENCE_NUMBER));
  CLDB_EXPECT_EQ_UINT(CLDB_MAX_SEQUENCE_NUMBER,
                      cldb_write_batch_sequence(batch));
  CLDB_EXPECT_FALSE(cldb_write_batch_set_sequence(
      batch, CLDB_MAX_SEQUENCE_NUMBER + 1));
  CLDB_EXPECT_EQ_UINT(CLDB_MAX_SEQUENCE_NUMBER,
                      cldb_write_batch_sequence(batch));

  CLDB_ASSERT_TRUE(cldb_write_batch_put(
      batch, cldb_slice_make("k", 1), cldb_slice_make("v", 1)));
  CLDB_EXPECT_EQ_UINT(1, cldb_write_batch_count(batch));
  CLDB_ASSERT_TRUE(cldb_write_batch_set_sequence(batch, 7));
  CLDB_EXPECT_EQ_UINT(7, cldb_write_batch_sequence(batch));
  CLDB_EXPECT_EQ_UINT(1, cldb_write_batch_count(batch));

  CLDB_ASSERT_TRUE(cldb_write_batch_set_contents(
      batch, cldb_slice_make(short_content, sizeof(short_content))));
  CLDB_EXPECT_FALSE(cldb_write_batch_set_sequence(batch, 1));
  CLDB_EXPECT_EQ_UINT(0, cldb_write_batch_sequence(batch));

  CLDB_ASSERT_TRUE(cldb_write_batch_clear(batch));
  CLDB_EXPECT_EQ_UINT(0, cldb_write_batch_sequence(batch));
  cldb_write_batch_destroy(batch);
}

void cldb_register_write_batch_tests(void) {
  CLDB_RUN_TEST(write_batch_empty_and_accessors);
  CLDB_RUN_TEST(write_batch_encode_golden);
  CLDB_RUN_TEST(write_batch_empty_key_value);
  CLDB_RUN_TEST(write_batch_append_and_sequence);
  CLDB_RUN_TEST(write_batch_append_self);
  CLDB_RUN_TEST(write_batch_large_value);
  CLDB_RUN_TEST(write_batch_invalid_inputs);
  CLDB_RUN_TEST(write_batch_count_overflow);
  CLDB_RUN_TEST(write_batch_set_contents_ownership);
  CLDB_RUN_TEST(write_batch_iterate_malformed);
  CLDB_RUN_TEST(write_batch_iterate_handler_errors);
  CLDB_RUN_TEST(write_batch_sequence_accessor);
}
