#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "helpers/memenv/memenv.h"
#include "port/port_atomic.h"
#include "status_test_util.h"

/* 内存 Env 测试；对应上游 helpers/memenv/memenv_test.cc。锁测试按本项目
 * 语义检查重复加锁失败，DBTest 留到 DB 可用后再移植。 */

static cldb_env_t *new_mem_env(void) {
  cldb_env_t *env = cldb_memenv_create(NULL);

  CLDB_ASSERT_TRUE(env != NULL);
  return env;
}

static bool strvec_contains(const cldb_strvec_t *vec, const char *value) {
  size_t i;

  for (i = 0; i < cldb_strvec_size(vec); ++i) {
    if (strcmp(cldb_strvec_get(vec, i), value) == 0) {
      return true;
    }
  }
  return false;
}

static void write_text(cldb_env_t *env, const char *path, const char *text) {
  CLDB_ASSERT_OK(cldb_write_string_to_file(
      env, cldb_slice_make(text, strlen(text)), path));
}

static void expect_text(cldb_env_t *env, const char *path,
                        const char *expected) {
  cldb_buf_t data;
  size_t expected_size = strlen(expected);

  cldb_buf_init(&data);
  CLDB_ASSERT_OK(cldb_read_file_to_string(env, path, &data));
  CLDB_EXPECT_EQ_UINT(expected_size, data.size);
  if (data.size == expected_size) {
    CLDB_EXPECT_EQ_INT(0, memcmp(data.data, expected, expected_size));
  }
  cldb_buf_free(&data);
}

static void mark_flag(void *arg) {
  cldb_atomic_u32_store((cldb_atomic_u32_t *)arg, 1, memory_order_release);
}

static void fill_pattern(char *data, size_t size) {
  size_t i;

  for (i = 0; i < size; ++i) {
    data[i] = (char)((i * 31 + 7) % 251);
  }
}

