#include "port/port_mutex.h"

#include "port/port.h"
#include "port/port_pthread.h"

/* 检查互斥量已初始化；不负责并发销毁，生命周期由调用者约定。 */
static cldb_status_t require_mutex(const cldb_mutex_t *mutex,
                                   const char *operation) {
  if (mutex == NULL) {
    return cldb_status_invalid_argument(operation, "mutex is NULL");
  }
  if (!mutex->initialized) {
    return cldb_status_invalid_argument(operation, "mutex is not initialized");
  }
  return cldb_status_ok();
}

/* 条件变量及其绑定互斥量都必须有效；等待期间需持有该互斥量。 */
static cldb_status_t require_condvar(const cldb_condvar_t *condvar,
                                     const char *operation) {
  if (condvar == NULL) {
    return cldb_status_invalid_argument(operation, "condvar is NULL");
  }
  if (!condvar->initialized) {
    return cldb_status_invalid_argument(operation,
                                        "condvar is not initialized");
  }
  if (condvar->mutex == NULL || !condvar->mutex->initialized) {
    return cldb_status_invalid_argument(operation,
                                        "bound mutex is not initialized");
  }
  return cldb_status_ok();
}

cldb_status_t cldb_mutex_init(cldb_mutex_t *mutex) {
  int rc;

  if (mutex == NULL) {
    return cldb_status_invalid_argument("cldb_mutex_init", "mutex is NULL");
  }
  /* 先标记为未初始化：init 失败时 destroy 等误用会被拒绝。 */
  mutex->initialized = false;
  rc = pthread_mutex_init(&mutex->mutex, NULL);
  if (rc != 0) {
    return cldb_pthread_status("pthread_mutex_init", rc);
  }
  mutex->initialized = true;
  return cldb_status_ok();
}

cldb_status_t cldb_mutex_destroy(cldb_mutex_t *mutex) {
  cldb_status_t status;
  int rc;

  if (mutex == NULL) {
    return cldb_status_ok();
  }
  status = require_mutex(mutex, "cldb_mutex_destroy");
  if (!cldb_status_is_ok(status)) {
    return status;
  }
  rc = pthread_mutex_destroy(&mutex->mutex);
  status = cldb_pthread_status("pthread_mutex_destroy", rc);
  if (cldb_status_is_ok(status)) {
    mutex->initialized = false;
  }
  return status;
}

cldb_status_t cldb_mutex_lock(cldb_mutex_t *mutex) {
  cldb_status_t status = require_mutex(mutex, "cldb_mutex_lock");

  if (!cldb_status_is_ok(status)) {
    return status;
  }
  return cldb_pthread_status("pthread_mutex_lock",
                             pthread_mutex_lock(&mutex->mutex));
}

cldb_status_t cldb_mutex_unlock(cldb_mutex_t *mutex) {
  cldb_status_t status = require_mutex(mutex, "cldb_mutex_unlock");

  if (!cldb_status_is_ok(status)) {
    return status;
  }
  return cldb_pthread_status("pthread_mutex_unlock",
                             pthread_mutex_unlock(&mutex->mutex));
}

cldb_status_t cldb_condvar_init(cldb_condvar_t *condvar,
                                cldb_mutex_t *mutex) {
  cldb_status_t status;
  int rc;

  if (condvar == NULL) {
    return cldb_status_invalid_argument("cldb_condvar_init",
                                        "condvar is NULL");
  }
  status = require_mutex(mutex, "cldb_condvar_init");
  if (!cldb_status_is_ok(status)) {
    return status;
  }
  condvar->initialized = false;
  condvar->mutex = NULL;
  rc = pthread_cond_init(&condvar->cond, NULL);
  if (rc != 0) {
    return cldb_pthread_status("pthread_cond_init", rc);
  }
  condvar->mutex = mutex;
  condvar->initialized = true;
  return cldb_status_ok();
}

cldb_status_t cldb_condvar_destroy(cldb_condvar_t *condvar) {
  cldb_status_t status;
  int rc;

  if (condvar == NULL) {
    return cldb_status_ok();
  }
  status = require_condvar(condvar, "cldb_condvar_destroy");
  if (!cldb_status_is_ok(status)) {
    return status;
  }
  rc = pthread_cond_destroy(&condvar->cond);
  status = cldb_pthread_status("pthread_cond_destroy", rc);
  if (cldb_status_is_ok(status)) {
    condvar->initialized = false;
    condvar->mutex = NULL;
  }
  return status;
}

cldb_status_t cldb_condvar_wait(cldb_condvar_t *condvar) {
  cldb_status_t status = require_condvar(condvar, "cldb_condvar_wait");

  if (!cldb_status_is_ok(status)) {
    return status;
  }
  return cldb_pthread_status("pthread_cond_wait",
                             pthread_cond_wait(&condvar->cond,
                                               &condvar->mutex->mutex));
}

cldb_status_t cldb_condvar_signal(cldb_condvar_t *condvar) {
  cldb_status_t status = require_condvar(condvar, "cldb_condvar_signal");

  if (!cldb_status_is_ok(status)) {
    return status;
  }
  return cldb_pthread_status("pthread_cond_signal",
                             pthread_cond_signal(&condvar->cond));
}

cldb_status_t cldb_condvar_broadcast(cldb_condvar_t *condvar) {
  cldb_status_t status = require_condvar(condvar, "cldb_condvar_broadcast");

  if (!cldb_status_is_ok(status)) {
    return status;
  }
  return cldb_pthread_status("pthread_cond_broadcast",
                             pthread_cond_broadcast(&condvar->cond));
}
