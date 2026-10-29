#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "cleveldb/env.h"
#include "harness.h"
#include "port/port_mutex.h"
#include "port/port_thread.h"
#include "status_test_util.h"
#include "temp_dir.h"

/* 默认 Env 的真实文件系统测试；对应上游 util/env_test.cc 与
 * util/env_posix_test.cc 的 Linux/POSIX 分支。 */

static bool join_path(cldb_buf_t *out, const char *dir, const char *name) {
  cldb_buf_clear(out);
  return cldb_buf_append(out, dir, strlen(dir)) &&
         cldb_buf_append(out, "/", 1) &&
         cldb_buf_append(out, name, strlen(name));
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

static void fill_pattern(char *data, size_t size) {
  size_t i;

  for (i = 0; i < size; ++i) {
    data[i] = (char)((i * 131 + 17) % 251);
  }
}

/* 时间戳形如 YYYY/MM/DD-HH:MM:SS.uuuuuu + 线程号 + 空格。 */
static bool has_timestamp_prefix(const char *line) {
  static const size_t kSeparatorPositions[] = {4, 7, 10, 13, 16, 19, 26};
  static const char kSeparators[] = {'/', '/', '-', ':', ':', '.', ' '};
  size_t separator = 0;
  size_t i;

  for (i = 0; i < 27; ++i) {
    if (separator < sizeof(kSeparators) &&
        i == kSeparatorPositions[separator]) {
      if (line[i] != kSeparators[separator]) {
        return false;
      }
      separator++;
    } else if (line[i] < '0' || line[i] > '9') {
      return false;
    }
  }
  return true;
}

static uint64_t elapsed_nanos(struct timespec start, struct timespec end) {
  uint64_t seconds = (uint64_t)(end.tv_sec - start.tv_sec);
  int64_t nanoseconds = (int64_t)end.tv_nsec - (int64_t)start.tv_nsec;

  if (nanoseconds < 0) {
    --seconds;
    nanoseconds += INT64_C(1000000000);
  }
  return seconds * UINT64_C(1000000000) + (uint64_t)nanoseconds;
}

CLDB_TEST(env_posix_default_and_time) {
  cldb_env_t *env = cldb_env_default();
  struct timespec start;
  struct timespec end;
  uint64_t before;
  uint64_t after;

  CLDB_ASSERT_TRUE(env != NULL);
  CLDB_EXPECT_EQ_PTR(env, cldb_env_default());

  before = cldb_env_now_micros(env);
  CLDB_EXPECT_TRUE(before > 0);
  cldb_env_sleep_for_microseconds(env, 0);
  cldb_env_sleep_for_microseconds(env, -1);

  CLDB_ASSERT_EQ_INT(0, clock_gettime(CLOCK_MONOTONIC, &start));
  cldb_env_sleep_for_microseconds(env, 5000);
  CLDB_ASSERT_EQ_INT(0, clock_gettime(CLOCK_MONOTONIC, &end));
  CLDB_EXPECT_TRUE(elapsed_nanos(start, end) >= UINT64_C(3000000));

  after = cldb_env_now_micros(env);
  CLDB_EXPECT_TRUE(after > 0);
  /* NowMicros 来自 gettimeofday，不保证单调，只验证它在前述睡眠后已推进。 */
  CLDB_EXPECT_TRUE(after >= before);
}

CLDB_TEST(env_posix_test_directory) {
  cldb_env_t *env = cldb_env_default();
  cldb_buf_t path;
  cldb_buf_t again;
  cldb_buf_t override_path;
  cldb_buf_t saved_tmpdir;
  const char *old_tmpdir = getenv("TEST_TMPDIR");
  bool had_old_tmpdir = old_tmpdir != NULL;
  struct stat file_stat;

  cldb_buf_init(&path);
  cldb_buf_init(&again);
  cldb_buf_init(&override_path);
  cldb_buf_init(&saved_tmpdir);
  if (had_old_tmpdir) {
    CLDB_ASSERT_TRUE(cldb_buf_assign(&saved_tmpdir, old_tmpdir,
                                     strlen(old_tmpdir)));
  }

  CLDB_ASSERT_OK(cldb_env_get_test_directory(env, &path));
  CLDB_EXPECT_TRUE(path.size > 0);
  CLDB_ASSERT_EQ_INT(0, stat(cldb_buf_cstr(&path), &file_stat));
  CLDB_EXPECT_TRUE(S_ISDIR(file_stat.st_mode));
  CLDB_ASSERT_OK(cldb_env_get_test_directory(env, &again));
  CLDB_EXPECT_EQ_STR(cldb_buf_cstr(&path), cldb_buf_cstr(&again));

  /* TEST_TMPDIR 优先于默认目录，且目录会被创建。 */
  CLDB_ASSERT_TRUE(join_path(&override_path, cldb_buf_cstr(&path),
                             "tmpdir-override"));
  CLDB_ASSERT_EQ_INT(0, setenv("TEST_TMPDIR", cldb_buf_cstr(&override_path),
                               1));
  CLDB_EXPECT_OK(cldb_env_get_test_directory(env, &again));
  CLDB_EXPECT_EQ_STR(cldb_buf_cstr(&override_path), cldb_buf_cstr(&again));
  CLDB_EXPECT_EQ_INT(0, stat(cldb_buf_cstr(&override_path), &file_stat));
  CLDB_EXPECT_TRUE(S_ISDIR(file_stat.st_mode));

  if (had_old_tmpdir) {
    CLDB_EXPECT_EQ_INT(0, setenv("TEST_TMPDIR", cldb_buf_cstr(&saved_tmpdir),
                                 1));
  } else {
    CLDB_EXPECT_EQ_INT(0, unsetenv("TEST_TMPDIR"));
  }
  CLDB_EXPECT_OK(cldb_env_remove_dir(env, cldb_buf_cstr(&override_path)));

  cldb_buf_free(&saved_tmpdir);
  cldb_buf_free(&override_path);
  cldb_buf_free(&again);
  cldb_buf_free(&path);
}

CLDB_TEST(env_posix_temp_dir_unique_and_cleanup) {
  cldb_env_t *env = cldb_env_default();
  cldb_test_temp_dir_t first;
  cldb_test_temp_dir_t second;
  cldb_buf_t first_path;
  cldb_buf_t second_path;
  cldb_buf_t nested_path;
  cldb_buf_t nested_file;

  cldb_buf_init(&first_path);
  cldb_buf_init(&second_path);
  cldb_buf_init(&nested_path);
  cldb_buf_init(&nested_file);
  CLDB_ASSERT_OK(cldb_test_temp_dir_create(env, "unique", &first));
  CLDB_ASSERT_OK(cldb_test_temp_dir_create(env, "unique", &second));
  CLDB_ASSERT_TRUE(cldb_buf_assign(&first_path,
                                   cldb_test_temp_dir_path(&first),
                                   strlen(cldb_test_temp_dir_path(&first))));
  CLDB_ASSERT_TRUE(cldb_buf_assign(&second_path,
                                   cldb_test_temp_dir_path(&second),
                                   strlen(cldb_test_temp_dir_path(&second))));
  CLDB_EXPECT_TRUE(strcmp(cldb_buf_cstr(&first_path),
                          cldb_buf_cstr(&second_path)) != 0);
  CLDB_EXPECT_TRUE(cldb_env_file_exists(env, cldb_buf_cstr(&first_path)));
  CLDB_EXPECT_TRUE(cldb_env_file_exists(env, cldb_buf_cstr(&second_path)));

  /* 在第一个目录下构造嵌套内容，destroy 必须递归删除。 */
  CLDB_ASSERT_TRUE(join_path(&nested_path, cldb_buf_cstr(&first_path),
                             "nested"));
  CLDB_ASSERT_OK(cldb_env_create_dir(env, cldb_buf_cstr(&nested_path)));
  CLDB_ASSERT_TRUE(join_path(&nested_file, cldb_buf_cstr(&nested_path),
                             "file"));
  CLDB_ASSERT_OK(cldb_write_string_to_file(
      env, cldb_slice_make("x", 1), cldb_buf_cstr(&nested_file)));

  cldb_test_temp_dir_destroy(&first);
  CLDB_EXPECT_FALSE(cldb_env_file_exists(env, cldb_buf_cstr(&first_path)));
  CLDB_EXPECT_TRUE(cldb_env_file_exists(env, cldb_buf_cstr(&second_path)));
  cldb_test_temp_dir_destroy(&second);
  CLDB_EXPECT_FALSE(cldb_env_file_exists(env, cldb_buf_cstr(&second_path)));

  cldb_buf_free(&nested_file);
  cldb_buf_free(&nested_path);
  cldb_buf_free(&second_path);
  cldb_buf_free(&first_path);
}

CLDB_TEST(env_posix_read_write) {
  static const size_t kWriteChunks[] = {1,    2,    100,  65535, 65536,
                                        65537, 4096, 17,   50000};
  static const size_t kReadChunks[] = {1, 7, 8192, 3, 4096, 65535, 2};
  enum { kDataSize = 300000 };
  cldb_env_t *env = cldb_env_default();
  cldb_test_temp_dir_t dir;
  cldb_buf_t file_path;
  cldb_buf_t read_back;
  cldb_writable_file_t *writable = NULL;
  cldb_sequential_file_t *sequential = NULL;
  cldb_random_access_file_t *random = NULL;
  char *data = malloc(kDataSize);
  char scratch[65536]; /* 必须不小于单次读取上限 65535。 */
  cldb_slice_t fragment;
  uint64_t file_size = 0;
  size_t written = 0;
  size_t chunk = 0;
  size_t read_offset = 0;
  size_t read_chunk = 0;
  bool read_done = false;

  CLDB_ASSERT_TRUE(data != NULL);
  fill_pattern(data, kDataSize);
  cldb_buf_init(&file_path);
  cldb_buf_init(&read_back);
  CLDB_ASSERT_OK(cldb_test_temp_dir_create(env, "read-write", &dir));
  CLDB_ASSERT_TRUE(join_path(&file_path, cldb_test_temp_dir_path(&dir),
                             "data.txt"));

  CLDB_ASSERT_OK(cldb_env_new_writable_file(env, cldb_buf_cstr(&file_path),
                                            &writable));
  while (written < kDataSize) {
    size_t amount = kWriteChunks[chunk % (sizeof(kWriteChunks) /
                                          sizeof(kWriteChunks[0]))];

    if (amount > kDataSize - written) {
      amount = kDataSize - written;
    }
    CLDB_ASSERT_OK(cldb_writable_file_append(
        writable, cldb_slice_make(data + written, amount)));
    written += amount;
    chunk++;
    if (chunk % 3 == 0) {
      CLDB_ASSERT_OK(cldb_writable_file_flush(writable));
    }
  }
  CLDB_ASSERT_OK(cldb_writable_file_sync(writable));
  CLDB_ASSERT_OK(cldb_writable_file_close(writable));
  cldb_writable_file_destroy(writable);
  writable = NULL;

  CLDB_ASSERT_OK(cldb_env_get_file_size(env, cldb_buf_cstr(&file_path),
                                        &file_size));
  CLDB_EXPECT_EQ_UINT(kDataSize, file_size);

  /* 顺序读：先验证 ReadFileToString，再用大小不一的读拼回原文。 */
  CLDB_ASSERT_OK(cldb_read_file_to_string(env, cldb_buf_cstr(&file_path),
                                          &read_back));
  CLDB_EXPECT_EQ_UINT(kDataSize, read_back.size);
  CLDB_EXPECT_EQ_INT(0, memcmp(read_back.data, data, kDataSize));

  CLDB_ASSERT_OK(cldb_env_new_sequential_file(
      env, cldb_buf_cstr(&file_path), &sequential));
  while (!read_done) {
    size_t amount = kReadChunks[read_chunk % (sizeof(kReadChunks) /
                                              sizeof(kReadChunks[0]))];

    if (amount > kDataSize - read_offset) {
      amount = kDataSize - read_offset;
    }
    CLDB_ASSERT_OK(
        cldb_sequential_file_read(sequential, amount, &fragment, scratch));
    if (fragment.size == 0) {
      read_done = true;
      break;
    }
    CLDB_ASSERT_TRUE(fragment.size <= amount);
    CLDB_EXPECT_EQ_INT(0, memcmp(fragment.data, data + read_offset,
                                 fragment.size));
    read_offset += fragment.size;
    read_chunk++;
  }
  CLDB_EXPECT_EQ_UINT(kDataSize, read_offset);
  cldb_sequential_file_destroy(sequential);
  sequential = NULL;

  /* Skip 后继续读，偏移必须与数据一致。 */
  CLDB_ASSERT_OK(cldb_env_new_sequential_file(
      env, cldb_buf_cstr(&file_path), &sequential));
  CLDB_ASSERT_OK(cldb_sequential_file_read(sequential, 100, &fragment,
                                           scratch));
  CLDB_EXPECT_EQ_UINT(100, fragment.size);
  CLDB_EXPECT_EQ_INT(0, memcmp(fragment.data, data, 100));
  CLDB_ASSERT_OK(cldb_sequential_file_skip(sequential, 12345));
  CLDB_ASSERT_OK(cldb_sequential_file_read(sequential, 100, &fragment,
                                           scratch));
  CLDB_EXPECT_EQ_UINT(100, fragment.size);
  CLDB_EXPECT_EQ_INT(0, memcmp(fragment.data, data + 100 + 12345, 100));
  cldb_sequential_file_destroy(sequential);
  sequential = NULL;

  /* 随机读：offset 与长度原样生效，越过 EOF 返回空。 */
  CLDB_ASSERT_OK(cldb_env_new_random_access_file(
      env, cldb_buf_cstr(&file_path), &random));
  CLDB_ASSERT_OK(cldb_random_access_file_read(random, 0, 100, &fragment,
                                              scratch));
  CLDB_EXPECT_EQ_UINT(100, fragment.size);
  CLDB_EXPECT_EQ_INT(0, memcmp(fragment.data, data, 100));
  CLDB_ASSERT_OK(cldb_random_access_file_read(random, 12345, 17, &fragment,
                                              scratch));
  CLDB_EXPECT_EQ_UINT(17, fragment.size);
  CLDB_EXPECT_EQ_INT(0, memcmp(fragment.data, data + 12345, 17));
  CLDB_ASSERT_OK(cldb_random_access_file_read(
      random, (uint64_t)kDataSize - 10, 100, &fragment, scratch));
  CLDB_EXPECT_EQ_UINT(10, fragment.size);
  CLDB_EXPECT_EQ_INT(0, memcmp(fragment.data, data + kDataSize - 10, 10));
  CLDB_ASSERT_OK(cldb_random_access_file_read(
      random, (uint64_t)kDataSize + 5, 100, &fragment, scratch));
  CLDB_EXPECT_EQ_UINT(0, fragment.size);
  cldb_random_access_file_destroy(random);
  random = NULL;

  cldb_test_temp_dir_destroy(&dir);
  cldb_buf_free(&read_back);
  cldb_buf_free(&file_path);
  free(data);
}

/* 并发随机读的共享状态；每线程只写自己的 ok，join 后由主线程检查。 */
typedef struct concurrent_read_state {
  cldb_random_access_file_t *file; /* borrowed */
  const char *data;                /* borrowed；期望内容 */
  size_t data_size;                /* data 与文件长度 */
  int index;                       /* 线程序号，用于生成不同偏移 */
  bool ok;                         /* 所有随机读结果正确 */
} concurrent_read_state_t;

/* 每线程独立 scratch；pread 不共享文件偏移，可并发调用。 */
static void *concurrent_read_main(void *arg) {
  enum { kScratchSize = 4096, kReadsPerThread = 200 };
  concurrent_read_state_t *state = arg;
  char scratch[kScratchSize];
  uint32_t seed = (uint32_t)state->index * UINT32_C(2654435761) + 1;
  int i;

  state->ok = true;
  for (i = 0; i < kReadsPerThread; ++i) {
    cldb_slice_t fragment;
    size_t offset;
    size_t size;

    seed = seed * UINT32_C(1103515245) + UINT32_C(12345);
    offset = (size_t)(seed % (uint32_t)(state->data_size - kScratchSize));
    size = 1 + (size_t)((seed >> 16) % kScratchSize);
    if (!cldb_status_is_ok(cldb_random_access_file_read(
            state->file, (uint64_t)offset, size, &fragment, scratch)) ||
        fragment.size != size ||
        memcmp(fragment.data, state->data + offset, size) != 0) {
      state->ok = false;
      return NULL;
    }
  }
  return NULL;
}

CLDB_TEST(env_posix_random_read_threads) {
  enum { kDataSize = 256 * 1024, kThreads = 8 };
  cldb_env_t *env = cldb_env_default();
  cldb_test_temp_dir_t dir;
  cldb_buf_t file_path;
  cldb_writable_file_t *writable = NULL;
  cldb_random_access_file_t *random = NULL;
  cldb_thread_t threads[kThreads];
  concurrent_read_state_t states[kThreads];
  char *data = malloc(kDataSize);
  int i;

  CLDB_ASSERT_TRUE(data != NULL);
  fill_pattern(data, kDataSize);
  cldb_buf_init(&file_path);
  CLDB_ASSERT_OK(cldb_test_temp_dir_create(env, "random-read", &dir));
  CLDB_ASSERT_TRUE(join_path(&file_path, cldb_test_temp_dir_path(&dir),
                             "data.bin"));
  CLDB_ASSERT_OK(cldb_env_new_writable_file(env, cldb_buf_cstr(&file_path),
                                            &writable));
  CLDB_ASSERT_OK(cldb_writable_file_append(
      writable, cldb_slice_make(data, kDataSize)));
  CLDB_ASSERT_OK(cldb_writable_file_close(writable));
  cldb_writable_file_destroy(writable);
  writable = NULL;
  CLDB_ASSERT_OK(cldb_env_new_random_access_file(
      env, cldb_buf_cstr(&file_path), &random));

  for (i = 0; i < kThreads; ++i) {
    states[i].file = random;
    states[i].data = data;
    states[i].data_size = kDataSize;
    states[i].index = i;
    states[i].ok = false;
    cldb_thread_init(&threads[i]);
    CLDB_ASSERT_OK(
        cldb_thread_start(&threads[i], concurrent_read_main, &states[i]));
  }
  for (i = 0; i < kThreads; ++i) {
    CLDB_ASSERT_OK(cldb_thread_join(&threads[i], NULL));
    CLDB_EXPECT_TRUE(states[i].ok);
  }

  cldb_random_access_file_destroy(random);
  cldb_test_temp_dir_destroy(&dir);
  cldb_buf_free(&file_path);
  free(data);
}

CLDB_TEST(env_posix_appendable_and_truncate) {
  cldb_env_t *env = cldb_env_default();
  cldb_test_temp_dir_t dir;
  cldb_buf_t file_path;
  cldb_buf_t content;
  cldb_writable_file_t *file = NULL;

  cldb_buf_init(&file_path);
  cldb_buf_init(&content);
  CLDB_ASSERT_OK(cldb_test_temp_dir_create(env, "append", &dir));
  CLDB_ASSERT_TRUE(join_path(&file_path, cldb_test_temp_dir_path(&dir),
                             "append.txt"));

  /* NewWritableFile 重开时截断。 */
  CLDB_ASSERT_OK(cldb_env_new_writable_file(env, cldb_buf_cstr(&file_path),
                                            &file));
  CLDB_ASSERT_OK(cldb_writable_file_append(
      file, cldb_slice_make("hello world!", 12)));
  CLDB_ASSERT_OK(cldb_writable_file_close(file));
  cldb_writable_file_destroy(file);
  file = NULL;

  CLDB_ASSERT_OK(cldb_env_new_writable_file(env, cldb_buf_cstr(&file_path),
                                            &file));
  CLDB_ASSERT_OK(cldb_writable_file_append(file, cldb_slice_make("42", 2)));
  CLDB_ASSERT_OK(cldb_writable_file_close(file));
  cldb_writable_file_destroy(file);
  file = NULL;
  CLDB_ASSERT_OK(cldb_read_file_to_string(env, cldb_buf_cstr(&file_path),
                                          &content));
  CLDB_EXPECT_EQ_STR("42", cldb_buf_cstr(&content));

  /* NewAppendableFile 保留已有内容，追加到末尾。 */
  CLDB_EXPECT_OK(cldb_env_remove_file(env, cldb_buf_cstr(&file_path)));
  CLDB_EXPECT_FALSE(cldb_env_file_exists(env, cldb_buf_cstr(&file_path)));
  CLDB_ASSERT_OK(cldb_env_new_appendable_file(
      env, cldb_buf_cstr(&file_path), &file));
  CLDB_ASSERT_OK(cldb_writable_file_append(
      file, cldb_slice_make("hello world!", 12)));
  CLDB_ASSERT_OK(cldb_writable_file_close(file));
  cldb_writable_file_destroy(file);
  file = NULL;

  CLDB_ASSERT_OK(cldb_env_new_appendable_file(
      env, cldb_buf_cstr(&file_path), &file));
  CLDB_ASSERT_OK(cldb_writable_file_append(file, cldb_slice_make("42", 2)));
  CLDB_ASSERT_OK(cldb_writable_file_close(file));
  cldb_writable_file_destroy(file);
  file = NULL;
  CLDB_ASSERT_OK(cldb_read_file_to_string(env, cldb_buf_cstr(&file_path),
                                          &content));
  CLDB_EXPECT_EQ_STR("hello world!42", cldb_buf_cstr(&content));

  cldb_test_temp_dir_destroy(&dir);
  cldb_buf_free(&content);
  cldb_buf_free(&file_path);
}

CLDB_TEST(env_posix_dirs_and_children) {
  cldb_env_t *env = cldb_env_default();
  cldb_test_temp_dir_t dir;
  cldb_buf_t file_path;
  cldb_buf_t renamed_path;
  cldb_buf_t sub_path;
  cldb_buf_t inner_path;
  cldb_strvec_t children;
  cldb_sequential_file_t *sequential = NULL;
  cldb_random_access_file_t *random = NULL;
  uint64_t size = 0;
  size_t i;
  bool saw_dot = false;
  bool saw_dotdot = false;

  cldb_buf_init(&file_path);
  cldb_buf_init(&renamed_path);
  cldb_buf_init(&sub_path);
  cldb_buf_init(&inner_path);
  cldb_strvec_init(&children);
  CLDB_ASSERT_OK(cldb_test_temp_dir_create(env, "dirs", &dir));
  CLDB_ASSERT_TRUE(join_path(&file_path, cldb_test_temp_dir_path(&dir),
                             "file.txt"));
  CLDB_ASSERT_TRUE(join_path(&renamed_path, cldb_test_temp_dir_path(&dir),
                             "renamed.txt"));
  CLDB_ASSERT_TRUE(join_path(&sub_path, cldb_test_temp_dir_path(&dir),
                             "sub"));
  CLDB_ASSERT_TRUE(join_path(&inner_path, cldb_buf_cstr(&sub_path),
                             "inner.txt"));

  /* 上游 stock Env 保留 "." 与 ".."；新目录恰好两个条目。 */
  CLDB_ASSERT_OK(cldb_env_get_children(env, cldb_test_temp_dir_path(&dir),
                                       &children));
  CLDB_EXPECT_EQ_UINT(2, cldb_strvec_size(&children));
  for (i = 0; i < cldb_strvec_size(&children); ++i) {
    const char *name = cldb_strvec_get(&children, i);

    if (strcmp(name, ".") == 0) {
      saw_dot = true;
    } else if (strcmp(name, "..") == 0) {
      saw_dotdot = true;
    }
  }
  CLDB_EXPECT_TRUE(saw_dot);
  CLDB_EXPECT_TRUE(saw_dotdot);

  CLDB_EXPECT_FALSE(cldb_env_file_exists(env, cldb_buf_cstr(&file_path)));
  CLDB_ASSERT_OK(cldb_write_string_to_file_sync(
      env, cldb_slice_make("abc", 3), cldb_buf_cstr(&file_path)));
  CLDB_EXPECT_TRUE(cldb_env_file_exists(env, cldb_buf_cstr(&file_path)));
  CLDB_EXPECT_OK(cldb_env_get_file_size(env, cldb_buf_cstr(&file_path),
                                        &size));
  CLDB_EXPECT_EQ_UINT(3, size);

  CLDB_EXPECT_OK(cldb_env_get_children(env, cldb_test_temp_dir_path(&dir),
                                       &children));
  CLDB_EXPECT_EQ_UINT(3, cldb_strvec_size(&children));
  CLDB_EXPECT_TRUE(strvec_contains(&children, "file.txt"));

  CLDB_ASSERT_OK(cldb_env_create_dir(env, cldb_buf_cstr(&sub_path)));
  CLDB_EXPECT_OK(cldb_env_get_children(env, cldb_test_temp_dir_path(&dir),
                                       &children));
  CLDB_EXPECT_EQ_UINT(4, cldb_strvec_size(&children));
  CLDB_EXPECT_TRUE(strvec_contains(&children, "sub"));

  CLDB_EXPECT_OK(cldb_env_rename_file(env, cldb_buf_cstr(&file_path),
                                      cldb_buf_cstr(&renamed_path)));
  CLDB_EXPECT_FALSE(cldb_env_file_exists(env, cldb_buf_cstr(&file_path)));
  CLDB_EXPECT_TRUE(cldb_env_file_exists(env, cldb_buf_cstr(&renamed_path)));

  CLDB_EXPECT_STATUS(CLDB_STATUS_NOT_FOUND,
                     cldb_env_remove_file(env, cldb_buf_cstr(&file_path)));
  CLDB_EXPECT_OK(cldb_env_remove_file(env, cldb_buf_cstr(&renamed_path)));
  CLDB_EXPECT_STATUS(CLDB_STATUS_NOT_FOUND,
                     cldb_env_remove_file(env, cldb_buf_cstr(&renamed_path)));

  /* 非空目录不能删除；清空后删除成功，别名接口同样可用。 */
  CLDB_ASSERT_OK(cldb_write_string_to_file(
      env, cldb_slice_make("x", 1), cldb_buf_cstr(&inner_path)));
  CLDB_EXPECT_STATUS(CLDB_STATUS_IO_ERROR,
                     cldb_env_remove_dir(env, cldb_buf_cstr(&sub_path)));
  CLDB_EXPECT_OK(cldb_env_delete_file(env, cldb_buf_cstr(&inner_path)));
  CLDB_EXPECT_OK(cldb_env_delete_dir(env, cldb_buf_cstr(&sub_path)));
  CLDB_EXPECT_STATUS(CLDB_STATUS_NOT_FOUND,
                     cldb_env_remove_dir(env, cldb_buf_cstr(&sub_path)));
  CLDB_EXPECT_STATUS(CLDB_STATUS_IO_ERROR,
                     cldb_env_create_dir(env, cldb_test_temp_dir_path(&dir)));

  /* 不存在的文件与目录。 */
  CLDB_EXPECT_STATUS(CLDB_STATUS_NOT_FOUND,
                     cldb_env_get_children(env, "/no/such/dir", &children));
  CLDB_EXPECT_STATUS(CLDB_STATUS_NOT_FOUND,
                     cldb_env_new_sequential_file(env, "/no/such/file",
                                                  &sequential));
  CLDB_EXPECT_EQ_PTR(NULL, sequential);
  CLDB_EXPECT_STATUS(CLDB_STATUS_NOT_FOUND,
                     cldb_env_new_random_access_file(env, "/no/such/file",
                                                     &random));
  CLDB_EXPECT_EQ_PTR(NULL, random);
  size = 99;
  CLDB_EXPECT_STATUS(CLDB_STATUS_NOT_FOUND,
                     cldb_env_get_file_size(env, "/no/such/file", &size));
  CLDB_EXPECT_EQ_UINT(0, size);

  cldb_test_temp_dir_destroy(&dir);
  cldb_strvec_free(&children);
  cldb_buf_free(&inner_path);
  cldb_buf_free(&sub_path);
  cldb_buf_free(&renamed_path);
  cldb_buf_free(&file_path);
}

/* 子进程在父进程持锁期间尝试加锁；退出码 0 表示按预期得到锁冲突。 */
static void lock_child_main(int read_fd, const char *path) {
  cldb_file_lock_t *lock = NULL;
  cldb_status_t status;
  char byte;

  if (read(read_fd, &byte, 1) != 1) {
    _exit(10);
  }
  status = cldb_env_lock_file(cldb_env_default(), path, &lock);
  if (cldb_status_is_ok(status)) {
    (void)cldb_env_unlock_file(cldb_env_default(), lock);
    _exit(11);
  }
  /* 必须是锁冲突（IOError），且失败时不返回锁对象。 */
  if (!cldb_status_is_io_error(status) || lock != NULL) {
    _exit(12);
  }
  _exit(0);
}

CLDB_TEST(env_posix_lock) {
  cldb_env_t *env = cldb_env_default();
  cldb_test_temp_dir_t dir;
  cldb_buf_t lock_path;
  cldb_file_lock_t *lock = NULL;
  cldb_file_lock_t *second = NULL;
  int pipe_fds[2];
  pid_t child;
  int child_status = 0;
  char signal_byte = 'x';

  cldb_buf_init(&lock_path);
  CLDB_ASSERT_OK(cldb_test_temp_dir_create(env, "lock", &dir));
  CLDB_ASSERT_TRUE(join_path(&lock_path, cldb_test_temp_dir_path(&dir),
                             "LOCK"));

  /* fork 必须在父进程加锁前完成，否则子进程会继承进程内锁表。 */
  CLDB_ASSERT_EQ_INT(0, pipe(pipe_fds));
  child = fork();
  CLDB_ASSERT_TRUE(child >= 0);
  if (child == 0) {
    (void)close(pipe_fds[1]);
    lock_child_main(pipe_fds[0], cldb_buf_cstr(&lock_path));
  }
  (void)close(pipe_fds[0]);

  CLDB_ASSERT_OK(cldb_env_lock_file(env, cldb_buf_cstr(&lock_path), &lock));
  CLDB_ASSERT_TRUE(lock != NULL);
  CLDB_EXPECT_EQ_INT(1, (int)write(pipe_fds[1], &signal_byte, 1));
  (void)close(pipe_fds[1]);
  CLDB_ASSERT_EQ_INT(child, waitpid(child, &child_status, 0));
  CLDB_EXPECT_TRUE(WIFEXITED(child_status));
  CLDB_EXPECT_EQ_INT(0, WEXITSTATUS(child_status));
  CLDB_ASSERT_OK(cldb_env_unlock_file(env, lock));
  lock = NULL;

  /* 同进程重复加锁由进程内锁表拒绝。 */
  CLDB_ASSERT_OK(cldb_env_lock_file(env, cldb_buf_cstr(&lock_path), &lock));
  CLDB_EXPECT_STATUS(CLDB_STATUS_IO_ERROR,
                     cldb_env_lock_file(env, cldb_buf_cstr(&lock_path),
                                        &second));
  CLDB_EXPECT_EQ_PTR(NULL, second);
  CLDB_ASSERT_OK(cldb_env_unlock_file(env, lock));
  lock = NULL;

  /* 解锁后可重新加锁。 */
  CLDB_ASSERT_OK(cldb_env_lock_file(env, cldb_buf_cstr(&lock_path), &lock));
  CLDB_ASSERT_OK(cldb_env_unlock_file(env, lock));
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_env_unlock_file(env, NULL));

  cldb_test_temp_dir_destroy(&dir);
  cldb_buf_free(&lock_path);
}