CLDB_TEST(memenv_basics) {
  cldb_env_t *env = new_mem_env();
  cldb_writable_file_t *writable = NULL;
  cldb_sequential_file_t *sequential = NULL;
  cldb_random_access_file_t *random = NULL;
  cldb_strvec_t children;
  uint64_t file_size = 99;

  cldb_strvec_init(&children);
  CLDB_ASSERT_OK(cldb_env_create_dir(env, "/dir"));

  CLDB_EXPECT_FALSE(cldb_env_file_exists(env, "/dir/non_existent"));
  CLDB_EXPECT_STATUS(CLDB_STATUS_NOT_FOUND,
                     cldb_env_get_file_size(env, "/dir/non_existent",
                                            &file_size));
  CLDB_EXPECT_EQ_UINT(0, file_size);
  CLDB_ASSERT_OK(cldb_env_get_children(env, "/dir", &children));
  CLDB_EXPECT_EQ_UINT(0, cldb_strvec_size(&children));

  CLDB_ASSERT_OK(cldb_env_new_writable_file(env, "/dir/f", &writable));
  CLDB_EXPECT_OK(cldb_env_get_file_size(env, "/dir/f", &file_size));
  CLDB_EXPECT_EQ_UINT(0, file_size);
  cldb_writable_file_destroy(writable);
  writable = NULL;

  CLDB_EXPECT_TRUE(cldb_env_file_exists(env, "/dir/f"));
  CLDB_ASSERT_OK(cldb_env_get_children(env, "/dir", &children));
  CLDB_EXPECT_EQ_UINT(1, cldb_strvec_size(&children));
  CLDB_EXPECT_EQ_STR("f", cldb_strvec_get(&children, 0));

  CLDB_ASSERT_OK(cldb_env_new_writable_file(env, "/dir/f", &writable));
  CLDB_ASSERT_OK(
      cldb_writable_file_append(writable, cldb_slice_make("abc", 3)));
  cldb_writable_file_destroy(writable);
  writable = NULL;

  CLDB_ASSERT_OK(cldb_env_new_appendable_file(env, "/dir/f", &writable));
  CLDB_EXPECT_OK(cldb_env_get_file_size(env, "/dir/f", &file_size));
  CLDB_EXPECT_EQ_UINT(3, file_size);
  CLDB_ASSERT_OK(
      cldb_writable_file_append(writable, cldb_slice_make("hello", 5)));
  cldb_writable_file_destroy(writable);
  writable = NULL;

  CLDB_EXPECT_OK(cldb_env_get_file_size(env, "/dir/f", &file_size));
  CLDB_EXPECT_EQ_UINT(8, file_size);
  expect_text(env, "/dir/f", "abchello");

  CLDB_EXPECT_STATUS(CLDB_STATUS_NOT_FOUND,
                     cldb_env_rename_file(env, "/dir/non_existent",
                                          "/dir/g"));
  CLDB_ASSERT_OK(cldb_env_rename_file(env, "/dir/f", "/dir/g"));
  CLDB_EXPECT_FALSE(cldb_env_file_exists(env, "/dir/f"));
  CLDB_EXPECT_TRUE(cldb_env_file_exists(env, "/dir/g"));
  CLDB_EXPECT_OK(cldb_env_get_file_size(env, "/dir/g", &file_size));
  CLDB_EXPECT_EQ_UINT(8, file_size);

  /* 自身重命名是 no-op；目标已存在时替换目标内容。 */
  CLDB_EXPECT_OK(cldb_env_rename_file(env, "/dir/g", "/dir/g"));
  write_text(env, "/dir/h", "z");
  CLDB_ASSERT_OK(cldb_env_rename_file(env, "/dir/g", "/dir/h"));
  CLDB_EXPECT_FALSE(cldb_env_file_exists(env, "/dir/g"));
  expect_text(env, "/dir/h", "abchello");

  CLDB_EXPECT_OK(cldb_env_get_children(env, "/dir", &children));
  CLDB_EXPECT_EQ_UINT(1, cldb_strvec_size(&children));
  CLDB_EXPECT_EQ_STR("h", cldb_strvec_get(&children, 0));

  CLDB_EXPECT_STATUS(CLDB_STATUS_NOT_FOUND,
                     cldb_env_new_sequential_file(env, "/dir/non_existent",
                                                  &sequential));
  CLDB_EXPECT_EQ_PTR(NULL, sequential);
  CLDB_EXPECT_STATUS(CLDB_STATUS_NOT_FOUND,
                     cldb_env_new_random_access_file(
                         env, "/dir/non_existent", &random));
  CLDB_EXPECT_EQ_PTR(NULL, random);

  CLDB_EXPECT_STATUS(CLDB_STATUS_NOT_FOUND,
                     cldb_env_remove_file(env, "/dir/non_existent"));
  CLDB_EXPECT_OK(cldb_env_delete_file(env, "/dir/h"));
  CLDB_EXPECT_FALSE(cldb_env_file_exists(env, "/dir/h"));
  CLDB_EXPECT_OK(cldb_env_get_children(env, "/dir", &children));
  CLDB_EXPECT_EQ_UINT(0, cldb_strvec_size(&children));
  CLDB_EXPECT_OK(cldb_env_remove_dir(env, "/dir"));

  cldb_strvec_free(&children);
  cldb_env_destroy(env);
}

