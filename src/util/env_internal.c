#include "util/env_internal.h"

#include <stdlib.h>
#include <string.h>

cldb_status_t cldb_env_out_of_memory(const char *context) {
  return cldb_status_io(context, "out of memory");
}

char *cldb_env_strdup(const char *value) {
  size_t length = strlen(value);
  char *copy = malloc(length + 1);

  if (copy != NULL) {
    memcpy(copy, value, length + 1);
  }
  return copy;
}
