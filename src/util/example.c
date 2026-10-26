#include "util/example.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "port/port_mutex.h"

/* 私有布局；对应上游把实现细节留在 .cc 的惯例。
 * 字段所有权与锁保护范围在此集中说明，修改时同步更新。 */
struct cldb_example {
  cldb_mutex_t mutex; /* owned；保护 name/value；对应上游 port::Mutex */
  char *name;         /* owned；读写都在 mutex 内；对应上游受锁保护的成员 */
  int64_t value;      /* 受 mutex 保护；演示 DBImpl 式的读-改-写 */
};

/* 深拷贝 C 字符串；对应上游用 std::string 复制入参、由对象接管所有权。 */
static char *copy_string(const char *s) {
  size_t len = strlen(s);
  char *copy = malloc(len + 1);

  if (copy != NULL) {
    memcpy(copy, s, len + 1);
  }
  return copy;
}

/* 检测 int64 加法溢出；上游依赖断言/未定义行为，C 版改为显式错误。 */
static bool add_overflows(int64_t a, int64_t b) {
  if (b > 0) {
    return a > INT64_MAX - b;
  }
  return a < INT64_MIN - b;
}

cldb_status_t cldb_example_create(const char *name, cldb_example_t **out) {
  cldb_status_t status = cldb_status_ok();
  cldb_example_t *example = NULL;
  char *name_copy = NULL;
  bool mutex_ready = false;

  if (out == NULL) {
    return cldb_status_invalid_argument("cldb_example_create", "out is NULL");
  }
  *out = NULL;
  if (name == NULL) {
    return cldb_status_invalid_argument("cldb_example_create", "name is NULL");
  }

  example = calloc(1, sizeof(*example));
  if (example == NULL) {
    status = cldb_status_io("out of memory", NULL);
    goto cleanup;
  }
  status = cldb_mutex_init(&example->mutex);
  if (!cldb_status_is_ok(status)) {
    goto cleanup;
  }
  mutex_ready = true;

  name_copy = copy_string(name);
  if (name_copy == NULL) {
    status = cldb_status_io("out of memory", NULL);
    goto cleanup;
  }

  example->name = name_copy;
  name_copy = NULL;
  *out = example;
  example = NULL;

cleanup:
  free(name_copy);
  if (example != NULL) {
    if (mutex_ready) {
      (void)cldb_mutex_destroy(&example->mutex);
    }
    free(example->name);
    free(example);
  }
  return status;
}

void cldb_example_destroy(cldb_example_t *example) {
  if (example == NULL) {
    return;
  }
  (void)cldb_mutex_destroy(&example->mutex);
  free(example->name);
  free(example);
}

const char *cldb_example_name(cldb_example_t *example) {
  return example != NULL ? example->name : NULL;
}

cldb_status_t cldb_example_set_name(cldb_example_t *example,
                                    const char *name) {
  cldb_status_t status;
  char *copy;
  bool locked = false;

  if (example == NULL || name == NULL) {
    return cldb_status_invalid_argument("cldb_example_set_name",
                                        "example or name is NULL");
  }
  copy = copy_string(name);
  if (copy == NULL) {
    return cldb_status_io("out of memory", NULL);
  }
  status = cldb_mutex_lock(&example->mutex);
  if (!cldb_status_is_ok(status)) {
    goto cleanup;
  }
  locked = true;
  free(example->name);
  example->name = copy;
  copy = NULL; /* 所有权转移给 example */

cleanup:
  /* 进入 cleanup 时若仍持锁则统一解锁；解锁失败覆盖成功状态。 */
  if (locked) {
    cldb_status_t unlock_status = cldb_mutex_unlock(&example->mutex);

    if (cldb_status_is_ok(status)) {
      status = unlock_status;
    }
  }
  free(copy);
  return status;
}

cldb_status_t cldb_example_add(cldb_example_t *example, int64_t delta,
                               int64_t *out) {
  cldb_status_t status;
  int64_t next = 0;
  bool locked = false;

  if (example == NULL) {
    return cldb_status_invalid_argument("cldb_example_add", "example is NULL");
  }
  status = cldb_mutex_lock(&example->mutex);
  if (!cldb_status_is_ok(status)) {
    goto cleanup;
  }
  locked = true;
  if (add_overflows(example->value, delta)) {
    status = cldb_status_invalid_argument("cldb_example_add",
                                          "integer overflow");
  } else {
    example->value += delta;
    next = example->value;
  }

cleanup:
  if (locked) {
    cldb_status_t unlock_status = cldb_mutex_unlock(&example->mutex);

    if (cldb_status_is_ok(status)) {
      status = unlock_status;
    }
  }
  if (cldb_status_is_ok(status) && out != NULL) {
    *out = next;
  }
  return status;
}

cldb_status_t cldb_example_get_value(cldb_example_t *example, int64_t *out) {
  cldb_status_t status;

  if (example == NULL || out == NULL) {
    return cldb_status_invalid_argument("cldb_example_get_value",
                                        "example or out is NULL");
  }
  status = cldb_mutex_lock(&example->mutex);
  if (!cldb_status_is_ok(status)) {
    return status;
  }
  *out = example->value;
  status = cldb_mutex_unlock(&example->mutex);
  return status;
}
