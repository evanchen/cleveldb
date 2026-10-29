#include "util/logging.h"

#include <stdio.h>
#include <string.h>

bool cldb_append_number_to(cldb_buf_t *dst, uint64_t num) {
  char buffer[32];
  int length;

  if (dst == NULL) {
    return false;
  }
  length = snprintf(buffer, sizeof(buffer), "%llu",
                    (unsigned long long)num);
  if (length < 0 || (size_t)length >= sizeof(buffer)) {
    return false;
  }
  return cldb_buf_append(dst, buffer, (size_t)length);
}

bool cldb_append_escaped_string_to(cldb_buf_t *dst, cldb_slice_t value) {
  size_t i;

  if (dst == NULL || (value.size > 0 && value.data == NULL)) {
    return false;
  }
  for (i = 0; i < value.size; ++i) {
    unsigned char ch = (unsigned char)value.data[i];

    if (ch >= (unsigned char)' ' && ch <= (unsigned char)'~') {
      if (!cldb_buf_append_char(dst, (char)ch)) {
        return false;
      }
    } else {
      char escaped[5];
      int length = snprintf(escaped, sizeof(escaped), "\\x%02x",
                            (unsigned int)ch);

      if (length < 0 || (size_t)length >= sizeof(escaped) ||
          !cldb_buf_append(dst, escaped, (size_t)length)) {
        return false;
      }
    }
  }
  return true;
}

bool cldb_number_to_string(cldb_buf_t *dst, uint64_t num) {
  if (dst == NULL) {
    return false;
  }
  cldb_buf_clear(dst);
  return cldb_append_number_to(dst, num);
}

bool cldb_escape_string(cldb_buf_t *dst, cldb_slice_t value) {
  if (dst == NULL || (value.size > 0 && value.data == NULL)) {
    return false;
  }
  cldb_buf_clear(dst);
  return cldb_append_escaped_string_to(dst, value);
}

bool cldb_consume_decimal_number(cldb_slice_t *in, uint64_t *value) {
  const uint64_t kMaxDiv10 = UINT64_MAX / 10;
  const uint8_t kMaxLastDigit = (uint8_t)(UINT64_MAX % 10);
  uint64_t result = 0;
  size_t consumed = 0;

  if (in == NULL || value == NULL || in->data == NULL || in->size == 0) {
    return false;
  }
  while (consumed < in->size) {
    uint8_t ch = (uint8_t)in->data[consumed];
    uint64_t digit;

    if (ch < (uint8_t)'0' || ch > (uint8_t)'9') {
      break;
    }
    digit = (uint64_t)(ch - (uint8_t)'0');
    if (result > kMaxDiv10 ||
        (result == kMaxDiv10 && digit > (uint64_t)kMaxLastDigit)) {
      return false;
    }
    result = result * 10 + digit;
    ++consumed;
  }
  if (consumed == 0) {
    return false;
  }
  *value = result;
  cldb_slice_remove_prefix(in, consumed);
  return true;
}
