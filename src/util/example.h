#ifndef CLDB_SRC_UTIL_EXAMPLE_H_
#define CLDB_SRC_UTIL_EXAMPLE_H_

#include <stdint.h>

/*
 * T003 规范示例：演示不透明句柄、所有权、错误传播与锁注释。
 * T102 实现 cldb_status_t 后，把本地 result 枚举迁移为统一状态。
 */
typedef enum cldb_example_result {
  CLDB_EXAMPLE_OK = 0,
  CLDB_EXAMPLE_INVALID_ARGUMENT = 1,
  CLDB_EXAMPLE_OUT_OF_MEMORY = 2,
  CLDB_EXAMPLE_RANGE_ERROR = 3,
  CLDB_EXAMPLE_SYSTEM_ERROR = 4
} cldb_example_result_t;

typedef struct cldb_example cldb_example_t;

/* 成功时 *out 拥有对象；失败时 *out = NULL。 */
cldb_example_result_t cldb_example_create(const char *name,
                                          cldb_example_t **out);

/* 接受 NULL；销毁后指针失效，不得重复销毁。 */
void cldb_example_destroy(cldb_example_t *example);

/* 借用：返回内部 name，直到 set_name/destroy；不得 free 或并发写。 */
const char *cldb_example_name(cldb_example_t *example);

/* 复制到内部；失败时旧 name 保持不变。 */
cldb_example_result_t cldb_example_set_name(cldb_example_t *example,
                                            const char *name);

/* 加 delta；溢出返回 RANGE_ERROR，成功时可选写入 *out。 */
cldb_example_result_t cldb_example_add(cldb_example_t *example, int64_t delta,
                                       int64_t *out);

/* 读取当前值；out 必须非 NULL。 */
cldb_example_result_t cldb_example_get_value(cldb_example_t *example,
                                             int64_t *out);

/* 静态诊断文本，永不返回 NULL；文本不构成契约。 */
const char *cldb_example_result_message(cldb_example_result_t result);

#endif /* CLDB_SRC_UTIL_EXAMPLE_H_ */