CLDB_TEST(memenv_read_write) {
  cldb_env_t *env = new_mem_env();
  cldb_writable_file_t *writable = NULL;
  cldb_sequential_file_t *sequential = NULL;
  cldb_random_access_file_t *random = NULL;
  cldb_slice_t result;
  char scratch[1024];

  CLDB_ASSERT_OK(cldb_env_new_writable_file(env, "/dir/f", &writable));
  CLDB_ASSERT_OK(
      cldb_writable_file_append(writable, cldb_slice_make("hello ", 6)));
  CLDB_ASSERT_OK(
      cldb_writable_file_append(writable, cldb_slice_make("world", 5)));
  cldb_writable_file_destroy(writable);
  writable = NULL;

  CLDB_ASSERT_OK(cldb_env_new_sequential_file(env, "/dir/f", &sequential));
  CLDB_ASSERT_OK(cldb_sequential_file_read(sequential, 5, &result, scratch));
  CLDB_EXPECT_TRUE(cldb_slice_equal(cldb_slice_make("hello", 5), result));
  CLDB_ASSERT_OK(cldb_sequential_file_skip(sequential, 1));
  CLDB_ASSERT_OK(
      cldb_sequential_file_read(sequential, 1000, &result, scratch));
  CLDB_EXPECT_TRUE(cldb_slice_equal(cldb_slice_make("world", 5), result));
  CLDB_ASSERT_OK(
      cldb_sequential_file_read(sequential, 1000, &result, scratch));
  CLDB_EXPECT_EQ_UINT(0, result.size);
  CLDB_ASSERT_OK(cldb_sequential_file_skip(sequential, 100));
  CLDB_ASSERT_OK(
      cldb_sequential_file_read(sequential, 1000, &result, scratch));
  CLDB_EXPECT_EQ_UINT(0, result.size);
  cldb_sequential_file_destroy(sequential);
  sequential = NULL;

  CLDB_ASSERT_OK(
      cldb_env_new_random_access_file(env, "/dir/f", &random));
  CLDB_ASSERT_OK(
      cldb_random_access_file_read(random, 6, 5, &result, scratch));
  CLDB_EXPECT_TRUE(cldb_slice_equal(cldb_slice_make("world", 5), result));
  CLDB_ASSERT_OK(
      cldb_random_access_file_read(random, 0, 5, &result, scratch));
  CLDB_EXPECT_TRUE(cldb_slice_equal(cldb_slice_make("hello", 5), result));
  CLDB_ASSERT_OK(
      cldb_random_access_file_read(random, 10, 100, &result, scratch));
  CLDB_EXPECT_TRUE(cldb_slice_equal(cldb_slice_make("d", 1), result));
  CLDB_EXPECT_STATUS(CLDB_STATUS_IO_ERROR,
                     cldb_random_access_file_read(random, 1000, 5, &result,
                                                  scratch));
  CLDB_EXPECT_EQ_UINT(0, result.size);
  cldb_random_access_file_destroy(random);
  random = NULL;

  cldb_env_destroy(env);
}

CLDB_TEST(memenv_locks) {
  cldb_env_t *env = new_mem_env();
  cldb_env_t *other = new_mem_env();
  cldb_file_lock_t *lock = NULL;
  cldb_file_lock_t *second = NULL;
  cldb_file_lock_t *other_lock = NULL;

  CLDB_ASSERT_OK(cldb_env_lock_file(env, "/lock/a", &lock));
  CLDB_EXPECT_TRUE(lock != NULL);
  CLDB_EXPECT_STATUS(CLDB_STATUS_IO_ERROR,
                     cldb_env_lock_file(env, "/lock/a", &second));
  CLDB_EXPECT_EQ_PTR(NULL, second);

  CLDB_ASSERT_OK(cldb_env_lock_file(env, "/lock/b", &second));
  CLDB_EXPECT_TRUE(second != NULL);

  /* 不同 MemEnv 的锁表互相独立。 */
  CLDB_ASSERT_OK(cldb_env_lock_file(other, "/lock/a", &other_lock));
  CLDB_EXPECT_TRUE(other_lock != NULL);
  CLDB_EXPECT_OK(cldb_env_unlock_file(other, other_lock));
  other_lock = NULL;

  CLDB_EXPECT_OK(cldb_env_unlock_file(env, lock));
  lock = NULL;
  CLDB_ASSERT_OK(cldb_env_lock_file(env, "/lock/a", &lock));
  CLDB_EXPECT_TRUE(lock != NULL);
  CLDB_EXPECT_OK(cldb_env_unlock_file(env, lock));
  lock = NULL;

  CLDB_EXPECT_OK(cldb_env_unlock_file(env, second));
  second = NULL;

  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_env_unlock_file(env, NULL));

  cldb_env_destroy(other);
  cldb_env_destroy(env);
}

