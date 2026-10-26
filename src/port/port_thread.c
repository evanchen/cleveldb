#include "port/port_thread.h"

#include <errno.h>
#include <string.h>
#include <time.h>

#include "port/port.h"
#include "port/port_pthread.h"

/* 检查秒数能无损转换成 time_t，避免大时长窄化。 */
static bool seconds_to_time_t(uint64_t seconds, time_t *out) {
  time_t value = (time_t)seconds;

  if (value < 0 || (uint64_t)value != seconds) {
    return false;
  }
  *out = value;
  return true;
}

/* 构造 timespec；tv_nsec 由调用方保证在 [0, 1e9)。 */
static cldb_status_t make_timespec(uint64_t seconds, long nanoseconds,
                                   struct timespec *out) {
  time_t value;

  if (!seconds_to_time_t(seconds, &value)) {
    return cldb_status_invalid_argument("cldb_sleep", "duration is too large");
  }
  out->tv_sec = value;
  out->tv_nsec = nanoseconds;
  return cldb_status_ok();
}

/* 睡眠完整请求；EINTR 后按剩余时间重试，与上游 SleepForMicroseconds
 * 的语义一致。 */
static cldb_status_t sleep_timespec(struct timespec request) {
  struct timespec remaining;

  for (;;) {
    int rc = nanosleep(&request, &remaining);

    if (rc == 0) {
      return cldb_status_ok();
    }
    if (errno != EINTR) {
      int error = errno;

      if (error == EINVAL) {
        return cldb_status_invalid_argument("nanosleep", strerror(error));
      }
      return cldb_status_io("nanosleep", strerror(error));
    }
    request = remaining;
  }
}

void cldb_thread_init(cldb_thread_t *thread) {
  if (thread == NULL) {
    return;
  }
  /* handle 尚无有效值，不触碰 pthread_t 内部表示。 */
  thread->started = false;
}

cldb_status_t cldb_thread_start(cldb_thread_t *thread, cldb_thread_fn_t fn,
                                void *arg) {
  cldb_status_t status;
  int rc;

  if (thread == NULL) {
    return cldb_status_invalid_argument("cldb_thread_start",
                                        "thread is NULL");
  }
  if (fn == NULL) {
    return cldb_status_invalid_argument("cldb_thread_start",
                                        "function is NULL");
  }
  if (thread->started) {
    return cldb_status_invalid_argument("cldb_thread_start",
                                        "thread is already started");
  }
  rc = pthread_create(&thread->handle, NULL, fn, arg);
  status = cldb_pthread_status("pthread_create", rc);
  if (!cldb_status_is_ok(status)) {
    thread->started = false;
    return status;
  }
  thread->started = true;
  return cldb_status_ok();
}

cldb_status_t cldb_thread_join(cldb_thread_t *thread, void **result) {
  cldb_status_t status;
  int rc;

  if (thread == NULL) {
    return cldb_status_invalid_argument("cldb_thread_join",
                                        "thread is NULL");
  }
  if (!thread->started) {
    return cldb_status_invalid_argument("cldb_thread_join",
                                        "thread is not started");
  }
  if (result != NULL) {
    *result = NULL;
  }
  rc = pthread_join(thread->handle, result);
  status = cldb_pthread_status("pthread_join", rc);
  if (cldb_status_is_ok(status)) {
    thread->started = false;
  }
  return status;
}

cldb_status_t cldb_thread_detach(cldb_thread_t *thread) {
  cldb_status_t status;
  int rc;

  if (thread == NULL) {
    return cldb_status_invalid_argument("cldb_thread_detach",
                                        "thread is NULL");
  }
  if (!thread->started) {
    return cldb_status_invalid_argument("cldb_thread_detach",
                                        "thread is not started");
  }
  rc = pthread_detach(thread->handle);
  status = cldb_pthread_status("pthread_detach", rc);
  if (cldb_status_is_ok(status)) {
    thread->started = false;
  }
  return status;
}

cldb_status_t cldb_sleep_for_millis(uint64_t millis) {
  cldb_status_t status;
  struct timespec request;

  status = make_timespec(millis / UINT64_C(1000),
                         (long)((millis % UINT64_C(1000)) *
                                UINT64_C(1000000)),
                         &request);
  if (!cldb_status_is_ok(status)) {
    return status;
  }
  return sleep_timespec(request);
}

cldb_status_t cldb_sleep_for_micros(uint64_t micros) {
  cldb_status_t status;
  struct timespec request;

  status = make_timespec(micros / UINT64_C(1000000),
                         (long)((micros % UINT64_C(1000000)) *
                                UINT64_C(1000)),
                         &request);
  if (!cldb_status_is_ok(status)) {
    return status;
  }
  return sleep_timespec(request);
}
