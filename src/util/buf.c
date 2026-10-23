#include "util/buf.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* 源指针相对本缓冲分配区的位置。 */
enum {
  CLDB_BUF_SOURCE_EXTERNAL = 0,
  CLDB_BUF_SOURCE_INTERNAL = 1,
  CLDB_BUF_SOURCE_INVALID = -1
};

/* 判断源是否指向本缓冲的分配区，并记录偏移。 */
static int source_kind(const cldb_buf_t *buf, const void *data, size_t size,
                       size_t *offset) {
  uintptr_t base;
  uintptr_t source;

  if (buf->data == NULL) {
    return CLDB_BUF_SOURCE_EXTERNAL;
  }
  base = (uintptr_t)buf->data;
  source = (uintptr_t)data;
  if (source < base || source - base >= buf->capacity) {
    return CLDB_BUF_SOURCE_EXTERNAL;
  }
  *offset = (size_t)(source - base);
  if (size > buf->capacity - *offset) {
    return CLDB_BUF_SOURCE_INVALID;
  }
  return CLDB_BUF_SOURCE_INTERNAL;
}

/* 保证能容纳 payload 字节内容（另有 1 字节 NUL）。 */
static bool ensure_payload(cldb_buf_t *buf, size_t payload) {
  size_t need;
  size_t capacity;

  if (buf == NULL) {
    return false;
  }
  if (payload == 0 || payload < buf->capacity) {
    return true;
  }
  if (payload == SIZE_MAX) {
    return false;
  }
  need = payload + 1;
  capacity = buf->capacity == 0 ? 16 : buf->capacity;
  while (capacity < need) {
    if (capacity > SIZE_MAX / 2) {
      capacity = need;
      break;
    }
    capacity *= 2;
  }
  return cldb_buf_reserve(buf, capacity);
}

void cldb_buf_init(cldb_buf_t *buf) {
  if (buf == NULL) {
    return;
  }
  buf->data = NULL;
  buf->size = 0;
  buf->capacity = 0;
}

void cldb_buf_free(cldb_buf_t *buf) {
  if (buf == NULL) {
    return;
  }
  free(buf->data);
  cldb_buf_init(buf);
}

void cldb_buf_clear(cldb_buf_t *buf) {
  if (buf == NULL) {
    return;
  }
  buf->size = 0;
  if (buf->data != NULL) {
    buf->data[0] = '\0';
  }
}

bool cldb_buf_reserve(cldb_buf_t *buf, size_t requested) {
  char *new_data;

  if (buf == NULL) {
    return false;
  }
  if (requested <= buf->capacity) {
    return true;
  }
  if (buf->size >= requested) {
    return false;
  }
  new_data = realloc(buf->data, requested);
  if (new_data == NULL) {
    return false;
  }
  buf->data = new_data;
  buf->capacity = requested;
  buf->data[buf->size] = '\0';
  return true;
}

bool cldb_buf_resize(cldb_buf_t *buf, size_t size) {
  size_t old_size;

  if (buf == NULL || size == SIZE_MAX) {
    return false;
  }
  old_size = buf->size;
  if (size > old_size) {
    if (!ensure_payload(buf, size)) {
      return false;
    }
    memset(buf->data + old_size, 0, size - old_size);
  }
  buf->size = size;
  if (buf->data != NULL) {
    buf->data[size] = '\0';
  }
  return true;
}

bool cldb_buf_append(cldb_buf_t *buf, const void *data, size_t size) {
  size_t offset = 0;
  size_t payload;
  int kind;

  if (buf == NULL) {
    return false;
  }
  if (size == 0) {
    return true;
  }
  if (data == NULL || size > SIZE_MAX - buf->size) {
    return false;
  }
  payload = buf->size + size;
  kind = source_kind(buf, data, size, &offset);
  if (kind == CLDB_BUF_SOURCE_INVALID) {
    return false;
  }
  if (!ensure_payload(buf, payload)) {
    return false;
  }
  if (kind == CLDB_BUF_SOURCE_INTERNAL) {
    data = buf->data + offset;
  }
  memmove(buf->data + buf->size, data, size);
  buf->size = payload;
  buf->data[buf->size] = '\0';
  return true;
}

bool cldb_buf_append_char(cldb_buf_t *buf, char ch) {
  if (buf == NULL || buf->size == SIZE_MAX) {
    return false;
  }
  if (!ensure_payload(buf, buf->size + 1)) {
    return false;
  }
  buf->data[buf->size] = ch;
  buf->size++;
  buf->data[buf->size] = '\0';
  return true;
}

bool cldb_buf_append_slice(cldb_buf_t *buf, cldb_slice_t slice) {
  return cldb_buf_append(buf, slice.data, slice.size);
}

bool cldb_buf_assign(cldb_buf_t *buf, const void *data, size_t size) {
  size_t offset = 0;
  int kind;

  if (buf == NULL) {
    return false;
  }
  if (size == 0) {
    cldb_buf_clear(buf);
    return true;
  }
  if (data == NULL) {
    return false;
  }
  kind = source_kind(buf, data, size, &offset);
  if (kind == CLDB_BUF_SOURCE_INVALID) {
    return false;
  }
  if (!ensure_payload(buf, size)) {
    return false;
  }
  if (kind == CLDB_BUF_SOURCE_INTERNAL) {
    data = buf->data + offset;
  }
  memmove(buf->data, data, size);
  buf->size = size;
  buf->data[buf->size] = '\0';
  return true;
}

void cldb_buf_swap(cldb_buf_t *a, cldb_buf_t *b) {
  cldb_buf_t tmp;

  if (a == NULL || b == NULL || a == b) {
    return;
  }
  tmp = *a;
  *a = *b;
  *b = tmp;
}

cldb_slice_t cldb_buf_slice(const cldb_buf_t *buf) {
  if (buf == NULL) {
    return cldb_slice_empty();
  }
  return cldb_slice_make(buf->data, buf->size);
}

const char *cldb_buf_cstr(const cldb_buf_t *buf) {
  return buf != NULL && buf->data != NULL ? buf->data : "";
}
