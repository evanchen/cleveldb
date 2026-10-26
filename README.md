# cleveldb

LevelDB 的 C11 学习性移植，**仅支持 Linux**。

目标是把上游 LevelDB（C++）按模块移植为 C 实现，保持磁盘格式和公开语义一致，并通过阅读上游源码 + 逐任务实现来吃透 LSM-Tree 的原理。

- 架构、格式规范、C 设计：[doc/plan.md](doc/plan.md)
- 任务分解与验收：[doc/tasks.md](doc/tasks.md)
- C 编码规范：[doc/coding-style.md](doc/coding-style.md)
- 所有权规则：[doc/ownership.md](doc/ownership.md)
- 上游基线：`E:\github\leveldb`，`1.23-93-gbb74ef7`（commit `bb74ef7`）

> 当前状态：T102 Status。数据库功能尚未实现。

## 平台与依赖

- 仅支持 Linux（GCC/Clang + pthread + POSIX.1-2008）。
- 不支持 Windows/macOS。`Makefile` 入口和 `src/port/platform.h` 编译期都会明确报错。
- 默认构建不依赖任何第三方库；库和测试只链接 libc 与 pthread。
- 可选压缩（Snappy/Zstd）是后续 T704 的 Make 变量，默认关闭。

## Debian 构建

依赖：GCC 或 Clang、GNU Make、pthread。

```sh
sudo apt install build-essential
# 可选：sudo apt install clang

make -j"$(nproc)"
make test
make check
```

默认 Debug 构建，产物在 `build/`：`libcleveldb.a` 与 `cleveldb_tests`。

Release 与 Clang 使用独立构建目录，避免对象文件混用：

```sh
make BUILD=release BUILD_DIR=build-release -j"$(nproc)"
make BUILD=release BUILD_DIR=build-release test

make CC=clang BUILD_DIR=build-clang -j"$(nproc)"
make CC=clang BUILD_DIR=build-clang test
```

ASan/UBSan：

```sh
make asan
```

常用目标：

| 目标 | 说明 |
|---|---|
| `all` | 构建静态库和测试（默认） |
| `lib` | 只构建 `build/libcleveldb.a` |
| `tests` | 只构建 `build/cleveldb_tests` |
| `tools` | 构建 `tools/*.c` |
| `test` | 运行完整测试套件 |
| `check` | 运行全套回归（过滤/列表/重复/失败路径） |
| `asan` | 在 `build-asan` 下用 ASan/UBSan 跑 `check` |
| `install` | 安装库和公开头文件 |
| `clean` | 删除当前 `BUILD_DIR` |
| `clean-all` | 删除 `build`、`build-asan`、`build-release`、`build-clang` |

常用变量：

| 变量 | 默认 | 说明 |
|---|---|---|
| `BUILD` | `debug` | `debug` 或 `release` |
| `BUILD_DIR` | `build` | 构建目录 |
| `CC` | `cc` | C 编译器 |
| `WERROR` | `0` | `1` 时把编译警告当作错误 |
| `SANITIZE` | `0` | `1` 时启用 ASan/UBSan |
| `PREFIX` | `/usr/local` | `install` 安装前缀 |
| `DESTDIR` | 空 | `install` 暂存目录 |

`make help` 列出全部目标与变量。

## 目录约定

```text
include/cleveldb/   公开 C API（唯一对外安装的头文件目录）
src/port/           Linux/POSIX 平台层（pthread、文件、时间、原子）
src/util/           基础工具（编码、CRC、哈希、Arena、Cache、Bloom 等）
src/db/             WAL、memtable、version、DB 核心
src/table/          SSTable 读写与迭代器
src/helpers/memenv/ 内存 Env（测试用）
tests/              单元测试、差分测试、兼容测试
tools/              cldb_dump、cldb_bench
examples/           公开 API 示例
doc/                计划、任务与规范
Makefile            构建入口（GNU Make）
```

内部头文件放在 `src/` 下，不对外安装；`#include "port/..."` 等路径由库的私有 include 目录解析。

## 每个任务的验证流程

1. 只实现一个任务，保持改动独立可审阅。
2. 在 Debian 上按上面的命令构建，运行本任务相关测试。
3. 运行 `make asan`，用 ASan/UBSan 再跑一遍测试。
4. 审阅本任务的 `git diff`/新增代码，对照上游实现理解所有权、锁和错误路径。
5. 回答任务里的“吃透检查点”，再进入下一个任务。