CLDB_TEST(memenv_misc) {
  cldb_env_t *env = new_mem_env();
  cldb_buf_t test_dir;
  cldb_strvec_t children;
  cldb_writable_file_t *writable = NULL;
  uint64_t file_size = 0;

  cldb_buf_init(&test_dir);
  cldb_strvec_init(&children);

  CLDB_ASSERT_OK(cldb_env_get_test_directory(env, &test_dir));
  CLDB_EXPECT_EQ_STR("/test", cldb_buf_cstr(&test_dir));
  CLDB_ASSERT_OK(cldb_env_get_test_directory(env, &test_dir));
  CLDB_EXPECT_EQ_STR("/test", cldb_buf_cstr(&test_dir));

  /* 目录只是路径前缀，父目录无需显式创建。 */
  CLDB_ASSERT_OK(cldb_env_new_writable_file(env, "/a/b", &writable));
  CLDB_EXPECT_OK(cldb_writable_file_sync(writable));
  CLDB_EXPECT_OK(cldb_writable_file_flush(writable));
  CLDB_EXPECT_OK(cldb_writable_file_close(writable));
  cldb_writable_file_destroy(writable);
  writable = NULL;

  CLDB_EXPECT_FALSE(cldb_env_file_exists(env, "/a"));
  CLDB_ASSERT_OK(cldb_env_get_children(env, "/a", &children));
  CLDB_EXPECT_EQ_UINT(1, cldb_strvec_size(&children));
  CLDB_EXPECT_EQ_STR("b", cldb_strvec_get(&children, 0));

  CLDB_ASSERT_OK(cldb_env_new_appendable_file(env, "/newfile", &writable));
  CLDB_EXPECT_OK(cldb_env_get_file_size(env, "/newfile", &file_size));
  CLDB_EXPECT_EQ_UINT(0, file_size);
  CLDB_ASSERT_OK(
      cldb_writable_file_append(writable, cldb_slice_make("x", 1)));
  cldb_writable_file_destroy(writable);
  writable = NULL;
  CLDB_EXPECT_OK(cldb_env_get_file_size(env, "/newfile", &file_size));
  CLDB_EXPECT_EQ_UINT(1, file_size);

  CLDB_ASSERT_OK(cldb_env_create_dir(env, "/empty"));
  CLDB_ASSERT_OK(cldb_env_get_children(env, "/empty", &children));
  CLDB_EXPECT_EQ_UINT(0, cldb_strvec_size(&children));
  CLDB_EXPECT_OK(cldb_env_delete_dir(env, "/empty"));

  CLDB_EXPECT_OK(cldb_env_remove_file(env, "/a/b"));
  CLDB_ASSERT_OK(cldb_env_get_children(env, "/a", &children));
  CLDB_EXPECT_EQ_UINT(0, cldb_strvec_size(&children));

  cldb_strvec_free(&children);
  cldb_buf_free(&test_dir);
  cldb_env_destroy(env);
}

