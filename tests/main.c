#include "harness.h"

void cldb_register_harness_tests(void);
void cldb_register_example_tests(void);
void cldb_register_slice_tests(void);
void cldb_register_buf_tests(void);
void cldb_register_status_tests(void);
void cldb_register_coding_tests(void);
void cldb_register_hash_tests(void);
void cldb_register_random_tests(void);
void cldb_register_arena_tests(void);
void cldb_register_crc32c_tests(void);
void cldb_register_port_tests(void);
void cldb_register_atomic_tests(void);

/* 测试入口；新增套件时在此注册。 */
int main(int argc, char **argv) {
  cldb_register_harness_tests();
  cldb_register_example_tests();
  cldb_register_slice_tests();
  cldb_register_buf_tests();
  cldb_register_status_tests();
  cldb_register_coding_tests();
  cldb_register_hash_tests();
  cldb_register_random_tests();
  cldb_register_arena_tests();
  cldb_register_crc32c_tests();
  cldb_register_port_tests();
  cldb_register_atomic_tests();
  return cldb_test_run(argc, argv);
}
