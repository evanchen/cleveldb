/*
 * T001 占位测试：验证测试程序可构建、链接并运行。
 * T002 替换为正式测试框架。
 */

#include <stdio.h>
#include <string.h>

#include "port/platform.h"

int main(void) {
  const char *platform = cldb_platform_name();

  if (platform == NULL || strcmp(platform, "linux") != 0) {
    fprintf(stderr, "cleveldb: unexpected platform: %s\n",
            platform != NULL ? platform : "(null)");
    return 1;
  }

  printf("cleveldb skeleton OK (%s)\n", platform);
  return 0;
}