static void *log_thread_main(void *arg) {
  cldb_logger_t *logger = arg;
  int i;

  for (i = 0; i < 32; ++i) {
    cldb_log(logger, "thread-log");
  }
  return NULL;
}

CLDB_TEST(env_posix_logger) {
  enum { kLongMessageSize = 4096, kThreads = 4, kLinesPerThread = 32 };
  cldb_env_t *env = cldb_env_default();
  cldb_test_temp_dir_t dir;
  cldb_buf_t log_path;
  cldb_buf_t content;
  cldb_logger_t *logger = NULL;
  cldb_thread_t threads[kThreads];
  char *long_message = malloc(kLongMessageSize + 1);
  size_t expected_newlines = 3 + kThreads * kLinesPerThread;
  size_t newlines = 0;
  size_t i;
  const char *text;

  CLDB_ASSERT_TRUE(long_message != NULL);
  memset(long_message, 'x', kLongMessageSize);
  long_message[kLongMessageSize] = '\0';
  cldb_buf_init(&log_path);
  cldb_buf_init(&content);
  CLDB_ASSERT_OK(cldb_test_temp_dir_create(env, "logger", &dir));
  CLDB_ASSERT_TRUE(join_path(&log_path, cldb_test_temp_dir_path(&dir),
                             "LOG"));

  CLDB_ASSERT_OK(cldb_env_new_logger(env, cldb_buf_cstr(&log_path), &logger));
  CLDB_ASSERT_TRUE(logger != NULL);
  cldb_log(logger, "hello %d", 7);
  cldb_log(logger, "%s", long_message);
  cldb_log(logger, "with newline\n");

  for (i = 0; i < kThreads; ++i) {
    cldb_thread_init(&threads[i]);
    CLDB_ASSERT_OK(cldb_thread_start(&threads[i], log_thread_main, logger));
  }
  for (i = 0; i < kThreads; ++i) {
    CLDB_ASSERT_OK(cldb_thread_join(&threads[i], NULL));
  }
  cldb_logger_destroy(logger);
  logger = NULL;

  CLDB_ASSERT_OK(cldb_read_file_to_string(env, cldb_buf_cstr(&log_path),
                                          &content));
  CLDB_ASSERT_TRUE(content.size > 0);
  text = cldb_buf_cstr(&content);
  CLDB_EXPECT_TRUE(has_timestamp_prefix(text));
  CLDB_EXPECT_TRUE(strstr(text, "hello 7\n") != NULL);
  CLDB_EXPECT_TRUE(strstr(text, long_message) != NULL);
  CLDB_EXPECT_TRUE(strstr(text, "with newline\n") != NULL);
  for (i = 0; i < content.size; ++i) {
    if (content.data[i] == '\n') {
      newlines++;
    }
  }
  CLDB_EXPECT_EQ_UINT(expected_newlines, newlines);

  cldb_test_temp_dir_destroy(&dir);
  cldb_buf_free(&content);
  cldb_buf_free(&log_path);
  free(long_message);
}

