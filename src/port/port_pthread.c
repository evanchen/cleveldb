#include "port/port_pthread.h"

#include <errno.h>
#include <string.h>

cldb_status_t cldb_pthread_status(const char *operation, int rc) {
  if (rc == 0) {
    return cldb_status_ok();
  }
  switch (rc) {
    case EINVAL:
    case EPERM:
    case EDEADLK:
    case ESRCH:
    case EBUSY:
      return cldb_status_invalid_argument(operation, strerror(rc));
    default:
      return cldb_status_io(operation, strerror(rc));
  }
}
