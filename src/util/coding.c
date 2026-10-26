#include "util/coding.h"

#include <stdint.h>

bool cldb_put_fixed32(cldb_buf_t *dst, uint32_t value) {
  char buffer[4];

  cldb_encode_fixed32(buffer, value);
  return cldb_buf_append(dst, buffer, sizeof(buffer));
}

bool cldb_put_fixed64(cldb_buf_t *dst, uint64_t value) {
  char buffer[8];

  cldb_encode_fixed64(buffer, value);
  return cldb_buf_append(dst, buffer, sizeof(buffer));
}

char *cldb_encode_varint32(char *dst, uint32_t value) {
  uint8_t *ptr = (uint8_t *)dst;

  while (value >= 128) {
    *ptr++ = (uint8_t)((value & 127) | 128);
    value >>= 7;
  }
  *ptr++ = (uint8_t)value;
  return (char *)ptr;
}

char *cldb_encode_varint64(char *dst, uint64_t value) {
  uint8_t *ptr = (uint8_t *)dst;

  while (value >= 128) {
    *ptr++ = (uint8_t)((value & 127) | 128);
    value >>= 7;
  }
  *ptr++ = (uint8_t)value;
  return (char *)ptr;
}

bool cldb_put_varint32(cldb_buf_t *dst, uint32_t value) {
  char buffer[5];
  char *end = cldb_encode_varint32(buffer, value);

  return cldb_buf_append(dst, buffer, (size_t)(end - buffer));
}

bool cldb_put_varint64(cldb_buf_t *dst, uint64_t value) {
  char buffer[10];
  char *end = cldb_encode_varint64(buffer, value);

  return cldb_buf_append(dst, buffer, (size_t)(end - buffer));
}

const char *cldb_get_varint32_ptr_fallback(const char *p, const char *limit,
                                           uint32_t *value) {
  uint32_t result = 0;
  uint32_t shift;

  if (p == NULL || limit == NULL || value == NULL) {
    return NULL;
  }
  for (shift = 0; shift <= 28 && p < limit; shift += 7) {
    uint32_t byte = (uint8_t)*p++;

    if ((byte & 128) != 0) {
      result |= (byte & 127) << shift;
    } else {
      result |= byte << shift;
      *value = result;
      return p;
    }
  }
  return NULL;
}

const char *cldb_get_varint64_ptr(const char *p, const char *limit,
                                  uint64_t *value) {
  uint64_t result = 0;
  uint32_t shift;

  if (p == NULL || limit == NULL || value == NULL) {
    return NULL;
  }
  for (shift = 0; shift <= 63 && p < limit; shift += 7) {
    uint64_t byte = (uint8_t)*p++;

    if ((byte & 128) != 0) {
      result |= (byte & 127) << shift;
    } else {
      result |= byte << shift;
      *value = result;
      return p;
    }
  }
  return NULL;
}

bool cldb_get_varint32(cldb_slice_t *input, uint32_t *value) {
  const char *p;
  const char *limit;
  const char *end;

  if (input == NULL || value == NULL || input->data == NULL) {
    return false;
  }
  p = input->data;
  limit = p + input->size;
  end = cldb_get_varint32_ptr(p, limit, value);
  if (end == NULL) {
    return false;
  }
  *input = cldb_slice_make(end, (size_t)(limit - end));
  return true;
}

bool cldb_get_varint64(cldb_slice_t *input, uint64_t *value) {
  const char *p;
  const char *limit;
  const char *end;

  if (input == NULL || value == NULL || input->data == NULL) {
    return false;
  }
  p = input->data;
  limit = p + input->size;
  end = cldb_get_varint64_ptr(p, limit, value);
  if (end == NULL) {
    return false;
  }
  *input = cldb_slice_make(end, (size_t)(limit - end));
  return true;
}

bool cldb_put_length_prefixed_slice(cldb_buf_t *dst, cldb_slice_t value) {
  size_t prefix_size;
  size_t total;

  if (dst == NULL || (value.size > 0 && value.data == NULL)) {
    return false;
  }
#if SIZE_MAX > UINT32_MAX
  if (value.size > (size_t)UINT32_MAX) {
    return false;
  }
#endif
  prefix_size = cldb_varint_length((uint64_t)value.size);
  if (value.size > SIZE_MAX - prefix_size) {
    return false;
  }
  total = prefix_size + value.size;
  if (total >= SIZE_MAX || dst->size > SIZE_MAX - total - 1) {
    return false;
  }
  /* 先预留前缀和内容的全部容量，后续追加不会再分配，失败不留下半条记录。 */
  if (!cldb_buf_reserve(dst, dst->size + total + 1)) {
    return false;
  }
  if (!cldb_put_varint32(dst, (uint32_t)value.size)) {
    return false;
  }
  return cldb_buf_append(dst, value.data, value.size);
}

bool cldb_get_length_prefixed_slice(cldb_slice_t *input,
                                    cldb_slice_t *result) {
  uint32_t length;

  if (input == NULL || result == NULL) {
    return false;
  }
  if (!cldb_get_varint32(input, &length)) {
    return false;
  }
  if (input->size < (size_t)length) {
    return false;
  }
  *result = cldb_slice_make(input->data, (size_t)length);
  cldb_slice_remove_prefix(input, (size_t)length);
  return true;
}

size_t cldb_varint_length(uint64_t value) {
  size_t length = 1;

  while (value >= 128) {
    value >>= 7;
    ++length;
  }
  return length;
}