CLDB_TEST(memenv_children_follow_path_prefix) {
  cldb_env_t *env = new_mem_env();
  cldb_strvec_t children;
  size_t i;
  bool saw_dot = false;

  cldb_strvec_init(&children);
  write_text(env, "/root/a.txt", "a");
  write_text(env, "/root/sub/b.txt", "b");
  write_text(env, "/root/sub/deep/c.txt", "c");

  /* 与上游一致：返回所有 dir/ 前缀条目，后缀可含 '/'，且没有 "."/".."。 */
  CLDB_ASSERT_OK(cldb_env_get_children(env, "/root", &children));
  CLDB_EXPECT_EQ_UINT(3, cldb_strvec_size(&children));
  CLDB_EXPECT_TRUE(strvec_contains(&children, "a.txt"));
  CLDB_EXPECT_TRUE(strvec_contains(&children, "sub/b.txt"));
  CLDB_EXPECT_TRUE(strvec_contains(&children, "sub/deep/c.txt"));
  for (i = 0; i < cldb_strvec_size(&children); ++i) {
    if (strcmp(cldb_strvec_get(&children, i), ".") == 0 ||
        strcmp(cldb_strvec_get(&children, i), "..") == 0) {
      saw_dot = true;
    }
  }
  CLDB_EXPECT_FALSE(saw_dot);

  CLDB_ASSERT_OK(cldb_env_get_children(env, "/root/sub", &children));
  CLDB_EXPECT_EQ_UINT(2, cldb_strvec_size(&children));
  CLDB_EXPECT_TRUE(strvec_contains(&children, "b.txt"));
  CLDB_EXPECT_TRUE(strvec_contains(&children, "deep/c.txt"));

  CLDB_ASSERT_OK(cldb_env_get_children(env, "/root/sub/deep", &children));
  CLDB_EXPECT_EQ_UINT(1, cldb_strvec_size(&children));
  CLDB_EXPECT_EQ_STR("c.txt", cldb_strvec_get(&children, 0));

  /* 不存在的目录没有表项，与上游一样返回空列表而不是错误。 */
  CLDB_ASSERT_OK(cldb_env_get_children(env, "/missing", &children));
  CLDB_EXPECT_EQ_UINT(0, cldb_strvec_size(&children));

  cldb_strvec_free(&children);
  cldb_env_destroy(env);
}

CLDB_TEST(memenv_large_write) {
  enum { kWriteSize = 300 * 1024 };
  cldb_env_t *env = new_mem_env();
  cldb_writable_file_t *writable = NULL;
  cldb_random_access_file_t *random = NULL;
  cldb_buf_t read_back;
  cldb_slice_t result;
  char *data = malloc(kWriteSize);
  char scratch[8];
  uint64_t file_size = 0;

  CLDB_ASSERT_TRUE(data != NULL);
  fill_pattern(data, kWriteSize);
  cldb_buf_init(&read_back);

  CLDB_ASSERT_OK(cldb_env_new_writable_file(env, "/dir/f", &writable));
  CLDB_ASSERT_OK(cldb_writable_file_append(
      writable, cldb_slice_make(data, kWriteSize)));
  cldb_writable_file_destroy(writable);
  writable = NULL;

  CLDB_EXPECT_OK(cldb_env_get_file_size(env, "/dir/f", &file_size));
  CLDB_EXPECT_EQ_UINT(kWriteSize, file_size);
  CLDB_ASSERT_OK(cldb_read_file_to_string(env, "/dir/f", &read_back));
  CLDB_EXPECT_EQ_UINT(kWriteSize, read_back.size);
  if (read_back.size == kWriteSize) {
    CLDB_EXPECT_EQ_INT(0, memcmp(read_back.data, data, kWriteSize));
  }

  CLDB_ASSERT_OK(cldb_env_new_appendable_file(env, "/dir/f", &writable));
  CLDB_ASSERT_OK(
      cldb_writable_file_append(writable, cldb_slice_make("foo", 3)));
  cldb_writable_file_destroy(writable);
  writable = NULL;

  CLDB_EXPECT_OK(cldb_env_get_file_size(env, "/dir/f", &file_size));
  CLDB_EXPECT_EQ_UINT(kWriteSize + 3, file_size);
  CLDB_ASSERT_OK(
      cldb_env_new_random_access_file(env, "/dir/f", &random));
  CLDB_ASSERT_OK(cldb_random_access_file_read(
      random, (uint64_t)kWriteSize, 3, &result, scratch));
  CLDB_EXPECT_TRUE(cldb_slice_equal(cldb_slice_make("foo", 3), result));

  cldb_random_access_file_destroy(random);
  cldb_buf_free(&read_back);
  free(data);
  cldb_env_destroy(env);
}

