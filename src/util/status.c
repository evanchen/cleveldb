#include "cleveldb/status.h"

#include <stdio.h>
#include <string.h>

/* 拼接 "context: detail"，空部分省略；对应上游 Status 构造函数
 * 在状态串后追加 msg、": "、msg2 的规则。 */
static void format_message(cldb_status_t *status, const char *context,
                           const char *detail) {
  if (context == NULL) {
    context = "";
  }
  if (detail == NULL) {
    detail = "";
  }
  if (context[0] == '\0') {
    (void)snprintf(status->message, sizeof(status->message), "%s", detail);
  } else if (detail[0] == '\0') {
    (void)snprintf(status->message, sizeof(status->message), "%s", context);
  } else {
    (void)snprintf(status->message, sizeof(status->message), "%s: %s", context,
                   detail);
  }
}

cldb_status_t cldb_status_ok(void) {
  cldb_status_t status = {0};

  return status;
}

cldb_status_t cldb_status_set(cldb_status_code_t code, const char *context,
                              const char *detail) {
  cldb_status_t status = {0};

  status.code = code;
  if (code != CLDB_STATUS_OK) {
    format_message(&status, context, detail);
  }
  return status;
}

cldb_status_t cldb_status_not_found(const char *context, const char *detail) {
  return cldb_status_set(CLDB_STATUS_NOT_FOUND, context, detail);
}

cldb_status_t cldb_status_corruption(const char *context, const char *detail) {
  return cldb_status_set(CLDB_STATUS_CORRUPTION, context, detail);
}

cldb_status_t cldb_status_not_supported(const char *context,
                                        const char *detail) {
  return cldb_status_set(CLDB_STATUS_NOT_SUPPORTED, context, detail);
}

cldb_status_t cldb_status_invalid_argument(const char *context,
                                           const char *detail) {
  return cldb_status_set(CLDB_STATUS_INVALID_ARGUMENT, context, detail);
}

cldb_status_t cldb_status_io(const char *context, const char *detail) {
  return cldb_status_set(CLDB_STATUS_IO_ERROR, context, detail);
}

size_t cldb_status_to_string(cldb_status_t status, char *buf, size_t size) {
  char unknown[32];
  const char *prefix;
  size_t prefix_size;
  size_t message_size = 0;
  size_t needed;
  size_t written;
  size_t limit;
  size_t copy_size;

  switch (status.code) {
    case CLDB_STATUS_OK:
      prefix = "OK";
      break;
    case CLDB_STATUS_NOT_FOUND:
      prefix = "NotFound: ";
      break;
    case CLDB_STATUS_CORRUPTION:
      prefix = "Corruption: ";
      break;
    case CLDB_STATUS_NOT_SUPPORTED:
      prefix = "Not implemented: ";
      break;
    case CLDB_STATUS_INVALID_ARGUMENT:
      prefix = "Invalid argument: ";
      break;
    case CLDB_STATUS_IO_ERROR:
      prefix = "IO error: ";
      break;
    default:
      (void)snprintf(unknown, sizeof(unknown), "Unknown code(%d): ",
                     (int)status.code);
      prefix = unknown;
      break;
  }

  if (status.code != CLDB_STATUS_OK) {
    while (message_size < sizeof(status.message) &&
           status.message[message_size] != '\0') {
      ++message_size;
    }
  }
  prefix_size = strlen(prefix);
  needed = prefix_size + message_size;

  if (buf != NULL && size > 0) {
    limit = size - 1;
    copy_size = prefix_size < limit ? prefix_size : limit;
    memcpy(buf, prefix, copy_size);
    written = copy_size;
    if (prefix_size <= limit) {
      size_t remaining = limit - written;
      size_t message_copy = message_size < remaining ? message_size : remaining;

      memcpy(buf + written, status.message, message_copy);
      written += message_copy;
    }
    buf[written] = '\0';
  }
  return needed;
}