typedef struct schedule_state {
  cldb_mutex_t mutex;
  cldb_condvar_t cv;
  int expected;     /* 下一个应执行的任务序号 */
  int executed;     /* 已完成任务数 */
  int in_flight;    /* 当前执行中的任务数 */
  int max_in_flight;/* 观测到的最大并发度 */
  bool order_ok;    /* 执行顺序是否与提交顺序一致 */
} schedule_state_t;

typedef struct schedule_task {
  schedule_state_t *state; /* borrowed */
  int index;               /* 提交序号 */
} schedule_task_t;

static void schedule_callback(void *arg) {
  schedule_task_t *task = arg;
  schedule_state_t *state = task->state;

  (void)cldb_mutex_lock(&state->mutex);
  state->in_flight++;
  if (state->in_flight > state->max_in_flight) {
    state->max_in_flight = state->in_flight;
  }
  if (task->index != state->expected) {
    state->order_ok = false;
  }
  state->expected++;
  state->executed++;
  state->in_flight--;
  (void)cldb_condvar_signal(&state->cv);
  (void)cldb_mutex_unlock(&state->mutex);
}

CLDB_TEST(env_posix_schedule) {
  enum { kTasks = 1000 };
  cldb_env_t *env = cldb_env_default();
  schedule_state_t state;
  schedule_task_t tasks[kTasks];
  schedule_task_t extra_task;
  int i;

  memset(&state, 0, sizeof(state));
  state.order_ok = true;
  CLDB_ASSERT_OK(cldb_mutex_init(&state.mutex));
  CLDB_ASSERT_OK(cldb_condvar_init(&state.cv, &state.mutex));

  for (i = 0; i < kTasks; ++i) {
    tasks[i].state = &state;
    tasks[i].index = i;
    cldb_env_schedule(env, schedule_callback, &tasks[i]);
  }
  CLDB_ASSERT_OK(cldb_mutex_lock(&state.mutex));
  while (state.executed < kTasks) {
    CLDB_ASSERT_OK(cldb_condvar_wait(&state.cv));
  }
  CLDB_EXPECT_EQ_INT(kTasks, state.executed);
  CLDB_EXPECT_EQ_INT(1, state.max_in_flight);
  CLDB_EXPECT_TRUE(state.order_ok);
  CLDB_ASSERT_OK(cldb_mutex_unlock(&state.mutex));

  /* 队列空闲后再次调度，后台线程仍会被唤醒。 */
  cldb_env_sleep_for_microseconds(env, 20000);
  extra_task.state = &state;
  extra_task.index = kTasks;
  cldb_env_schedule(env, schedule_callback, &extra_task);
  CLDB_ASSERT_OK(cldb_mutex_lock(&state.mutex));
  while (state.executed < kTasks + 1) {
    CLDB_ASSERT_OK(cldb_condvar_wait(&state.cv));
  }
  CLDB_EXPECT_EQ_INT(kTasks + 1, state.executed);
  CLDB_EXPECT_TRUE(state.order_ok);
  CLDB_ASSERT_OK(cldb_mutex_unlock(&state.mutex));

  CLDB_ASSERT_OK(cldb_condvar_destroy(&state.cv));
  CLDB_ASSERT_OK(cldb_mutex_destroy(&state.mutex));
}

