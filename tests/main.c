/*
 * 测试入口：显式注册各测试套件。
 * 新增套件时在下方声明并调用 cldb_register_xxx_tests。
 */

#include "harness.h"

void cldb_register_harness_tests(void);

int main(int argc, char **argv) {
  cldb_register_harness_tests();
  return cldb_test_run(argc, argv);
}