CLDB_TEST(memenv_overwrite_open_file) {
  cldb_env_t *env = new_mem_env();
  cldb_random_access_file_t *random = NULL;
  cldb_slice_t result;
  char scratch[128];

  write_text(env, "/dir/f", "abc");
  CLDB_ASSERT_OK(
      cldb_env_new_random_access_file(env, "/dir/f", &random));

  /* 截断并重写同一 FileState；已打开的随机读文件应看到新内容。 */
  write_text(env, "/dir/f", "0123456789");
  CLDB_ASSERT_OK(cldb_random_access_file_read(random, 0, 100, &result,
                                              scratch));
  CLDB_EXPECT_TRUE(
      cldb_slice_equal(cldb_slice_make("0123456789", 10), result));

  cldb_random_access_file_destroy(random);
  cldb_env_destroy(env);
}

CLDB_TEST(memenv_open_handles_keep_state_alive) {
  cldb_env_t *env = new_mem_env();
  cldb_writable_file_t *writable = NULL;
  cldb_random_access_file_t *random = NULL;
  cldb_slice_t result;
  char scratch[16];
  uint64_t file_size = 0;

  CLDB_ASSERT_OK(cldb_env_new_writable_file(env, "/gone", &writable));
  CLDB_ASSERT_OK(
      cldb_writable_file_append(writable, cldb_slice_make("a", 1)));
  CLDB_ASSERT_OK(
      cldb_env_new_random_access_file(env, "/gone", &random));

  /* 删除只移除文件表引用；打开的对象继续持有 FileState。 */
  CLDB_EXPECT_OK(cldb_env_remove_file(env, "/gone"));
  CLDB_EXPECT_FALSE(cldb_env_file_exists(env, "/gone"));
  CLDB_ASSERT_OK(
      cldb_writable_file_append(writable, cldb_slice_make("b", 1)));
  CLDB_ASSERT_OK(cldb_random_access_file_read(random, 0, 16, &result,
                                              scratch));
  CLDB_EXPECT_TRUE(cldb_slice_equal(cldb_slice_make("ab", 2), result));
  CLDB_EXPECT_STATUS(CLDB_STATUS_NOT_FOUND,
                     cldb_env_get_file_size(env, "/gone", &file_size));

  cldb_writable_file_destroy(writable);
  cldb_random_access_file_destroy(random);
  cldb_env_destroy(env);
}

typedef struct memenv_reader_task {
  cldb_random_access_file_t *file; /* 仅引用 */
  const char *data;                /* 仅引用 */
  size_t size;
  unsigned int seed;
  bool failed;
} memenv_reader_task_t;

static void *reader_main(void *arg) {
  enum { kReadSize = 64 };
  memenv_reader_task_t *task = (memenv_reader_task_t *)arg;
  char scratch[kReadSize];
  size_t i;

  for (i = 0; i < 2000; ++i) {
    uint64_t offset =
        ((uint64_t)task->seed * 131 + (uint64_t)i * 17) %
        (uint64_t)(task->size - kReadSize);
    cldb_slice_t result;
    cldb_status_t status = cldb_random_access_file_read(
        task->file, offset, kReadSize, &result, scratch);

    if (!cldb_status_is_ok(status) || result.size != kReadSize ||
        memcmp(result.data, task->data + (size_t)offset, kReadSize) != 0) {
      task->failed = true;
      break;
    }
  }
  return NULL;
}

