#include "db/write_batch_internal.h"

#include <stdint.h>
#include <stdlib.h>

#include "db/dbformat.h"
#include "db/memtable.h"
#include "util/coding.h"

/* WriteBatch 编码、合并与遍历；对应上游 db/write_batch.cc。
 * Handler 回调改为返回状态：本实现的分配可能失败，遍历需要及时终止并
 * 把原因交给调用者；上游该接口无返回值。 */

/* 从 [*pos, limit) 读 varint32 长度前缀；失败不移动 *pos。 */
static bool read_length_prefixed(const char **pos, const char *limit,
                                 cldb_slice_t *result) {
  uint32_t length;
  const char *p;

  if (pos == NULL || *pos == NULL || limit == NULL || result == NULL ||
      *pos > limit) {
    return false;
  }
  p = cldb_get_varint32_ptr(*pos, limit, &length);
  if (p == NULL || (size_t)(limit - p) < (size_t)length) {
    return false;
  }
  *result = cldb_slice_make(p, (size_t)length);
  *pos = p + (size_t)length;
  return true;
}

/* 追加一条记录并在成功后 count++；失败把 rep 回滚到原长度。 */
static bool append_record(cldb_write_batch_t *batch, cldb_value_type_t type,
                          cldb_slice_t key, cldb_slice_t value) {
  uint32_t count;
  size_t old_size;

  if (batch == NULL || batch->rep.data == NULL ||
      batch->rep.size < (size_t)CLDB_WRITE_BATCH_HEADER_SIZE ||
      (type != CLDB_VALUE_TYPE_VALUE && type != CLDB_VALUE_TYPE_DELETION) ||
      !cldb_slice_is_valid(key) ||
      (type == CLDB_VALUE_TYPE_VALUE && !cldb_slice_is_valid(value)) ||
      key.size > (size_t)UINT32_MAX ||
      (type == CLDB_VALUE_TYPE_VALUE && value.size > (size_t)UINT32_MAX)) {
    return false;
  }
  count = cldb_write_batch_count(batch);
  if (count == UINT32_MAX) {
    return false;
  }

  old_size = batch->rep.size;
  if (!cldb_buf_append_char(&batch->rep,
                            (char)(unsigned int)type)) {
    goto rollback;
  }
  if (!cldb_put_length_prefixed_slice(&batch->rep, key)) {
    goto rollback;
  }
  if (type == CLDB_VALUE_TYPE_VALUE &&
      !cldb_put_length_prefixed_slice(&batch->rep, value)) {
    goto rollback;
  }
  cldb_encode_fixed32(batch->rep.data + 8, count + 1);
  return true;

rollback:
  /* 缩短长度不需要分配，必定成功。 */
  (void)cldb_buf_resize(&batch->rep, old_size);
  return false;
}

bool cldb_write_batch_init(cldb_write_batch_t *batch) {
  if (batch == NULL) {
    return false;
  }
  cldb_buf_init(&batch->rep);
  if (!cldb_buf_resize(&batch->rep,
                       (size_t)CLDB_WRITE_BATCH_HEADER_SIZE)) {
    cldb_buf_free(&batch->rep);
    return false;
  }
  return cldb_write_batch_clear(batch);
}

void cldb_write_batch_free(cldb_write_batch_t *batch) {
  if (batch == NULL) {
    return;
  }
  cldb_buf_free(&batch->rep);
}

cldb_write_batch_t *cldb_write_batch_create(void) {
  cldb_write_batch_t *batch = malloc(sizeof(*batch));

  if (batch == NULL) {
    return NULL;
  }
  if (!cldb_write_batch_init(batch)) {
    free(batch);
    return NULL;
  }
  return batch;
}

void cldb_write_batch_destroy(cldb_write_batch_t *batch) {
  if (batch == NULL) {
    return;
  }
  cldb_write_batch_free(batch);
  free(batch);
}

bool cldb_write_batch_clear(cldb_write_batch_t *batch) {
  if (batch == NULL ||
      !cldb_buf_resize(&batch->rep,
                       (size_t)CLDB_WRITE_BATCH_HEADER_SIZE) ||
      batch->rep.data == NULL) {
    return false;
  }
  cldb_encode_fixed64(batch->rep.data, 0);
  cldb_encode_fixed32(batch->rep.data + 8, 0);
  return true;
}

