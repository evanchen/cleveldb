#include <string.h>

#include "cleveldb/comparator.h"
#include "cleveldb/env.h"
#include "cleveldb/options.h"
#include "harness.h"

/* 选项默认值；对应上游 Options/ReadOptions/WriteOptions 的构造函数。 */

CLDB_TEST(options_defaults) {
  cldb_options_t options;
  cldb_options_t again;

  memset(&options, 0x7f, sizeof(options));
  cldb_options_init(&options);
  CLDB_EXPECT_EQ_PTR(cldb_bytewise_comparator(), options.comparator);
  CLDB_EXPECT_EQ_PTR(cldb_env_default(), options.env);
  CLDB_EXPECT_EQ_PTR(NULL, options.info_log);
  CLDB_EXPECT_EQ_UINT(4u * 1024u * 1024u, options.write_buffer_size);
  CLDB_EXPECT_EQ_INT(1000, options.max_open_files);
  CLDB_EXPECT_EQ_PTR(NULL, options.block_cache);
  CLDB_EXPECT_EQ_UINT(4u * 1024u, options.block_size);
  CLDB_EXPECT_EQ_INT(16, options.block_restart_interval);
  CLDB_EXPECT_EQ_UINT(2u * 1024u * 1024u, options.max_file_size);
  CLDB_EXPECT_EQ_INT(CLDB_COMPRESSION_NONE, options.compression);
  CLDB_EXPECT_EQ_INT(1, options.zstd_compression_level);
  CLDB_EXPECT_FALSE(options.reuse_logs);
  CLDB_EXPECT_EQ_PTR(NULL, options.filter_policy);
  CLDB_EXPECT_FALSE(options.paranoid_checks);
  CLDB_EXPECT_FALSE(options.create_if_missing);
  CLDB_EXPECT_FALSE(options.error_if_exists);

  /* 重复初始化应恢复全部默认值，不累积旧状态。 */
  options.write_buffer_size = 1;
  options.max_open_files = -1;
  options.compression = CLDB_COMPRESSION_ZSTD;
  options.reuse_logs = true;
  options.create_if_missing = true;
  cldb_options_init(&options);
  CLDB_EXPECT_EQ_UINT(4u * 1024u * 1024u, options.write_buffer_size);
  CLDB_EXPECT_EQ_INT(1000, options.max_open_files);
  CLDB_EXPECT_EQ_INT(CLDB_COMPRESSION_NONE, options.compression);
  CLDB_EXPECT_FALSE(options.reuse_logs);
  CLDB_EXPECT_FALSE(options.create_if_missing);

  again = options;
  cldb_options_init(&again);
  CLDB_EXPECT_EQ_PTR(options.comparator, again.comparator);
  CLDB_EXPECT_EQ_PTR(options.env, again.env);
  CLDB_EXPECT_EQ_UINT(options.write_buffer_size, again.write_buffer_size);
  cldb_options_init(NULL);
}

CLDB_TEST(options_read_write_defaults) {
  cldb_read_options_t read_options;
  cldb_write_options_t write_options;

  memset(&read_options, 0xff, sizeof(read_options));
  cldb_read_options_init(&read_options);
  CLDB_EXPECT_FALSE(read_options.verify_checksums);
  CLDB_EXPECT_TRUE(read_options.fill_cache);
  CLDB_EXPECT_EQ_PTR(NULL, read_options.snapshot);

  read_options.verify_checksums = true;
  read_options.fill_cache = false;
  cldb_read_options_init(&read_options);
  CLDB_EXPECT_FALSE(read_options.verify_checksums);
  CLDB_EXPECT_TRUE(read_options.fill_cache);
  cldb_read_options_init(NULL);

  memset(&write_options, 0xff, sizeof(write_options));
  cldb_write_options_init(&write_options);
  CLDB_EXPECT_FALSE(write_options.sync);
  write_options.sync = true;
  cldb_write_options_init(&write_options);
  CLDB_EXPECT_FALSE(write_options.sync);
  cldb_write_options_init(NULL);
}

CLDB_TEST(options_compression_values) {
  /* 数值写入 SSTable，必须与上游枚举一致。 */
  CLDB_EXPECT_EQ_INT(0x0, CLDB_COMPRESSION_NONE);
  CLDB_EXPECT_EQ_INT(0x1, CLDB_COMPRESSION_SNAPPY);
  CLDB_EXPECT_EQ_INT(0x2, CLDB_COMPRESSION_ZSTD);
}

void cldb_register_options_tests(void) {
  CLDB_RUN_TEST(options_defaults);
  CLDB_RUN_TEST(options_read_write_defaults);
  CLDB_RUN_TEST(options_compression_values);
}
