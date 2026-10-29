#include <stdint.h>
#include <string.h>

#include "db/filename.h"
#include "harness.h"
#include "helpers/memenv/memenv.h"
#include "status_test_util.h"

/* 文件名生成/解析与 CURRENT 原子替换；对应上游 db/filename_test.cc。 */

typedef bool (*numbered_name_fn_t)(const char *, uint64_t, cldb_buf_t *);
typedef bool (*fixed_name_fn_t)(const char *, cldb_buf_t *);

static void expect_file_text(cldb_env_t *env, const char *path,
                             const char *expected) {
  cldb_buf_t data;
  cldb_status_t status;
  size_t expected_size = strlen(expected);

  cldb_buf_init(&data);
  status = cldb_read_file_to_string(env, path, &data);
  CLDB_EXPECT_OK(status);
  if (cldb_status_is_ok(status)) {
    CLDB_EXPECT_EQ_UINT(expected_size, data.size);
    if (data.size == expected_size) {
      CLDB_EXPECT_EQ_INT(0, memcmp(data.data, expected, expected_size));
    }
  }
  cldb_buf_free(&data);
}

/* 生成完整路径、检查文本，再用 basename 反解类型和编号。 */
static void expect_numbered_name(numbered_name_fn_t function,
                                 const char *dbname, uint64_t number,
                                 cldb_file_type_t expected_type,
                                 const char *expected) {
  cldb_buf_t path;
  const char *basename;
  uint64_t parsed_number = 0;
  cldb_file_type_t parsed_type = CLDB_FILE_TYPE_LOG;

  cldb_buf_init(&path);
  if (!function(dbname, number, &path)) {
    CLDB_EXPECT_TRUE(false);
    cldb_buf_free(&path);
    return;
  }
  CLDB_EXPECT_EQ_STR(expected, cldb_buf_cstr(&path));
  basename = strrchr(cldb_buf_cstr(&path), '/');
  if (basename == NULL) {
    CLDB_EXPECT_TRUE(false);
    cldb_buf_free(&path);
    return;
  }
  ++basename;
  CLDB_EXPECT_TRUE(cldb_parse_file_name(basename, &parsed_number,
                                        &parsed_type));
  CLDB_EXPECT_EQ_UINT(number, parsed_number);
  CLDB_EXPECT_EQ_INT(expected_type, parsed_type);
  cldb_buf_free(&path);
}

static void expect_fixed_name(fixed_name_fn_t function, const char *dbname,
                              cldb_file_type_t expected_type,
                              const char *expected) {
  cldb_buf_t path;
  const char *basename;
  uint64_t parsed_number = 1;
  cldb_file_type_t parsed_type = CLDB_FILE_TYPE_LOG;

  cldb_buf_init(&path);
  if (!function(dbname, &path)) {
    CLDB_EXPECT_TRUE(false);
    cldb_buf_free(&path);
    return;
  }
  CLDB_EXPECT_EQ_STR(expected, cldb_buf_cstr(&path));
  basename = strrchr(cldb_buf_cstr(&path), '/');
  if (basename == NULL) {
    CLDB_EXPECT_TRUE(false);
    cldb_buf_free(&path);
    return;
  }
  ++basename;
  CLDB_EXPECT_TRUE(cldb_parse_file_name(basename, &parsed_number,
                                        &parsed_type));
  CLDB_EXPECT_EQ_UINT(0, parsed_number);
  CLDB_EXPECT_EQ_INT(expected_type, parsed_type);
  cldb_buf_free(&path);
}