CLDB_TEST(memenv_concurrent_random_reads) {
  enum { kDataSize = 64 * 1024, kThreads = 8 };
  cldb_env_t *env = new_mem_env();
  cldb_random_access_file_t *random = NULL;
  pthread_t threads[kThreads];
  memenv_reader_task_t tasks[kThreads];
  char *data = malloc(kDataSize);
  size_t i;

  CLDB_ASSERT_TRUE(data != NULL);
  fill_pattern(data, kDataSize);
  CLDB_ASSERT_OK(cldb_write_string_to_file(
      env, cldb_slice_make(data, kDataSize), "/dir/f"));
  CLDB_ASSERT_OK(
      cldb_env_new_random_access_file(env, "/dir/f", &random));

  for (i = 0; i < kThreads; ++i) {
    tasks[i].file = random;
    tasks[i].data = data;
    tasks[i].size = kDataSize;
    tasks[i].seed = (unsigned int)i;
    tasks[i].failed = false;
    CLDB_ASSERT_EQ_INT(0, pthread_create(&threads[i], NULL, reader_main,
                                         &tasks[i]));
  }
  for (i = 0; i < kThreads; ++i) {
    CLDB_ASSERT_EQ_INT(0, pthread_join(threads[i], NULL));
    CLDB_EXPECT_FALSE(tasks[i].failed);
  }

  cldb_random_access_file_destroy(random);
  free(data);
  cldb_env_destroy(env);
}

CLDB_TEST(memenv_logger_modes) {
  cldb_env_t *env = new_mem_env();
  cldb_logger_t *logger = NULL;
  cldb_buf_t log_data;
  cldb_memenv_options_t options;

  cldb_buf_init(&log_data);
  CLDB_ASSERT_OK(cldb_env_new_logger(env, "/log/info", &logger));
  cldb_log(logger, "hello %d", 42);
  cldb_logger_destroy(logger);
  logger = NULL;

  CLDB_ASSERT_OK(cldb_read_file_to_string(env, "/log/info", &log_data));
  CLDB_ASSERT_TRUE(log_data.size > 0);
  CLDB_EXPECT_TRUE(strstr(cldb_buf_cstr(&log_data), "hello 42") != NULL);
  CLDB_EXPECT_TRUE(log_data.size > strlen("hello 42"));
  CLDB_EXPECT_TRUE(log_data.data[log_data.size - 1] == '\n');
  cldb_buf_free(&log_data);
  cldb_env_destroy(env);

  /* stderr 模式只输出到标准错误，不在文件表创建条目。 */
  cldb_memenv_options_init(&options);
  options.log_target = CLDB_MEMENV_LOG_STDERR;
  env = cldb_memenv_create_with_options(NULL, &options);
  CLDB_ASSERT_TRUE(env != NULL);
  CLDB_ASSERT_OK(cldb_env_new_logger(env, "/stderr.log", &logger));
  cldb_log(logger, "memenv stderr logger test");
  cldb_logger_destroy(logger);
  CLDB_EXPECT_FALSE(cldb_env_file_exists(env, "/stderr.log"));
  cldb_env_destroy(env);

  options.log_target = (cldb_memenv_log_target_t)99;
  CLDB_EXPECT_EQ_PTR(NULL,
                     cldb_memenv_create_with_options(NULL, &options));
}