bool cldb_write_batch_put(cldb_write_batch_t *batch, cldb_slice_t key,
                          cldb_slice_t value) {
  return append_record(batch, CLDB_VALUE_TYPE_VALUE, key, value);
}

bool cldb_write_batch_delete(cldb_write_batch_t *batch, cldb_slice_t key) {
  return append_record(batch, CLDB_VALUE_TYPE_DELETION, key,
                       cldb_slice_empty());
}

bool cldb_write_batch_append(cldb_write_batch_t *batch,
                             const cldb_write_batch_t *source) {
  uint32_t batch_count;
  uint32_t source_count;
  size_t payload;

  if (batch == NULL || source == NULL || batch->rep.data == NULL ||
      source->rep.data == NULL ||
      batch->rep.size < (size_t)CLDB_WRITE_BATCH_HEADER_SIZE ||
      source->rep.size < (size_t)CLDB_WRITE_BATCH_HEADER_SIZE) {
    return false;
  }
  batch_count = cldb_write_batch_count(batch);
  source_count = cldb_write_batch_count(source);
  if (source_count > UINT32_MAX - batch_count) {
    return false;
  }
  payload = source->rep.size - (size_t)CLDB_WRITE_BATCH_HEADER_SIZE;
  if (payload > 0 &&
      !cldb_buf_append(&batch->rep,
                       source->rep.data + CLDB_WRITE_BATCH_HEADER_SIZE,
                       payload)) {
    return false;
  }
  cldb_encode_fixed32(batch->rep.data + 8, batch_count + source_count);
  return true;
}

uint64_t cldb_write_batch_sequence(const cldb_write_batch_t *batch) {
  if (batch == NULL || batch->rep.data == NULL ||
      batch->rep.size < (size_t)CLDB_WRITE_BATCH_HEADER_SIZE) {
    return 0;
  }
  return cldb_decode_fixed64(batch->rep.data);
}

uint32_t cldb_write_batch_count(const cldb_write_batch_t *batch) {
  if (batch == NULL || batch->rep.data == NULL ||
      batch->rep.size < (size_t)CLDB_WRITE_BATCH_HEADER_SIZE) {
    return 0;
  }
  return cldb_decode_fixed32(batch->rep.data + 8);
}

bool cldb_write_batch_set_sequence(cldb_write_batch_t *batch,
                                   uint64_t sequence) {
  if (batch == NULL || batch->rep.data == NULL ||
      batch->rep.size < (size_t)CLDB_WRITE_BATCH_HEADER_SIZE ||
      sequence > CLDB_MAX_SEQUENCE_NUMBER) {
    return false;
  }
  cldb_encode_fixed64(batch->rep.data, sequence);
  return true;
}

size_t cldb_write_batch_byte_size(const cldb_write_batch_t *batch) {
  return batch != NULL ? batch->rep.size : 0;
}

size_t cldb_write_batch_approximate_size(const cldb_write_batch_t *batch) {
  return cldb_write_batch_byte_size(batch);
}

cldb_slice_t cldb_write_batch_contents(const cldb_write_batch_t *batch) {
  return batch != NULL ? cldb_buf_slice(&batch->rep)
                       : cldb_slice_empty();
}

bool cldb_write_batch_set_contents(cldb_write_batch_t *batch,
                                   cldb_slice_t contents) {
  if (batch == NULL || !cldb_slice_is_valid(contents)) {
    return false;
  }
  return cldb_buf_assign(&batch->rep, contents.data, contents.size);
}