CLDB_TEST(filename_parse) {
  static const struct {
    const char *name;
    uint64_t number;
    cldb_file_type_t type;
  } cases[] = {
      {"100.log", 100, CLDB_FILE_TYPE_LOG},
      {"0.log", 0, CLDB_FILE_TYPE_LOG},
      {"0.sst", 0, CLDB_FILE_TYPE_TABLE},
      {"0.ldb", 0, CLDB_FILE_TYPE_TABLE},
      {"0.dbtmp", 0, CLDB_FILE_TYPE_TEMP},
      {"CURRENT", 0, CLDB_FILE_TYPE_CURRENT},
      {"LOCK", 0, CLDB_FILE_TYPE_DB_LOCK},
      {"MANIFEST-2", 2, CLDB_FILE_TYPE_DESCRIPTOR},
      {"MANIFEST-7", 7, CLDB_FILE_TYPE_DESCRIPTOR},
      {"MANIFEST-0", 0, CLDB_FILE_TYPE_DESCRIPTOR},
      {"LOG", 0, CLDB_FILE_TYPE_INFO_LOG},
      {"LOG.old", 0, CLDB_FILE_TYPE_INFO_LOG},
      {"000001.log", 1, CLDB_FILE_TYPE_LOG},
      {"18446744073709551615.log", UINT64_MAX, CLDB_FILE_TYPE_LOG},
      {"18446744073709551615.sst", UINT64_MAX, CLDB_FILE_TYPE_TABLE},
  };
  static const char *errors[] = {
      "",
      "foo",
      "foo-dx-100.log",
      ".log",
      "manifest",
      "CURREN",
      "CURRENTX",
      "MANIFES",
      "MANIFEST",
      "MANIFEST-",
      "XMANIFEST-3",
      "MANIFEST-3x",
      "MANIFEST-1/2",
      "LOC",
      "LOCKx",
      "LO",
      "LOGx",
      "LOG.oldx",
      "LOG.old.old",
      "18446744073709551616.log",
      "184467440737095516150.log",
      "100",
      "100.",
      "100.lop",
      "100.log.old",
      "100.LOG",
      " 100.log",
      "100.log ",
      "/100.log",
      "dir/100.log",
      "100.log/",
      "dir/MANIFEST-1",
      "..",
      ".",
  };
  size_t i;

  for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    uint64_t number = UINT64_MAX;
    cldb_file_type_t type = CLDB_FILE_TYPE_TEMP;

    CLDB_EXPECT_TRUE(cldb_parse_file_name(cases[i].name, &number, &type));
    CLDB_EXPECT_EQ_UINT(cases[i].number, number);
    CLDB_EXPECT_EQ_INT(cases[i].type, type);
  }
  for (i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
    uint64_t number = 42;
    cldb_file_type_t type = CLDB_FILE_TYPE_TABLE;

    CLDB_EXPECT_FALSE(cldb_parse_file_name(errors[i], &number, &type));
    CLDB_EXPECT_EQ_UINT(42, number);
    CLDB_EXPECT_EQ_INT(CLDB_FILE_TYPE_TABLE, type);
  }
}

CLDB_TEST(filename_construction) {
  expect_numbered_name(cldb_log_file_name, "foo", 192,
                       CLDB_FILE_TYPE_LOG, "foo/000192.log");
  expect_numbered_name(cldb_table_file_name, "bar", 200,
                       CLDB_FILE_TYPE_TABLE, "bar/000200.ldb");
  expect_numbered_name(cldb_sst_table_file_name, "bar", 200,
                       CLDB_FILE_TYPE_TABLE, "bar/000200.sst");
  expect_numbered_name(cldb_descriptor_file_name, "bar", 100,
                       CLDB_FILE_TYPE_DESCRIPTOR, "bar/MANIFEST-000100");
  expect_numbered_name(cldb_temp_file_name, "tmp", 999,
                       CLDB_FILE_TYPE_TEMP, "tmp/000999.dbtmp");

  expect_fixed_name(cldb_current_file_name, "foo", CLDB_FILE_TYPE_CURRENT,
                    "foo/CURRENT");
  expect_fixed_name(cldb_lock_file_name, "foo", CLDB_FILE_TYPE_DB_LOCK,
                    "foo/LOCK");
  expect_fixed_name(cldb_info_log_file_name, "foo",
                    CLDB_FILE_TYPE_INFO_LOG, "foo/LOG");
  expect_fixed_name(cldb_old_info_log_file_name, "foo",
                    CLDB_FILE_TYPE_INFO_LOG, "foo/LOG.old");
}

CLDB_TEST(filename_number_boundaries) {
  expect_numbered_name(cldb_log_file_name, "db", 0, CLDB_FILE_TYPE_LOG,
                       "db/000000.log");
  expect_numbered_name(cldb_log_file_name, "db", 999999,
                       CLDB_FILE_TYPE_LOG, "db/999999.log");
  expect_numbered_name(cldb_log_file_name, "db", 1000000,
                       CLDB_FILE_TYPE_LOG, "db/1000000.log");
  expect_numbered_name(cldb_log_file_name, "db", UINT64_MAX,
                       CLDB_FILE_TYPE_LOG,
                       "db/18446744073709551615.log");
  expect_numbered_name(cldb_descriptor_file_name, "db", UINT64_MAX,
                       CLDB_FILE_TYPE_DESCRIPTOR,
                       "db/MANIFEST-18446744073709551615");
}

