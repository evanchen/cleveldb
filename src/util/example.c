/*
 * 上游：无（T003 规范示例）。
 * 职责：演示 create/destroy、错误传播、goto cleanup、所有权与锁注释。
 * 锁：mutex 保护 name/value；name() 返回借用指针。
 */

#include "util/example.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* 字段所有权与锁保护范围写在这里，修改时同步更新。 */
struct cldb_example {
  pthread_mutex_t mutex; /* 保护 name/value */
  char *name;            /* owned；受 mutex 保护 */
  int64_t value;         /* 受 mutex 保护 */
};

static char *copy_string(const char *s) {
  size_t len = strlen(s);
  char *copy = malloc(len + 1);

  if (copy != NULL) {
    memcpy(copy, s, len + 1);
  }
  return copy;
}

static bool add_overflows(int64_t a, int64_t b) {
  if (b > 0) {
    return a > INT64_MAX - b;
  }
  return a < INT64_MIN - b;
}

cldb_example_result_t cldb_example_create(const char *name,
                                          cldb_example_t **out) {
  cldb_example_result_t result = CLDB_EXAMPLE_OK;
  cldb_example_t *example = NULL;
  char *name_copy = NULL;
  bool mutex_ready = false;

  if (out == NULL) {
    return CLDB_EXAMPLE_INVALID_ARGUMENT;
  }
  *out = NULL;
  if (name == NULL) {
    return CLDB_EXAMPLE_INVALID_ARGUMENT;
  }

  example = calloc(1, sizeof(*example));
  if (example == NULL) {
    result = CLDB_EXAMPLE_OUT_OF_MEMORY;
    goto cleanup;
  }
  if (pthread_mutex_init(&example->mutex, NULL) != 0) {
    result = CLDB_EXAMPLE_SYSTEM_ERROR;
    goto cleanup;
  }
  mutex_ready = true;

  name_copy = copy_string(name);
  if (name_copy == NULL) {
    result = CLDB_EXAMPLE_OUT_OF_MEMORY;
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
      pthread_mutex_destroy(&example->mutex);
    }
    free(example->name);
    free(example);
  }
  return result;
}

void cldb_example_destroy(cldb_example_t *example) {
  if (example == NULL) {
    return;
  }
  pthread_mutex_destroy(&example->mutex);
  free(example->name);
  free(example);
}

const char *cldb_example_name(cldb_example_t *example) {
  return example != NULL ? example->name : NULL;
}

cldb_example_result_t cldb_example_set_name(cldb_example_t *example,
                                            const char *name) {
  char *copy;

  if (example == NULL || name == NULL) {
    return CLDB_EXAMPLE_INVALID_ARGUMENT;
  }
  copy = copy_string(name);
  if (copy == NULL) {
    return CLDB_EXAMPLE_OUT_OF_MEMORY;
  }
  if (pthread_mutex_lock(&example->mutex) != 0) {
    free(copy);
    return CLDB_EXAMPLE_SYSTEM_ERROR;
  }
  free(example->name);
  example->name = copy;
  (void)pthread_mutex_unlock(&example->mutex);
  return CLDB_EXAMPLE_OK;
}

cldb_example_result_t cldb_example_add(cldb_example_t *example, int64_t delta,
                                       int64_t *out) {
  cldb_example_result_t result = CLDB_EXAMPLE_OK;
  int64_t next = 0;

  if (example == NULL) {
    return CLDB_EXAMPLE_INVALID_ARGUMENT;
  }
  if (pthread_mutex_lock(&example->mutex) != 0) {
    return CLDB_EXAMPLE_SYSTEM_ERROR;
  }
  if (add_overflows(example->value, delta)) {
    result = CLDB_EXAMPLE_RANGE_ERROR;
  } else {
    example->value += delta;
    next = example->value;
  }
  (void)pthread_mutex_unlock(&example->mutex);

  if (result == CLDB_EXAMPLE_OK && out != NULL) {
    *out = next;
  }
  return result;
}

cldb_example_result_t cldb_example_get_value(cldb_example_t *example,
                                             int64_t *out) {
  if (example == NULL || out == NULL) {
    return CLDB_EXAMPLE_INVALID_ARGUMENT;
  }
  if (pthread_mutex_lock(&example->mutex) != 0) {
    return CLDB_EXAMPLE_SYSTEM_ERROR;
  }
  *out = example->value;
  (void)pthread_mutex_unlock(&example->mutex);
  return CLDB_EXAMPLE_OK;
}

const char *cldb_example_result_message(cldb_example_result_t result) {
  switch (result) {
    case CLDB_EXAMPLE_OK:
      return "ok";
    case CLDB_EXAMPLE_INVALID_ARGUMENT:
      return "invalid argument";
    case CLDB_EXAMPLE_OUT_OF_MEMORY:
      return "out of memory";
    case CLDB_EXAMPLE_RANGE_ERROR:
      return "range error";
    case CLDB_EXAMPLE_SYSTEM_ERROR:
      return "system error";
  }
  return "unknown result";
}