typedef struct start_thread_state {
  cldb_mutex_t mutex;
  cldb_condvar_t cv;
  int remaining;
  int value;
} start_thread_state_t;

static void start_thread_body(void *arg) {
  start_thread_state_t *state = arg;

  (void)cldb_mutex_lock(&state->mutex);
  state->value++;
  state->remaining--;
  (void)cldb_condvar_signal(&state->cv);
  (void)cldb_mutex_unlock(&state->mutex);
}

CLDB_TEST(env_posix_start_thread) {
  cldb_env_t *env = cldb_env_default();
  start_thread_state_t state;
  int i;

  memset(&state, 0, sizeof(state));
  state.remaining = 3;
  CLDB_ASSERT_OK(cldb_mutex_init(&state.mutex));
  CLDB_ASSERT_OK(cldb_condvar_init(&state.cv, &state.mutex));

  for (i = 0; i < 3; ++i) {
    cldb_env_start_thread(env, start_thread_body, &state);
  }
  CLDB_ASSERT_OK(cldb_mutex_lock(&state.mutex));
  while (state.remaining != 0) {
    CLDB_ASSERT_OK(cldb_condvar_wait(&state.cv));
  }
  CLDB_EXPECT_EQ_INT(3, state.value);
  CLDB_ASSERT_OK(cldb_mutex_unlock(&state.mutex));

  CLDB_ASSERT_OK(cldb_condvar_destroy(&state.cv));
  CLDB_ASSERT_OK(cldb_mutex_destroy(&state.mutex));
}

