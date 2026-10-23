/*
 * 上游：无（仅构建/平台骨架）。
 * 职责：提供平台名，供占位测试验证库可链接。
 * 锁：无。所有权：返回静态字符串。
 */

#include "port/platform.h"

const char *cldb_platform_name(void) { return "linux"; }