CLDB_TEST(filename_invalid_arguments) {
  cldb_buf_t path;
  uint64_t number = 7;
  cldb_file_type_t type = CLDB_FILE_TYPE_LOG;

  cldb_buf_init(&path);
  CLDB_EXPECT_TRUE(cldb_buf_append(&path, "keep", 4));

  CLDB_EXPECT_FALSE(cldb_log_file_name(NULL, 1, &path));
  CLDB_EXPECT_EQ_STR("keep", cldb_buf_cstr(&path));
  CLDB_EXPECT_FALSE(cldb_log_file_name("db", 1, NULL));
  CLDB_EXPECT_FALSE(cldb_descriptor_file_name(NULL, 1, &path));
  CLDB_EXPECT_EQ_STR("keep", cldb_buf_cstr(&path));
  CLDB_EXPECT_FALSE(cldb_current_file_name(NULL, &path));
  CLDB_EXPECT_EQ_STR("keep", cldb_buf_cstr(&path));

  CLDB_EXPECT_FALSE(cldb_parse_file_name(NULL, &number, &type));
  CLDB_EXPECT_EQ_UINT(7, number);
  CLDB_EXPECT_EQ_INT(CLDB_FILE_TYPE_LOG, type);
  CLDB_EXPECT_FALSE(cldb_parse_file_name("100.log", NULL, &type));
  CLDB_EXPECT_EQ_INT(CLDB_FILE_TYPE_LOG, type);
  CLDB_EXPECT_FALSE(cldb_parse_file_name("100.log", &number, NULL));
  CLDB_EXPECT_EQ_UINT(7, number);

  cldb_buf_free(&path);
}

/* 包装 MemEnv，只让 RenameFile 失败，用于验证临时文件清理。 */
typedef struct fail_rename_env {
  cldb_env_wrapper_t wrapper;
  cldb_env_vtbl_t vtbl;
} fail_rename_env_t;

static cldb_status_t injected_rename_failure(cldb_env_t *env,
                                             const char *src,
                                             const char *target) {
  (void)env;
  (void)src;
  (void)target;
  return cldb_status_io("RenameFile", "injected failure");
}

static void init_fail_rename_env(fail_rename_env_t *failing,
                                 cldb_env_t *target) {
  cldb_env_wrapper_init(&failing->wrapper, target);
  failing->vtbl = *failing->wrapper.base.vtbl;
  failing->vtbl.rename_file = injected_rename_failure;
  failing->wrapper.base.vtbl = &failing->vtbl;
}

CLDB_TEST(filename_set_current_file) {
  cldb_env_t *env = cldb_memenv_create(NULL);

  CLDB_EXPECT_TRUE(env != NULL);
  if (env != NULL) {
    CLDB_EXPECT_OK(cldb_set_current_file(env, "db", 3));
    expect_file_text(env, "db/CURRENT", "MANIFEST-000003\n");
    CLDB_EXPECT_FALSE(cldb_env_file_exists(env, "db/000003.dbtmp"));

    CLDB_EXPECT_OK(cldb_set_current_file(env, "db", 12));
    expect_file_text(env, "db/CURRENT", "MANIFEST-000012\n");
    CLDB_EXPECT_FALSE(cldb_env_file_exists(env, "db/000012.dbtmp"));
    CLDB_EXPECT_FALSE(cldb_env_file_exists(env, "db/000003.dbtmp"));

    CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                       cldb_set_current_file(NULL, "db", 1));
    CLDB_EXPECT_STATUS(CLDB_STATUS_INVALID_ARGUMENT,
                       cldb_set_current_file(env, NULL, 1));
    cldb_env_destroy(env);
  }
}

CLDB_TEST(filename_set_current_file_rename_failure) {
  cldb_env_t *env = cldb_memenv_create(NULL);
  fail_rename_env_t failing;
  cldb_status_t status;

  CLDB_EXPECT_TRUE(env != NULL);
  if (env == NULL) {
    return;
  }
  init_fail_rename_env(&failing, env);
  status = cldb_set_current_file(&failing.wrapper.base, "db", 7);
  CLDB_EXPECT_STATUS(CLDB_STATUS_IO_ERROR, status);
  CLDB_EXPECT_FALSE(cldb_env_file_exists(env, "db/000007.dbtmp"));
  CLDB_EXPECT_FALSE(cldb_env_file_exists(env, "db/CURRENT"));
  cldb_env_destroy(env);
}

void cldb_register_filename_tests(void) {
  CLDB_RUN_TEST(filename_parse);
  CLDB_RUN_TEST(filename_construction);
  CLDB_RUN_TEST(filename_number_boundaries);
  CLDB_RUN_TEST(filename_invalid_arguments);
  CLDB_RUN_TEST(filename_set_current_file);
  CLDB_RUN_TEST(filename_set_current_file_rename_failure);
}
