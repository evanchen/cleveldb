#include "cleveldb/options.h"

#include "cleveldb/comparator.h"
#include "cleveldb/env.h"

void cldb_options_init(cldb_options_t *options) {
  if (options == NULL) {
    return;
  }
  options->comparator = cldb_bytewise_comparator();
  options->env = cldb_env_default();
  options->info_log = NULL;
  options->write_buffer_size = 4u * 1024u * 1024u;
  options->max_open_files = 1000;
  options->block_cache = NULL;
  options->block_size = 4u * 1024u;
  options->block_restart_interval = 16;
  options->max_file_size = 2u * 1024u * 1024u;
  options->compression = CLDB_COMPRESSION_NONE;
  options->zstd_compression_level = 1;
  options->reuse_logs = false;
  options->filter_policy = NULL;
  options->paranoid_checks = false;
  options->create_if_missing = false;
  options->error_if_exists = false;
}

void cldb_read_options_init(cldb_read_options_t *options) {
  if (options == NULL) {
    return;
  }
  options->verify_checksums = false;
  options->fill_cache = true;
  options->snapshot = NULL;
}

void cldb_write_options_init(cldb_write_options_t *options) {
  if (options == NULL) {
    return;
  }
  options->sync = false;
}
