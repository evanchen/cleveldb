#include "harness.h"

void cldb_register_harness_tests(void);
void cldb_register_example_tests(void);
void cldb_register_slice_tests(void);
void cldb_register_buf_tests(void);

/* 测试入口；新增套件时在此注册。 */
int main(int argc, char **argv) {
  cldb_register_harness_tests();
  cldb_register_example_tests();
  cldb_register_slice_tests();
  cldb_register_buf_tests();
  return cldb_test_run(argc, argv);
}
