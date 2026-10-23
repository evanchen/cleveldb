# cleveldb

LevelDB 的 C11 学习性移植，**仅支持 Linux**。

目标是把上游 LevelDB（C++）按模块移植为 C 实现，保持磁盘格式和公开语义一致，并通过阅读上游源码 + 逐任务实现来吃透 LSM-Tree 的原理。

- 架构、格式规范、C 设计：[doc/plan.md](doc/plan.md)
- 任务分解与验收：[doc/tasks.md](doc/tasks.md)
- 上游基线：`E:\github\leveldb`，`1.23-93-gbb74ef7`（commit `bb74ef7`）

> 当前状态：T001 工程骨架。除平台 smoke test 外，数据库功能尚未实现。

## 平台与依赖

- 仅支持 Linux（GCC/Clang + pthread + POSIX.1-2008）。
- 不支持 Windows/macOS。CMake 配置期和 `src/port/platform.h` 编译期都会明确报错。
- 默认构建不依赖任何第三方库；库和测试只链接 libc 与 pthread。
- 可选压缩（Snappy/Zstd）是后续 T704 的构建开关，默认关闭。

## Debian 构建

```sh
sudo apt install build-essential cmake
# 可选：sudo apt install clang ninja-build

cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j"$(nproc)"
cd build && ctest --output-on-failure
```

Release 构建：

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j"$(nproc)"
cd build-release && ctest --output-on-failure
```

Clang 构建：

```sh
cmake -S . -B build-clang \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-clang -j"$(nproc)"
cd build-clang && ctest --output-on-failure
```

常用 CMake 选项：

| 选项 | 默认 | 说明 |
|---|---|---|
| `BUILD_SHARED_LIBS` | `OFF` | `OFF` 构建静态库 `libcleveldb.a`；`ON` 构建共享库 |
| `CLDB_BUILD_TESTS` | `ON` | 构建 `cleveldb_tests` 并注册到 CTest |
| `CLDB_BUILD_TOOLS` | `OFF` | 对应源文件存在时构建 `cldb_dump` / `cldb_bench` |
| `CLDB_WERROR` | `OFF` | 把编译警告当作错误（CI 用） |

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
```

内部头文件放在 `src/` 下，不对外安装；`#include "port/..."` 等路径由库的私有 include 目录解析。

## 每个任务的验证流程

1. 只实现一个任务，保持改动独立可审阅。
2. 在 Debian 上按上面的命令构建，运行本任务相关测试。
3. 用 ASan/UBSan 构建再跑一遍相关测试。
4. 审阅本任务的 `git diff`/新增代码，对照上游实现理解所有权、锁和错误路径。
5. 回答任务里的“吃透检查点”，再进入下一个任务。
