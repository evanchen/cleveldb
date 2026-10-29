#include "temp_dir.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* 同一进程内递增，保证子目录名不重复；测试串行创建，无需原子操作。 */
static unsigned int g_temp_dir_counter;

static cldb_status_t out_of_memory(void) {
  return cldb_status_io("TempDir", "out of memory");
}

/* 追加路径片段；失败时缓冲保持原样。 */
static bool append_path(cldb_buf_t *path, const char *value) {
  return cldb_buf_append(path, value, strlen(value));
}

/* 把 "-pid-counter" 追加到 path；counter 变化后用于重试。 */
static bool append_unique_suffix(cldb_buf_t *path) {
  char suffix[64];
  int length = snprintf(suffix, sizeof(suffix), "-%ld-%u", (long)getpid(),
                        g_temp_dir_counter);

  if (length < 0 || (size_t)length >= sizeof(suffix)) {
    return false;
  }
  g_temp_dir_counter++;
  return cldb_buf_append(path, suffix, (size_t)length);
}

/* 递归删除单个路径；目录会先删除所有子项。lstat 不跟随符号链接。 */
static void remove_tree(cldb_env_t *env, const char *path) {
  struct stat file_stat;
  cldb_strvec_t children;
  cldb_buf_t child_path;
  size_t i;

  if (lstat(path, &file_stat) != 0) {
    return;
  }
  if (!S_ISDIR(file_stat.st_mode)) {
    (void)cldb_env_remove_file(env, path);
    return;
  }
  cldb_strvec_init(&children);
  if (cldb_status_is_ok(cldb_env_get_children(env, path, &children))) {
    cldb_buf_init(&child_path);
    for (i = 0; i < cldb_strvec_size(&children); ++i) {
      const char *name = cldb_strvec_get(&children, i);

      if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        continue;
      }
      cldb_buf_clear(&child_path);
      if (!append_path(&child_path, path) ||
          !append_path(&child_path, "/") ||
          !append_path(&child_path, name)) {
        continue;
      }
      remove_tree(env, cldb_buf_cstr(&child_path));
    }
    cldb_buf_free(&child_path);
  }
  cldb_strvec_free(&children);
  (void)cldb_env_remove_dir(env, path);
}

cldb_status_t cldb_test_temp_dir_create(cldb_env_t *env, const char *prefix,
                                        cldb_test_temp_dir_t *dir) {
  enum { kMaxAttempts = 100 };
  size_t prefix_end;
  cldb_status_t status;
  int attempt;

  if (env == NULL || dir == NULL) {
    return cldb_status_invalid_argument("TempDir", "invalid argument");
  }
  dir->env = env;
  cldb_buf_init(&dir->path);
  status = cldb_env_get_test_directory(env, &dir->path);
  if (!cldb_status_is_ok(status)) {
    return status;
  }
  if (!append_path(&dir->path, "/") ||
      !append_path(&dir->path, prefix != NULL ? prefix : "cldb")) {
    return out_of_memory();
  }
  prefix_end = dir->path.size;

  for (attempt = 0; attempt < kMaxAttempts; ++attempt) {
    if (!cldb_buf_resize(&dir->path, prefix_end) ||
        !append_unique_suffix(&dir->path)) {
      return out_of_memory();
    }
    status = cldb_env_create_dir(env, cldb_buf_cstr(&dir->path));
    if (cldb_status_is_ok(status)) {
      return status;
    }
  }
  return status;
}

void cldb_test_temp_dir_destroy(cldb_test_temp_dir_t *dir) {
  if (dir == NULL) {
    return;
  }
  if (dir->env != NULL && dir->path.size > 0) {
    remove_tree(dir->env, cldb_buf_cstr(&dir->path));
  }
  cldb_buf_free(&dir->path);
  dir->env = NULL;
}

const char *cldb_test_temp_dir_path(const cldb_test_temp_dir_t *dir) {
  if (dir == NULL) {
    return "";
  }
  return cldb_buf_cstr(&dir->path);
}