CLDB_TEST(env_posix_invalid_arguments) {
  cldb_env_t *env = cldb_env_default();
  cldb_sequential_file_t *sequential = NULL;
  cldb_logger_t *logger = NULL;
  cldb_file_lock_t *lock = NULL;
  cldb_strvec_t children;
  cldb_buf_t path;
  uint64_t size = 0;

  cldb_strvec_init(&children);
  cldb_buf_init(&path);
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_env_new_sequential_file(env, NULL, &sequential));
  CLDB_EXPECT_EQ_PTR(NULL, sequential);
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_env_new_writable_file(env, "x", NULL));
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_env_new_logger(env, NULL, &logger));
  CLDB_EXPECT_EQ_PTR(NULL, logger);
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_env_get_children(env, NULL, &children));
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_env_get_test_directory(env, NULL));
  CLDB_EXPECT_FALSE(cldb_env_file_exists(env, NULL));
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_env_remove_file(env, NULL));
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_env_get_file_size(env, NULL, &size));
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_env_rename_file(env, NULL, "dst"));
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_env_lock_file(env, NULL, &lock));
  CLDB_EXPECT_EQ_PTR(NULL, lock);
  CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                     cldb_env_unlock_file(env, NULL));
  cldb_buf_free(&path);
  cldb_strvec_free(&children);
}

void cldb_register_env_posix_tests(void) {
  CLDB_RUN_TEST(env_posix_default_and_time);
  CLDB_RUN_TEST(env_posix_test_directory);
  CLDB_RUN_TEST(env_posix_temp_dir_unique_and_cleanup);
  CLDB_RUN_TEST(env_posix_read_write);
  CLDB_RUN_TEST(env_posix_random_read_threads);
  CLDB_RUN_TEST(env_posix_appendable_and_truncate);
  CLDB_RUN_TEST(env_posix_dirs_and_children);
  CLDB_RUN_TEST(env_posix_lock);
  CLDB_RUN_TEST(env_posix_logger);
  CLDB_RUN_TEST(env_posix_schedule);
  CLDB_RUN_TEST(env_posix_start_thread);
  CLDB_RUN_TEST(env_posix_invalid_arguments);
}