cldb_status_t cldb_write_batch_iterate(
    const cldb_write_batch_t *batch, cldb_write_batch_handler_t *handler) {
  const char *pos;
  const char *limit;
  uint64_t found = 0;

  if (batch == NULL || handler == NULL || handler->vtbl == NULL ||
      handler->vtbl->put == NULL || handler->vtbl->delete == NULL) {
    return cldb_status_invalid_argument("WriteBatch::Iterate",
                                        "invalid argument");
  }
  if (batch->rep.data == NULL ||
      batch->rep.size < (size_t)CLDB_WRITE_BATCH_HEADER_SIZE) {
    return cldb_status_corruption("WriteBatch::Iterate",
                                  "malformed WriteBatch (too small)");
  }

  pos = batch->rep.data + CLDB_WRITE_BATCH_HEADER_SIZE;
  limit = batch->rep.data + batch->rep.size;
  while (pos < limit) {
    cldb_slice_t key;
    cldb_slice_t value;
    cldb_status_t status;
    unsigned int tag = (unsigned int)(unsigned char)*pos;

    ++pos;
    ++found;
    if (tag == (unsigned int)CLDB_VALUE_TYPE_VALUE) {
      if (!read_length_prefixed(&pos, limit, &key) ||
          !read_length_prefixed(&pos, limit, &value)) {
        return cldb_status_corruption("WriteBatch::Iterate",
                                      "bad WriteBatch Put");
      }
      status = handler->vtbl->put(handler, key, value);
    } else if (tag == (unsigned int)CLDB_VALUE_TYPE_DELETION) {
      if (!read_length_prefixed(&pos, limit, &key)) {
        return cldb_status_corruption("WriteBatch::Iterate",
                                      "bad WriteBatch Delete");
      }
      status = handler->vtbl->delete(handler, key);
    } else {
      return cldb_status_corruption("WriteBatch::Iterate",
                                    "unknown WriteBatch tag");
    }
    if (!cldb_status_is_ok(status)) {
      return status;
    }
  }

  if (found != (uint64_t)cldb_write_batch_count(batch)) {
    return cldb_status_corruption("WriteBatch::Iterate",
                                  "WriteBatch has wrong count");
  }
  return cldb_status_ok();
}

/* WriteBatch handler；base 必须为首成员。对应上游 MemTableInserter。 */
typedef struct cldb_memtable_inserter {
  cldb_write_batch_handler_t base; /* 必须为首成员 */
  cldb_memtable_t *memtable;       /* 仅引用，遍历期间必须存活 */
  cldb_sequence_number_t sequence; /* 当前记录的序列号 */
} cldb_memtable_inserter_t;

static cldb_memtable_inserter_t *memtable_inserter_self(
    cldb_write_batch_handler_t *handler) {
  return (cldb_memtable_inserter_t *)(void *)handler;
}

/* 写入一条记录；Add 成功后才消费序列号，失败时保留给调用者诊断。 */
static cldb_status_t memtable_inserter_add(
    cldb_write_batch_handler_t *handler, cldb_value_type_t type,
    cldb_slice_t key, cldb_slice_t value) {
  cldb_memtable_inserter_t *self = memtable_inserter_self(handler);
  cldb_status_t status = cldb_memtable_add(
      self->memtable, self->sequence, type, key, value);

  if (!cldb_status_is_ok(status)) {
    return status;
  }
  ++self->sequence;
  return cldb_status_ok();
}

static cldb_status_t memtable_inserter_put(
    cldb_write_batch_handler_t *handler, cldb_slice_t key,
    cldb_slice_t value) {
  return memtable_inserter_add(handler, CLDB_VALUE_TYPE_VALUE, key, value);
}

static cldb_status_t memtable_inserter_delete(
    cldb_write_batch_handler_t *handler, cldb_slice_t key) {
  return memtable_inserter_add(handler, CLDB_VALUE_TYPE_DELETION, key,
                               cldb_slice_empty());
}

static const cldb_write_batch_handler_vtbl_t kMemTableInserterVtbl = {
    .put = memtable_inserter_put,
    .delete = memtable_inserter_delete,
};

cldb_status_t cldb_write_batch_insert_into(const cldb_write_batch_t *batch,
                                           struct cldb_memtable *memtable) {
  cldb_memtable_inserter_t inserter;

  if (memtable == NULL) {
    return cldb_status_invalid_argument("WriteBatch::InsertInto",
                                        "memtable is NULL");
  }
  inserter.base.vtbl = &kMemTableInserterVtbl;
  inserter.memtable = memtable;
  inserter.sequence = cldb_write_batch_sequence(batch);
  return cldb_write_batch_iterate(batch, &inserter.base);
}
