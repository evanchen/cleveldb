#ifndef CLDB_INCLUDE_CLEVELDB_STATUS_H_
#define CLDB_INCLUDE_CLEVELDB_STATUS_H_

#include <stdbool.h>
#include <stddef.h>

/* 错误码；数值与上游 Status::Code 一致，是稳定契约。 */
typedef enum cldb_status_code {
  CLDB_STATUS_OK = 0,
  CLDB_STATUS_NOT_FOUND = 1,
  CLDB_STATUS_CORRUPTION = 2,
  CLDB_STATUS_NOT_SUPPORTED = 3,
  CLDB_STATUS_INVALID_ARGUMENT = 4,
  CLDB_STATUS_IO_ERROR = 5
} cldb_status_code_t;

/* 操作状态；对应上游 Status，但用内联消息替代其堆分配的 state_。
 * 可按值拷贝并独立使用，不需要 destroy；code 稳定，message 仅用于诊断。 */
typedef struct cldb_status {
  cldb_status_code_t code; /* 对应上游 kOk..kIOError；可参与分支和测试 */
  char message[256];       /* 对应 state_ 消息；owned，NUL 结尾，超长截断 */
} cldb_status_t;

/* 成功状态；对应上游 Status::OK()，message 为空。 */
cldb_status_t cldb_status_ok(void);

/* 通用构造；对应上游私有 Status(Code, msg, msg2)，C 版公开以便统一映射。
 * code 为 OK 时忽略 context/detail；否则消息为 "context: detail"，超长截断。 */
cldb_status_t cldb_status_set(cldb_status_code_t code, const char *context,
                              const char *detail);

/* 各错误码构造器；对应上游 Status::NotFound/Corruption/NotSupported/
 * InvalidArgument/IOError，detail 可为 NULL。 */
cldb_status_t cldb_status_not_found(const char *context, const char *detail);
cldb_status_t cldb_status_corruption(const char *context, const char *detail);
cldb_status_t cldb_status_not_supported(const char *context,
                                        const char *detail);
cldb_status_t cldb_status_invalid_argument(const char *context,
                                           const char *detail);
cldb_status_t cldb_status_io(const char *context, const char *detail);

/* 返回错误码；对应上游私有 code()，C 版公开以便调用者分支。 */
static inline cldb_status_code_t cldb_status_code(cldb_status_t status) {
  return status.code;
}

/* 借用 message；对应上游 ToString() 的消息部分。必须传指针，
 * 因为按值返回会指向形参副本，调用后悬垂；status 为 NULL 时返回空串。 */
static inline const char *cldb_status_message(const cldb_status_t *status) {
  return status != NULL ? status->message : "";
}

/* 以下判断对应上游 ok()/IsNotFound()/IsCorruption()/IsNotSupportedError()/
 * IsInvalidArgument()/IsIOError()；只比较 code，不解析文本。 */
static inline bool cldb_status_is_ok(cldb_status_t status) {
  return status.code == CLDB_STATUS_OK;
}

static inline bool cldb_status_is_not_found(cldb_status_t status) {
  return status.code == CLDB_STATUS_NOT_FOUND;
}

static inline bool cldb_status_is_corruption(cldb_status_t status) {
  return status.code == CLDB_STATUS_CORRUPTION;
}

static inline bool cldb_status_is_not_supported(cldb_status_t status) {
  return status.code == CLDB_STATUS_NOT_SUPPORTED;
}

static inline bool cldb_status_is_invalid_argument(cldb_status_t status) {
  return status.code == CLDB_STATUS_INVALID_ARGUMENT;
}

static inline bool cldb_status_is_io_error(cldb_status_t status) {
  return status.code == CLDB_STATUS_IO_ERROR;
}

/* 对应上游 Status::ToString()，前缀为 "NotFound: " 等；成功返回 "OK"。
 * 写入调用者缓冲并截断；返回所需字节数（不含 NUL），buf 为 NULL 或
 * size 为 0 时只计算，便于调用者先查询长度。 */
size_t cldb_status_to_string(cldb_status_t status, char *buf, size_t size);

#endif /* CLDB_INCLUDE_CLEVELDB_STATUS_H_ */