CLDB_TEST(memenv_time_schedule_and_thread) {
  cldb_env_t *env = new_mem_env();
  cldb_atomic_u32_t scheduled;
  cldb_atomic_u32_t threaded;
  uint64_t before;
  uint64_t after;
  int i;

  cldb_atomic_u32_init(&scheduled, 0);
  cldb_atomic_u32_init(&threaded, 0);

  before = cldb_env_now_micros(env);
  cldb_env_sleep_for_microseconds(env, 2000);
  after = cldb_env_now_micros(env);
  CLDB_EXPECT_TRUE(after >= before);
  if (after >= before) {
    CLDB_EXPECT_TRUE(after - before >= 1000);
  }

  cldb_env_schedule(env, mark_flag, &scheduled);
  for (i = 0; i < 5000 &&
              cldb_atomic_u32_load(&scheduled, memory_order_acquire) == 0;
       ++i) {
    cldb_env_sleep_for_microseconds(env, 1000);
  }
  CLDB_EXPECT_EQ_UINT(1,
                      cldb_atomic_u32_load(&scheduled, memory_order_acquire));

  cldb_env_start_thread(env, mark_flag, &threaded);
  for (i = 0; i < 5000 &&
              cldb_atomic_u32_load(&threaded, memory_order_acquire) == 0;
       ++i) {
    cldb_env_sleep_for_microseconds(env, 1000);
  }
  CLDB_EXPECT_EQ_UINT(1,
                      cldb_atomic_u32_load(&threaded, memory_order_acquire));

  cldb_env_destroy(env);
}

CLDB_TEST(memenv_invalid_arguments) {
  cldb_env_t *env = new_mem_env();
  cldb_sequential_file_t *sequential = NULL;
  cldb_random_access_file_t *random = NULL;
  cldb_writable_file_t *writable = NULL;
  cldb_logger_t *logger = NULL;
  cldb_file_lock_t *lock = NULL;
  cldb_strvec_t children;
  cldb_buf_t test_dir;
  uint64_t size = 0;

  cldb_strvec_init(&children);
  cldb_buf_init(&test_dir);

  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_env_new_sequential_file(env, "/x", NULL));
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_env_new_sequential_file(env, NULL, &sequential));
  CLDB_EXPECT_EQ_PTR(NULL, sequential);
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_env_new_random_access_file(env, NULL, &random));
  CLDB_EXPECT_EQ_PTR(NULL, random);
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_env_new_writable_file(env, NULL, &writable));
  CLDB_EXPECT_EQ_PTR(NULL, writable);
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_env_new_appendable_file(env, NULL, &writable));
  CLDB_EXPECT_EQ_PTR(NULL, writable);

  CLDB_EXPECT_FALSE(cldb_env_file_exists(env, NULL));
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_env_get_file_size(env, NULL, &size));
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_env_get_file_size(env, "/x", NULL));
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_env_get_children(env, NULL, &children));
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_env_get_children(env, "/x", NULL));
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_env_remove_file(env, NULL));
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_env_create_dir(env, NULL));
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_env_remove_dir(env, NULL));
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_env_rename_file(env, NULL, "/x"));
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_env_rename_file(env, "/x", NULL));
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_env_lock_file(env, NULL, &lock));
  CLDB_EXPECT_EQ_PTR(NULL, lock);
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_env_lock_file(env, "/x", NULL));
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_env_unlock_file(env, NULL));
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_env_get_test_directory(env, NULL));
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_env_new_logger(env, NULL, &logger));
  CLDB_EXPECT_EQ_PTR(NULL, logger);
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_env_new_logger(env, "/log", NULL));

  cldb_buf_free(&test_dir);
  cldb_strvec_free(&children);
  cldb_env_destroy(env);
}

void cldb_register_memenv_tests(void) {
  CLDB_RUN_TEST(memenv_basics);
  CLDB_RUN_TEST(memenv_read_write);
  CLDB_RUN_TEST(memenv_locks);
  CLDB_RUN_TEST(memenv_misc);
  CLDB_RUN_TEST(memenv_children_follow_path_prefix);
  CLDB_RUN_TEST(memenv_large_write);
  CLDB_RUN_TEST(memenv_overwrite_open_file);
  CLDB_RUN_TEST(memenv_open_handles_keep_state_alive);
  CLDB_RUN_TEST(memenv_concurrent_random_reads);
  CLDB_RUN_TEST(memenv_logger_modes);
  CLDB_RUN_TEST(memenv_time_schedule_and_thread);
  CLDB_RUN_TEST(memenv_invalid_arguments);
}
