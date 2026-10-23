# cleveldb：LevelDB → C 语言移植与学习计划（SDD）

| 项目 | 内容 |
|---|---|
| 文档版本 | v1.1 |
| 编写日期 | 2026-10-23 |
| 上游基线 | `E:\github\leveldb`，`git describe = 1.23-93-gbb74ef7`，commit `bb74ef739973a70ca9f0d90788a1da89b788c0ad`（2026-09-01） |
| 目标仓库 | `E:\github\cleveldb` |
| 实现语言 | C11（Linux 专用，POSIX 实现） |
| 平台范围 | 仅 Linux（GCC/Clang + pthread + POSIX）；不支持 Windows/macOS |
| 配套文档 | `doc/tasks.md`（任务分解）、`doc/notes/`（每个任务的学习笔记） |
| 参考文档 | 上游 `doc/impl.md`、`doc/table_format.md`、`doc/log_format.md`、`doc/index.md` |

---

## 0. 一页摘要

### 0.1 要做的事

把最新 LevelDB 的 C++ 实现按模块移植为 C11 实现，得到一个可用的嵌入式 LSM-Tree 键值存储库 `cleveldb`：

- 保持 LevelDB 的公开语义：`Put / Delete / Write / Get / Iterator / Snapshot / CompactRange / GetProperty` 等；
- 保持**磁盘格式兼容**：WAL、SSTable、MANIFEST、CURRENT、文件名、内部键编码、CRC32C 算法全部与上游一致；
- 在真实数据目录上做双向验证：C++ LevelDB 写、C 版读；C 版写、C++ LevelDB 读（双方关闭压缩时）；
- 通过移植上游测试 + 自建差分测试 + 故障注入 + 并发压测来证明正确性；
- 每个任务都配套“原理摘要 + 自检问题 + 小实验”，最终能独立解释、调试、修改 LevelDB 的核心机制。

### 0.2 怎么做

采用 **SDD（Specification-Driven Development）**：

1. **Specify**：把上游行为写成可验证的需求（本文件的 FR/NFR、格式规范、不变量）。
2. **Design**：先确定 C 语言映射方式和模块接口，再写代码；每个任务在 `doc/notes/Txxx.md` 里记录设计与上游对应关系。
3. **Tasks**：按依赖顺序拆成可独立验收的小任务（`doc/tasks.md`）。
4. **Implement**：一次只做一个任务，不提前实现后续任务；遵循错误处理、内存所有权和锁规则。
5. **Verify**：单元测试、格式 golden test、与上游的差分/兼容测试、故障注入、并发/内存检查。
6. **Reflect**：回答任务的自检问题，记录实验观察，再进入下一任务。

### 0.3 最终交付物

```
libcleveldb.a（静态库优先；可选 libcleveldb.so）
include/cleveldb/*.h           公开 C API
src/**                         实现
tests/**                       单元测试 + 差分测试
tools/cldb_dump, cldb_bench   诊断与基准工具
doc/plan.md, doc/tasks.md      本计划与任务
doc/notes/*.md                 每个任务的原理笔记与实验记录
```

### 0.4 成功标准（Definition of Success）

必须同时满足：

1. 功能：DB 打开/创建/恢复、读写、批量写、快照、迭代器、flush、自动 compaction、手工 compaction、属性查询、销毁、修复全部可用。
2. 格式：在不压缩的前提下，C++ LevelDB 与 C 版可以互相打开对方创建的数据库并读到相同结果。
3. 正确性：移植后的关键测试（db/log/table/version/skiplist/cache/bloom/filename/recovery/corruption）通过；随机模型测试与参考实现一致。
4. 健壮性：ASan/UBSan/Valgrind（至少一种）无内存错误；并发测试通过（TSAN 可用时）；WAL/表/MANIFEST 的人为损坏不会崩溃，返回合理错误或被修复。
5. 学习：能不看源码讲清楚写路径、读路径、版本管理、compaction、恢复、缓存、布隆过滤器，并完成每个任务指定的实验。
6. 性能：正确性优先；在 `db_bench` 的 fillrandom/readrandom 上与上游保持同一数量级，热点路径没有明显算法级退化。

---

## 1. 目标、范围与验收定义

### 1.1 目标（Goals）

- **G1 结构对应**：每个 C 模块都能指回上游文件/类，避免“重新发明 LevelDB”。
- **G2 语义一致**：快照隔离、批量写原子性、sequence 编号、迭代器双向移动、compaction 删除规则与上游一致。
- **G3 格式一致**：所有落盘字节格式与上游相同，便于交叉验证和调试。
- **G4 可测试**：每个模块都能在不启动完整 DB 的情况下单测；测试不依赖 C++/gtest。
- **G5 平台明确**：只支持 Linux（POSIX）；平台相关代码集中在 `src/port/` 和 Env，不追求 Windows/macOS 兼容。
- **G6 可学习**：任务粒度小，每个任务都有明确的原理检查点和小实验。

### 1.2 非目标（Non-Goals）

- 不实现 C++ ABI/源码兼容；对外只提供 C API。
- 不追求与上游逐行一致的 C++ 写法；允许用 C 惯用法改写，但行为和格式必须一致。
- 不实现多进程并发写（遵循 LevelDB：同一 DB 目录只允许一个进程写，靠 `LOCK` 文件）。
- 不强制实现 Snappy/Zstd 压缩。首版只支持 `kNoCompression`；压缩作为可插拔接口和可选任务。
- 不保证修复（Repair）后的数据库保留原有层级结构和快照语义；与上游一样，修复是尽力而为。
- 不追求精确的内存占用数字或与硬件无关的绝对性能指标。
- 不做跨平台支持：不支持 Windows、macOS 等非 Linux 平台；也不实现 HDFS、Fuchsia 等特殊 Env。

### 1.3 明确简化项（允许的偏差）

| 偏差 | 原因 | 影响 |
|---|---|---|
| `Status` 用内联定长消息（建议 256 字节），不用上游“不可变堆状态 + 永不释放”方案 | C 中避免所有权歧义和分配失败传播；状态不落盘 | 错误文本可能截断；不改变控制流语义 |
| 首版只实现 `kNoCompression` | 不引入第三方压缩库；先打通格式（见 §1.6） | 与 C++ 互操作时两端都必须设置 `compression = kNoCompression` |
| Env 只实现 Linux/POSIX 基础能力 | 聚焦核心 | 不移植 Windows/macOS 分支；首版无 mmap 优化、无特殊平台支持 |
| 用自定义轻量测试框架代替 googletest | gtest 是 C++；减少依赖 | 测试写法不同，但用例语义可一一对应 |
| `std::set`/`std::map` 用有序动态数组/哈希表替代 | C 无 STL | 复杂度在 DB 规模下可接受；必要时再优化 |
| 公开 API 重新设计为 `cldb_*` | 提供真正的 C 接口 | 与上游 `leveldb::DB` 名称不同，语义对应 |

**原则：任何简化都不能改变磁盘格式、可见读写语义、崩溃恢复语义。** 如果某个简化会影响其中任意一项，必须在任务笔记里写出理由并加测试。

### 1.4 完成定义（Definition of Done，每个任务都适用）

一个任务只有全部满足才算 DONE：

- [ ] 代码在 Linux/GCC 或 Clang 上编译通过，`-Wall -Wextra` 无新增警告。
- [ ] 有对应单元测试；测试通过；新增代码路径有错误分支测试。
- [ ] 不引入未说明的内存所有权、锁顺序或全局状态。
- [ ] ASan/UBSan 跑过相关测试。
- [ ] `doc/notes/Txxx.md` 已写：上游调用链、C 设计、关键不变量、实验记录、自检问题答案。
- [ ] 能不看源码回答该任务的“吃透检查点”。
- [ ] 没有把后续任务的逻辑提前塞进来（保持任务边界，便于学习）。

### 1.5 学习闭环（每个任务的固定动作）

1. **读**：按任务给出的上游文件/函数阅读；先用调用链理解“谁调用它、它依赖谁”。
2. **讲**：合上源码，用自己的话写 200–500 字原理摘要（写入 note）。
3. **画**：画出该模块的数据结构/状态转换或字节布局。
4. **译**：写出 C 头文件接口与所有权约定，再实现。
5. **验**：跑单测 + 至少一个边界/错误用例。
6. **实验**：改一个参数或注入一个错误，观察行为并记录。
7. **答**：回答任务底部的问题；答不出来的部分回到源码补读。

> 学习标准分三级：L1 能解释（读得懂）；L2 能脱离源码复述并定位常见 bug；L3 能修改并预测/测量影响。阶段门要求 L2，最终验收要求核心模块达到 L3。

### 1.6 第三方依赖策略（C 版）

上游 LevelDB 的核心本身不依赖第三方库；`CMakeLists.txt` 里的 Snappy/Zstd/CRC32C/tcmalloc 检测全部是可选项，只有测试和基准会拉取 GoogleTest、Google Benchmark 子模块。C 版沿用“核心零第三方”的原则：

| 依赖 | 上游用途 | 上游是否必需 | C 版策略 |
|---|---|---|---|
| Snappy | SSTable block 压缩；`Options::compression` 默认 `kSnappyCompression` | 否；未编译时写路径回退未压缩 | 可选。T704 通过 `snappy-c.h` 的 C API 链接系统 `libsnappy`，不直接使用 C++ `snappy::*` 接口 |
| Zstd | SSTable block 压缩；`kZstdCompression` | 否 | 可选。T704 直接使用 `libzstd` 的 C API |
| google/crc32c | CRC32C 的可选加速实现 | 否 | 不用；T106 自实现查表 CRC32C（后续可加 SSE4.2），保证与上游逐位一致 |
| tcmalloc | 可选分配器 | 否 | 核心不链接；基准可按需显式链接，不作为构建依赖 |
| GoogleTest/GMock | 上游单元测试 | 仅测试 | 不用；T002 自建 C 测试框架。上游测试只在独立的上游构建目录运行 |
| Google Benchmark | 上游基准测试 | 仅基准 | 不用；T805 自建 `cldb_bench`，C++ `db_bench` 独立构建用于同机对照 |
| SQLite | `db_bench_sqlite3` 对照 | 仅基准 | 不引入；需要时作为外部对照工具 |
| Kyoto Cabinet | `db_bench_tree_db` 对照 | 仅基准 | 不引入（GPLv3，避免把 GPL 依赖带入本项目） |

具体规则：

- `libcleveldb` 的强制依赖只有 C11 标准库 + Linux/POSIX（libc、pthread）；默认构建不依赖 libstdc++ 或任何 C++ ABI。
- 不把第三方源码或 git submodule 复制进本仓库；Snappy/Zstd 通过系统包或显式路径链接。这一点与上游 CMake 的 `check_library_exists` 思路一致，但 C 版同时要求找到头文件，配置期缺依赖直接报错。
- 可选压缩由 `CLDB_WITH_SNAPPY`、`CLDB_WITH_ZSTD` 控制，默认 OFF。
- 默认构建（两个开关均 OFF）写出的块全部为 type=0，可与同样未启用压缩的 C++ LevelDB 双向互读；要读上游默认配置（Snappy）创建的 DB，必须启用 `CLDB_WITH_SNAPPY`。
- 使用 C API 不代表运行时完全没有 C++ 依赖：系统 `libsnappy.so` 本身通常以 C++ 实现并传递依赖 libstdc++；这是启用 Snappy 后的库传递依赖，不是 `libcleveldb` 源码的 C++ 依赖。若静态链接 `libsnappy.a`，还需显式加 `-lstdc++`；动态链接则由 `libsnappy.so` 的 DT_NEEDED 自动带入。Zstd 是纯 C 库，无此问题。若严格要求进程不加载 libstdc++，只能自行实现 Snappy 或换用纯 C 实现；首版不采用。
- 允许的第三方许可证：Snappy BSD-3-Clause、Zstd BSD-3-Clause；启用时在 `THIRD_PARTY_NOTICES` 中记录。tcmalloc（Apache-2.0）等基准可选项不进入发布库。

---

## 2. 上游基线分析

### 2.1 版本与新增特性

上游当前版本字符串仍是 `1.23`（`include/leveldb/db.h` 中 `kMajorVersion=1, kMinorVersion=23`），但 `git describe` 为 `1.23-93-gbb74ef7`，包含 1.23 之后的 93 个提交。移植时必须覆盖以下新特性：

- `CompressionType::kZstdCompression = 0x2` 与 `Options::zstd_compression_level`（默认 1）。
- `Options::reuse_logs`（默认 false）：恢复时复用最后一个 WAL 文件，减少一次不必要的 L0 表生成。
- `Cache::SetCapacity(size_t)`：运行期调整 LRU 容量并立即驱逐超额条目。
- `Env::RemoveFile` / `Env::RemoveDir` 新命名；`DeleteFile` / `DeleteDir` 仍保留为兼容别名。
- `Iterator::RegisterCleanup`：迭代器销毁时执行清理回调（用于释放 memtable/version 引用等）。
- `Options::paranoid_checks` 默认 false，但恢复 WAL 时仍强制校验 CRC；`ReadOptions::verify_checksums` 默认 false。
- 表索引键缩短、过滤器元数据、压缩类型字节等细节以当前源码为准。

### 2.2 上游代码地图

| 目录/文件 | 职责 | 移植优先级 |
|---|---|---|
| `include/leveldb/slice.h`, `status.h` | 基础值类型 | P1 |
| `include/leveldb/options.h`, `comparator.h`, `filter_policy.h`, `cache.h`, `env.h`, `iterator.h`, `write_batch.h` | 公开接口 | P1 |
| `util/coding.*`, `hash.*`, `random.h`, `arena.*`, `crc32c.*`, `bloom.cc`, `cache.cc` | 基础算法/容器 | P1 |
| `util/env_posix.cc`, `posix_logger.h`, `helpers/memenv/` | Linux/POSIX 平台抽象与测试环境 | P1/P2 |
| `db/filename.*`, `util/logging.*` | 文件名/日志格式化 | P1 |
| `db/log_format.h`, `log_writer.*`, `log_reader.*` | WAL | P2 |
| `db/dbformat.*`, `write_batch.*`, `memtable.*`, `skiplist.h`, `snapshot.h` | 内存引擎与内部键 | P3 |
| `table/block_builder.*`, `block.*`, `filter_block.*`, `format.*` | SSTable 块格式 | P4 |
| `table/table_builder.*`, `table.*`, `two_level_iterator.*` | SSTable 读写 | P4 |
| `table/merger.*`, `table/iterator.*`, `iterator_wrapper.h` | 迭代器栈 | P4 |
| `db/version_edit.*`, `version_set.*`, `table_cache.*`, `builder.*` | 版本/MANIFEST/表缓存 | P5 |
| `db/db_impl.*`, `db_iter.*` | DB 状态机、读写、后台 compaction | P6 |
| `db/repair.cc` | 修复 | P7 |
| `db/c.cc`, `db/dumpfile.cc`, `db/leveldbutil.cc` | 工具/绑定（仅参考） | P7 |
| `benchmarks/db_bench.cc` | 基准 | P8 |
| `db/*_test.cc`, `table/*_test.cc`, `util/*_test.cc` | 行为规范与测试用例 | 贯穿始终 |

### 2.3 必须先读的上游文档

- `doc/impl.md`：文件类型、flush、compaction、恢复、垃圾回收的宏观流程。
- `doc/table_format.md`：data/meta/index block、footer、filter block。
- `doc/log_format.md`：32 KB block、7 字节记录头、分片类型。
- `doc/index.md`：公开 API 语义、快照、同步写、比较器、缓存/过滤器。
- `include/leveldb/*.h`：以头文件注释为准，不用博客替代源码。

---

## 3. 需求规格（Specification）

### 3.1 功能需求

| ID | 需求 | 验收方式 | 主要任务 |
|---|---|---|---|
| FR-01 | 打开/创建/关闭 DB；`create_if_missing`、`error_if_exists`；同一目录单写者锁 | 单元测试 + 双进程锁测试 | T604 |
| FR-02 | `Put/Delete/Write`；批量写原子；`sync` 写持久化 | 崩溃/恢复测试、批量中途失败测试 | T605, T617 |
| FR-03 | `Get` 遵守快照序列；memtable → imm → L0 → L1..L6 查找 | db_test + 快照测试 | T606 |
| FR-04 | 用户迭代器支持正/反向、Seek、快照；隐藏已删除/旧版本 | db_iter_test + 随机模型 | T607, T608 |
| FR-05 | 快照创建/释放；compaction 不能删除快照可见版本 | 快照保留测试 | T305, T610 |
| FR-06 | WAL 写入/读取/校验/分片/恢复；`reuse_logs` | 与上游字节对比 + 截断恢复 | T201–T204, T603, T617 |
| FR-07 | MemTable/SkipList/Arena；WAL 恢复与 flush 到 L0 | 单测 + 小型 DB 集成 | T303–T306, T602 |
| FR-08 | SSTable 读写：block 前缀压缩、restart、filter、index、footer、CRC | 格式 golden + 交叉读取 | T401–T409 |
| FR-09 | Version/VersionEdit/MANIFEST/CURRENT；版本引用计数 | version_set_test + MANIFEST golden | T501–T512 |
| FR-10 | 自动 compaction（size/seek）、trivial move、手工 CompactRange | 小 buffer 强制 compaction + 内容校验 | T507–T512, T609–T612 |
| FR-11 | Bloom filter、FilterBlock | 误判率/格式测试 | T111, T404 |
| FR-12 | BlockCache/TableCache、容量调整、淘汰语义 | cache_test + table_cache 测试 | T112, T509 |
| FR-13 | GetProperty/GetApproximateSizes/NumFilesAtLevel | 属性值测试 | T613 |
| FR-14 | DestroyDB/RepairDB | 删除/修复测试 | T614, T701 |
| FR-15 | Env 抽象：文件、目录、锁、日志、时间、后台线程；Linux/POSIX、MemEnv、FaultEnv | Linux env 测试 + CI | T113–T115, T117 |
| FR-16 | 自定义 Comparator/FilterPolicy/Env/Cache | 自定义实现测试 | T109, T111, T113, T117 |
| FR-17 | 与 C++ LevelDB 磁盘格式兼容（无压缩） | 双向交叉打开 | T802 |
| FR-18 | dump/bench 工具 | 工具冒烟测试 | T702, T805 |
| FR-19 | 公开 C API 完整、错误码清晰、内存所有权明确 | examples + API 测试 | T703 |
| FR-20 | 后台错误传播、关闭时等待后台工作、资源释放 | 故障注入 + 关闭测试 | T601, T609, T617 |

### 3.2 非功能需求

| ID | 需求 | 验收 |
|---|---|---|
| NFR-01 | 所有公开操作要么成功，要么返回明确错误；不崩溃、不静默丢数据 | 故障注入/模糊测试 |
| NFR-02 | 多线程读者 + 单写者 + 后台 compaction 的线程安全 | TSAN/压力测试 |
| NFR-03 | 崩溃一致性：已同步写不丢；未同步写可能丢失但 DB 可恢复 | 截断/断电模拟 |
| NFR-04 | 内存有界：block cache 容量、table cache 文件数、memtable 大小 | 配置边界测试 |
| NFR-05 | 无未定义行为、无泄漏（除明确文档化的进程级单例） | ASan/UBSan/Valgrind |
| NFR-06 | 可测试性：核心逻辑不依赖真实磁盘；可注入错误 | MemEnv/FaultEnv |
| NFR-07 | C11 + POSIX.1-2008；仅 Linux 平台 | Linux GCC/Clang 构建与测试 |
| NFR-08 | 与上游同数量级性能；不因 C 化引入 O(n²) 路径 | db_bench 对比 |

### 3.3 需求到阶段的映射

```
P0 脚手架
P1 基础库  ────────────────┐
P2 WAL                      │
P3 内存引擎                  ├──> P5 版本/压缩 ──> P6 DB 核心 ──> P7 工具/修复 ──> P8 兼容/加固
P4 SSTable ─────────────────┘
```

---

## 4. 架构分析（Architecture）

### 4.1 组件总览

```
                       +--------------------------------------+
                       |          公开 C API (cldb_*)          |
                       +------------------+-------------------+
                                          |
             +----------------------------v----------------------------+
             |                      DBImpl                             |
             |  mutex / writer queue / snapshots / bg_error / stats     |
             +--+--------------+----------------+-------------------+--+
                |              |                |                   |
          MemTable        WAL Writer       VersionSet           TableCache
        (Arena+SkipList)   (log::Writer)   (MANIFEST/CURRENT)   + BlockCache
                |              |                |                   |
                |              |             Version(s) ---------> Table
                |              |                |                   |
                +--------------+---- BackgroundCompaction ----------+
                                          |
                                   BuildTable / TableBuilder
                                          |
                     Env (Linux/POSIX / MemEnv / FaultEnv)
```

关键对象关系：

- `DBImpl` 持有 `mem_`、`imm_`、`log_`/`logfile_`、`versions_`、`table_cache_`、`snapshots_` 和写者队列。
- `MemTable` 内部是 `Arena + SkipList`，引用计数；`imm_` 只由后台 flush 处理。
- `VersionSet` 维护 `current_`（当前版本）、`dummy_versions_`（所有存活版本的双向链表）、文件号/sequence/log 号等元数据，以及 `descriptor_log_`（MANIFEST 追加写）。
- `Version` 是不可变的“某一时刻的文件布局快照”；每个版本持有各层 `FileMetaData*`，文件元数据被多个版本共享。
- `TableCache` 用 LRU cache 缓存已打开的 `Table`；`Table` 内部用 block cache 缓存数据块。
- `Env` 是所有 I/O、线程、时间的唯一入口；测试用 MemEnv/FaultEnv 替换。

### 4.2 必须始终成立的不变量

| # | 不变量 | 违反后果 |
|---|---|---|
| I-1 | 内部键排序：user key 升序；同一 user key 中 tag（sequence/type）降序，即新数据在前 | 读/合并/compaction 结果错误 |
| I-2 | 同一 user key 的 sequence 唯一（由写者串行分配保证） | 快照边界不确定 |
| I-3 | `LastSequence` 只在整批 WAL 写入且 memtable 插入完成后推进 | 快照读到“未来”数据或恢复丢序列 |
| I-4 | `mem_` 可写；`imm_` 不可变；读者在看期间持有引用 | use-after-free / 读到半成品 |
| I-5 | `Version` 发布后不可变；`current_` 的替换在 DB mutex 下 | 读到不一致的文件集合 |
| I-6 | L0 文件可重叠；L1–L6 内文件按 smallest key 有序且不重叠 | 查找/压缩算法失效 |
| I-7 | 任何 SSTable 必须先落盘并 `Sync`，然后才能写进 MANIFEST | 崩溃后 MANIFEST 指向不存在的表 |
| I-8 | CURRENT 的更新是“写临时文件 + rename”，且 MANIFEST 记录先于 CURRENT | 崩溃后无法恢复 |
| I-9 | 同一时刻只有一个 compaction 在跑；`pending_outputs_` 防止正在生成的输出被 GC | 删除正在写的文件 |
| I-10 | 快照 = 一个 sequence number；释放快照只是从链表删除，不需要通知读者 | 旧版本被过早删除 |
| I-11 | `Get`/迭代器在释放 DB mutex 前固定 snapshot 并增加 mem/imm/version 引用 | 读到不一致状态 |
| I-12 | WAL 记录先于 memtable 插入；恢复重放 WAL 可重建 memtable | 已确认写丢失 |
| I-13 | 写者队列同一时刻只有 leader 写日志；组提交只扩大当前批次，不改变“日志先行”顺序 | 日志顺序与 sequence 不一致 |
| I-14 | 后台错误发生后停止文件 GC 和后续 compaction；写者返回该错误 | 在不确定状态上继续破坏数据 |

### 4.3 写路径（Write Path）

#### 4.3.1 单次写流程

1. `cldb_put/delete` 构造 `WriteBatch`（12 字节头：8 字节 sequence + 4 字节 count；后接操作记录）。
2. `DBImpl::Write`：
   - 在 `mutex_` 下把当前线程包装为 `Writer` 推入 FIFO `writers_`；
   - 非队首线程在各自的 condition variable 上等待（`done` 或轮到自己）。
3. 队首成为 leader，调用 `MakeRoomForWrite(force=false)`：
   - 若 `bg_error_` 非 OK → 直接失败；
   - 若 L0 文件数 ≥ `kL0_SlowdownWritesTrigger (8)` 且允许延迟 → 解锁睡眠 1 ms，且每个写最多延迟一次；
   - 若 `mem_->ApproximateMemoryUsage() <= write_buffer_size` → 有空间，继续；
   - 若 `imm_ != nullptr` → 等待后台 flush；
   - 若 L0 文件数 ≥ `kL0_StopWritesTrigger (12)` → 等待；
   - 否则切换 memtable：分配新文件号、创建新 WAL、关闭旧 WAL、`imm_ = mem_`、`has_imm_ = true`、新建 `mem_`、触发后台 compaction。
4. `BuildBatchGroup` 做组提交：
   - 上限 1 MB；如果第一个 batch ≤128 KB，则上限改为 `first_size + 128 KB`，避免小写被大组拖慢；
   - 遇到“follower 要求 sync 而 leader 不要求 sync”时停止合并（不能把同步写混入非同步组）；
   - 多于一个 batch 时复制到 `tmp_batch_`，不修改调用者的 batch。
5. 分配序列号：`first_seq = LastSequence + 1`，`last_seq = first_seq + Count - 1`；`SetSequence(batch, first_seq)`。
6. 释放 DB mutex，执行：
   - `log_->AddRecord(batch bytes)`：写 WAL（可能分片、可能触发 WritableFile 缓冲 Flush）；
   - 若 `options.sync` → `logfile_->Sync()`；
   - `WriteBatch::InsertInto(mem_)`：逐条插入，`MemTableInserter` 对每条操作递增 sequence；
   - 重新加锁；若 sync 失败，记录 `bg_error_` 并让后续写失败（日志状态不确定）。
7. `versions_->SetLastSequence(last_seq)`；唤醒组内等待者，再唤醒新队首。
8. 返回状态；所有被合并的 writer 获得同一结果。

**顺序关键点**：WAL 写入 → （可选 Sync）→ memtable 插入 → LastSequence 推进。不能先推进 LastSequence；否则快照可能读到尚未进入 memtable 的序列号。

#### 4.3.2 flush 与 L0

- `CompactMemTable`（后台）把 `imm_` 通过 `BuildTable` 写成一个 SSTable，使用 `base->PickLevelForMemTableOutput` 选择 L0/L1/L2（默认最多 L2，见 `kMaxMemCompactLevel=2`）。
- `WriteLevel0Table` 在生成文件期间把文件号放入 `pending_outputs_`，防止 GC 删除；文件大小 0 视为未生成，不加入 MANIFEST。
- `CompactMemTable` 构造 edit（`prev_log_number=0`、`log_number=当前 WAL 号`）并调用 `LogAndApply`；只有提交成功后才删除 `imm_`、释放引用并调用 `RemoveObsoleteFiles`。
- 若生成文件失败，记录 `bg_error_`，`imm_` 保留；所有写者随后失败/等待策略由 `MakeRoomForWrite` 处理。

#### 4.3.3 重要常量与默认值

| 常量 | 值 | 作用 |
|---|---|---|
| `kNumLevels` | 7 | 层级数（L0–L6） |
| `kL0_CompactionTrigger` | 4 | L0 文件数达到 4 触发 compaction |
| `kL0_SlowdownWritesTrigger` | 8 | 开始每次写延迟 1 ms |
| `kL0_StopWritesTrigger` | 12 | 停止接收新写，等待 compaction |
| `kMaxMemCompactLevel` | 2 | memtable flush 最多放到 L2 |
| `write_buffer_size` | 4 MB | memtable 阈值（Sanitize 后 64 KB–1 GB） |
| `max_file_size` | 2 MB | compaction 输出文件目标大小（1 MB–1 GB） |
| `block_size` | 4 KB | SSTable 数据块目标大小（1 KB–4 MB） |
| `block_restart_interval` | 16 | block 前缀压缩 restart 间隔 |
| `max_open_files` | 1000 | Sanitize 后 [74, 50000]；TableCache 容量 = 该值 - 10 |
| `kReadBytesPeriod` | 1 MB | 迭代器读取采样周期，用于 seek compaction |

### 4.4 读路径（Read Path）

#### 4.4.1 Get

1. 在 DB mutex 下确定 `snapshot`：`ReadOptions.snapshot` 的 sequence，否则 `LastSequence`。
2. 对 `mem_`、`imm_`（若存在）、`current_` 增加引用；解锁。
3. 构造 `LookupKey(user_key, snapshot)`（内部键 = user key + `(snapshot << 8) | kValueTypeForSeek`）。
4. 依次查找：
   - `mem_->Get`：SkipList 定位第一个 ≥ LookupKey 的条目，解析内部键，比较 user key；若同 key 且 sequence ≤ snapshot，则是 value/deletion；否则未命中；
   - `imm_->Get`（若有）；
   - `current_->Get`：见下。
5. 重新加锁；若查询经过多个文件，`Version::UpdateStats` 递减 `allowed_seeks`，≤0 时标记 seek compaction 并调度后台工作；释放引用。
6. 返回 value 或 `NotFound`；deletion 返回 `NotFound`，而不是错误。

`Version::Get` 的文件顺序：

- L0：筛选所有 user key 范围覆盖目标的文件，按**文件号从大到小**（新→旧）查找，因为 L0 文件可重叠，文件号越大越新；
- L1–L6：按 smallest key 二分（`FindFile`）找到第一个 `largest >= internal_key` 的文件；若该文件的 smallest user key 大于目标 user key，说明目标不在该层；否则查该文件。
- 每个文件通过 `TableCache::Get`：先查 filter（若配置），再索引块定位数据块，最后在块内 seek。
- 一旦命中 value 或 deletion 即停止；`kCorrupt` 转成 Corruption 错误。

#### 4.4.2 迭代器栈

```
DBIter (user key / snapshot / deletion 过滤 / 双向)
  └── MergingIterator (按 InternalKeyComparator 归并)
        ├── MemTable::NewIterator()
        ├── Immutable MemTable::NewIterator()
        ├── L0: 每个文件一个 Table::NewIterator()（TwoLevelIterator）
        └── L1..L6: 每层一个 ConcatenatingIterator
              └── LevelFileNumIterator (按 smallest key 遍历文件号)
                    └── Table::NewIterator() (TwoLevelIterator)
```

- `NewInternalIterator` 在 mutex 下收集所有子迭代器，固定 `latest_snapshot`，并对 `mem_`/`imm_`/`current_` 增加引用；注册 `CleanupIteratorState`，在迭代器销毁时递减引用。
- `MergingIterator` 用 `current_` 指针指向最小（或反向时最大）的子迭代器；切换方向时重新定位所有子迭代器。
- `TwoLevelIterator` 先用 index iterator 找到数据块 handle，再创建/复用 data block iterator；空块跳过。
- `DBIter` 只暴露 user key；对同一 user key 跳过 sequence > snapshot 的条目，跳过被更新的版本和 deletion；反向迭代时把值缓存到 `saved_value_`，因为要在读到更早版本时返回第一个可见版本。
- 每读取约 1 MB（随机周期），`DBIter` 调用 `RecordReadSample`，触发 seek compaction 统计。

### 4.5 版本管理与 MANIFEST

- **Version**：某一时刻的 `files_[7]` 布局 + 各文件的 `FileMetaData`；引用计数。`FileMetaData` 含 `number`、`file_size`、`smallest/largest` 内部键、`allowed_seeks`（初始 1<<30，成为版本文件后设为 `max(file_size/16384, 100)`）。
- **VersionEdit**：一组增量修改：comparator 名、log number、next file number、last sequence、compact pointer、deleted file（level, number）、new file（level, number, size, smallest, largest）、prev log number。编码为 varint 流。
- **VersionSet**：所有版本的链表、当前版本、下一个文件号、MANIFEST 文件号、log/prev log、最后 sequence、compact pointer。
- **LogAndApply**（简化描述）：
  1. 补全 edit 中缺省的 log/prev，填入当前 next file number、last sequence；用 Builder 在 current 基础上生成候选 Version（先不安装）。
  2. 若还没有 MANIFEST writer，用 `manifest_file_number_` 创建新 MANIFEST，先写 `WriteSnapshot`（comparator、compact pointers、current 的所有文件）。
  3. 把本次 edit 编码为 log record 追加到 MANIFEST（edit 携带 log/prev/next/last），然后 Sync。
  4. 只有本次新建了 MANIFEST 才写 CURRENT：先写 `CURRENT.tmp`，再 rename 成 CURRENT；这是提交点。
  5. 安装候选 Version，更新 `log_number_`/`prev_log_number_`；`last_sequence_`/`next_file_number_` 由调用方在提交前推进。旧版本引用释放后自然消失。
- **恢复**：读 CURRENT（必须以换行结尾，否则 Corruption）→ 解析 `MANIFEST-xxxxxx` → 用 log::Reader 逐条读 VersionEdit → Builder 应用 → 检查 comparator 名一致 → 要求 next file number、last sequence、log number 都存在 → 可选 `ReuseManifest`（要求 `options.reuse_logs=true` 且文件大小 < max_file_size）继续追加，否则下次写新 MANIFEST。
- **CURRENT 内容**：`MANIFEST-000001\n`（basename，不含 db 路径）。
- **文件号**：`next_file_number_` 从 2 开始；1 保留给初始 MANIFEST。`MarkFileNumberUsed` 在恢复 WAL 时推进计数器，避免复用旧编号。

### 4.6 Compaction

#### 4.6.1 触发与选择

- `Version::Finalize` 计算每层 score：
  - L0：`file_count / 4`；
  - L1–L6：`total_bytes / MaxBytesForLevel(level)`，其中 `MaxBytesForLevel(1)=10 MB`，每升一层 ×10；
  - 选择 score 最大的层；score ≥ 1 需要 compaction。
- seek compaction：`FileMetaData::allowed_seeks` 递减到 0 时记录 `file_to_compact_`，优先级低于 size compaction。
- `PickCompaction`：
  - size compaction：在目标层按 `compact_pointer_[level]` 轮转选择第一个 `largest > pointer` 的文件，找不到则回绕到第一个；
  - seek compaction：使用 `file_to_compact_`；
  - L0 特殊处理：选择文件后，把所有与它范围重叠的 L0 文件一起加入 inputs[0]；
  - `SetupOtherInputs` 再找 level+1 的重叠文件。
- **边界文件**：同一 user key 可能跨越同一层的两个文件（内部键范围不同）。`AddBoundaryInputs` 会把同一层中 smallest user key 等于当前最大 user key 的相邻文件也纳入，避免 Get 按层查找时返回错误版本。
- 可能扩大 inputs[0]：如果扩大后 level+1 的 inputs 不变，且 `inputs[1] 字节数 + expanded0 字节数 < 25 * max_file_size`，接受扩大，以减少后续 compaction。
- `compact_pointer_[level]` 在 `SetupOtherInputs` 选定范围后立即设为 inputs[0] 的最大键（并写入 edit），即使 compaction 失败下次也会尝试不同范围。

#### 4.6.2 trivial move

满足以下全部条件时，不重写数据，只把文件从 level 移到 level+1：

- inputs[0] 恰好 1 个文件；
- inputs[1] 为空；
- grandparents（level+2 的重叠文件）总字节 ≤ `10 * max_file_size`（默认 20 MB），避免把昂贵合并推后。

实现方式是 VersionEdit：删除原文件、在 level+1 添加同号文件（文件本身不变）。

#### 4.6.3 合并与删除规则

`DoCompactionWork` 使用 `MakeInputIterator`（L0 多文件直接 merge；高层用 concatenating iterator），按内部键顺序遍历，输出新 SSTable。删除规则（`smallest_snapshot` = 最老快照；无快照时 = LastSequence）：

- 若 `last_sequence_for_key <= smallest_snapshot`：当前条目被同一 user key 的更新版本覆盖，可删（规则 A）。
- 否则若当前是 deletion、`sequence <= smallest_snapshot`、且 `IsBaseLevelForKey(user_key)`：删除标记已无意义，可删。
  - `IsBaseLevelForKey` 检查 level+2 及更深层是否可能有该 user key；深层的都是更老数据，若没有，且本次输入中更老版本会被规则 A 删除，则删除标记可以消失。
- 解析内部键失败时：清空当前 user key 跟踪（`has_current_user_key=false`、`last_sequence_for_key=kMaxSequenceNumber`），但不丢弃该条目（“do not hide error keys”），让它保留到输出以暴露问题。
- `last_sequence_for_key` 在遇到新 user key 时重置为 `kMaxSequenceNumber`。

输出文件切分：

- 每个输出文件达到 `max_file_size` 后结束当前文件；
- `ShouldStopBefore(key)`：累计 level+2（grandparent）中 largest < key 的文件字节数；超过 `10 * max_file_size` 时结束当前输出文件，避免未来 compaction 重叠过大；
- 输出文件 Finish 后记录 smallest/largest，写 VersionEdit。

安装：

- `InstallCompactionResults` 对 inputs 中每个文件 `RemoveFile`，对输出文件 `AddFile`，调用 `LogAndApply`（compact pointer 已在 `SetupOtherInputs` 写入 `c->edit_`）；
- 后台 compaction 结束调用 `RemoveObsoleteFiles` 删除不再被任何版本引用的表；
- `stats_[level]` 记录 micros、bytes_read、bytes_written。

#### 4.6.4 手工 compaction

`CompactRange(begin, end)`：先 flush memtable，再对 0..max_level_with_files-1 逐层调用 `TEST_CompactRange`；手工 compaction 使用 begin/end 构造内部键范围，`manual_compaction_` 指针让后台线程执行，期间写者/读者继续工作，调用线程等待完成。它是“尽力把范围压到更深层”，不保证所有数据都到最后一层。

### 4.7 恢复与修复

#### 4.7.1 DB::Open / Recover 的顺序

1. `CreateDir(dbname)`（忽略已存在错误）。
2. `LockFile(LOCK)`；失败则返回（同目录只允许一个写者）。
3. 检查 `CURRENT`：
   - 不存在且 `create_if_missing` → `NewDB()`：写 `MANIFEST-000001`（comparator 名为用户 comparator，默认 Bytewise；log=0、next=2、last=0），`Sync`，`SetCurrentFile`；
   - 不存在且不允许创建 → `InvalidArgument`；
   - 存在且 `error_if_exists` → `InvalidArgument`。
4. `VersionSet::Recover`：读 CURRENT/MANIFEST，重建 current version 和文件号/sequence/log 号；检查 comparator 名。
5. 扫描目录，收集编号 ≥ `log_number` 或等于 `prev_log_number` 的 `.log`；检查 MANIFEST 引用的文件是否都存在（缺失 → Corruption）。
6. 按 log 编号升序重放：
   - `log::Reader` 逐条读 WriteBatch（不足 12 字节报 corruption 并跳过）；
   - `WriteBatch::InsertInto` 写入新 MemTable；
   - 超过 `write_buffer_size` 就 `WriteLevel0Table` 并重建 memtable；
   - 追踪最大 sequence；
   - 若 `options.reuse_logs`、是最后一个 log、且没有发生过 flush，则用 `NewAppendableFile` 复用该 WAL，`log::Writer` 以现有文件长度初始化 block offset；否则把剩余 memtable flush 成 L0。
7. `LastSequence = max(LastSequence, max_sequence)`。
8. 如果没有可复用的 log/memtable，创建新的 WAL 和 memtable，并把新 log number 写入 edit。
9. 若 `save_manifest`，`LogAndApply` 写 MANIFEST；`RemoveObsoleteFiles`；`MaybeScheduleCompaction`。

#### 4.7.2 崩溃点分析

- 表文件先写、`Sync`，再 MANIFEST 记录；否则 MANIFEST 会引用不存在的表。
- MANIFEST 追加记录并 `Sync` 后，才认为版本变更提交；即使 CURRENT 尚未切换，旧 MANIFEST 也是完整一致的。
- 新 MANIFEST 首次写入时先写全量快照；CURRENT 的 rename 是原子提交点。
- WAL 记录可能只写了一半：Reader 按块/长度/CRC 校验，截断的尾部记录被丢弃；已经完整写入但未 Sync 的写可能在崩溃后消失。
- `pending_outputs_` 中的文件即使尚未进入 MANIFEST 也不会被 GC；重启后这些孤立文件会在 `RemoveObsoleteFiles` 时被清理。

#### 4.7.3 Repair（尽力修复）

`RepairDB`：

- 扫描目录中的 `.log`、`.sst/.ldb`，并收集旧 MANIFEST 供最后归档；
- 把日志转成表（能读多少读多少，坏记录报告并跳过）；
- 扫描表的每个条目，损坏的表用可读条目重建新表，坏文件移入 `lost/`；
- 取所有表的最小/最大内部键和最大 sequence；
- 写新的 `MANIFEST-000001`，把所有表放在 L0（不保留原层级），`CURRENT` 指向它；
- 修复不保留快照；可能丢失损坏部分；成功后建议用户检查日志。

### 4.8 并发与锁模型

| 状态/对象 | 保护方式 | 说明 |
|---|---|---|
| DB 状态（mem/imm/current/log/file 号/快照链/写者队列） | `DBImpl::mutex_` | 绝大多数状态只在此锁下修改；`VersionSet` 的方法要求持锁 |
| 写者等待 | 每个 Writer 自己的 `condvar` + `writers_` 队列 | 只有 leader 能执行 `MakeRoomForWrite` 和日志写 |
| 后台工作完成/唤醒 | `background_work_finished_signal_` | `MakeRoomForWrite`、手工 compaction、关闭流程等待 |
| 后台调度标志 | `background_compaction_scheduled_`（锁下） + `has_imm_`/`shutting_down_` 原子量 | 避免重复调度 |
| Version 引用计数 | DB mutex 下的手工 ref/unref（上游 C++ 在锁下操作） | 版本对象不跨锁修改，但引用计数需要谨慎 |
| MemTable 引用 | 手工 ref/unref，通常在锁下 | Get/迭代器持引用期间 memtable 不被释放 |
| TableCache | 内部 16 分片 LRU，各分片自锁 | 多读者可并发打开/查找表 |
| BlockCache | 分片 LRU + 手工 handle 引用 | cache 释放 handle 时才可能调用 deleter |
| SkipList 读 | 无锁；`next_` 原子 load acquire | 写者必须由外部互斥锁串行化（DB memtable 由 DB mutex 保护） |
| Env::Schedule | 内部互斥锁 + 条件变量 + 单后台线程 | 所有 `Schedule` 回调串行执行 |
| `has_imm_`、`shutting_down_` | 原子 load/store | 后台/写者快速路径避免拿锁（但仍需在锁下做最终决定） |

**锁顺序**：DB mutex → TableCache/BlockCache 内部分片锁；不要反向。DB mutex 下可以调用 `Env::Schedule`（Env 内部锁只做入队，不等待回调），但后台回调不得在持有 Env 内部锁时进入 DB mutex 或阻塞等待。`RemoveObsoleteFiles` 在持有 DB mutex 时收集 live 集合并删除文件（上游如此），以避免与版本提交/文件号分配竞态。

### 4.9 缓存与过滤器

- **LRU Cache**：16 个 shard，每 shard 独立锁/哈希表/LRU 链表/in-use 链表；每个条目有 `refs`（cache 引用 + 客户端引用）和 `in_cache` 标志。
  - `refs==1 && in_cache` → 在 LRU 链表；客户端 Lookup 后 `refs>=2` → 移到 in-use 链表；
  - `Release` 使 `refs` 回到 1 → 回到 LRU；`Erase` 使 `in_cache=false` 并减 cache 引用，等到客户端都 Release 后才调用 deleter；
  - `charge` 用于容量统计；容量为 0 时不缓存（但仍返回 handle）；
  - `SetCapacity` 调整容量并立即从 LRU 尾部驱逐，直到 usage ≤ capacity 或无可驱逐条目。
- **TableCache**：key 是 8 字节文件号；值为 `TableAndFile`；`NewIterator` 注册 cleanup 释放 cache handle；`Get` 查表；`Evict` 删除 key 并释放。
- **BlockCache**：key 是 `cache_id (8B) + block offset (8B)`；`cache_id` 由 `block_cache->NewId()` 生成，确保不同 Table 的相同 offset 不冲突；charge = block 大小。
- **FilterBlock**：每 2 KB 数据块区间一个 Bloom filter（`kFilterBaseLg=11`）；尾部是 offsets 数组 + array_offset(uint32) + base_lg(byte)；读取时按 `block_offset >> base_lg` 找过滤器；过滤器损坏/越界时“可能匹配”，不应误判为不存在。
- **Bloom**：`bits_per_key`（默认 10）→ `k = floor(bits_per_key * 0.69)`，限制 [1,30]；至少 64 位；双哈希：`h = Hash(key, 0xbc9f1d34)`，`delta = rotl(h,17)`，每次探测 `h += delta`；policy 名 `leveldb.BuiltinBloomFilter2` 要写入 metaindex，保证 C++/C 互操作。

---

## 5. 磁盘格式规范（On-disk Format）

> 这是移植正确性的核心约束。实现时先用 golden test 固定字节，再谈优化。

### 5.1 内部键与 WriteBatch

**内部键**：

```
user_key (任意字节)
+ fixed64 little-endian (tag)
tag = (sequence << 8) | type
type: kTypeDeletion=0, kTypeValue=1
kMaxSequenceNumber = (1 << 56) - 1
```

排序（`InternalKeyComparator`）：

1. 先按 user key 的用户比较器升序；
2. user key 相同，则按 tag 数值**降序**（sequence 大的在前，type 大的在前）；
3. 内部键比较是 unique 的（同一 user key + sequence 不会有两条记录）。

**LookupKey**：user key + `fixed64((snapshot << 8) | kValueTypeForSeek)`；`kValueTypeForSeek = kTypeValue (1)`。这使 Seek 落在“所有 sequence ≤ snapshot 的条目之前/之中”的正确位置。

**WriteBatch**：

```
offset 0: fixed64 sequence
offset 8: fixed32 count
offset 12: record*
record:
  uint8 tag (0=delete, 1=put)
  varint32 key_len; key bytes
  if put: varint32 value_len; value bytes
```

- `ByteSize = rep.size()`；`Count` 从 offset 8 读取。
- `InsertInto` 对每条记录使用 `sequence + i`，因此一个 batch 内的操作有连续序列号。
- `Append` 去掉源 batch 的 12 字节头，只复制记录并把 count 相加。

### 5.2 WAL（log format）

```
block size = 32768
header = 7 bytes:
  fixed32 checksum (masked CRC32C of type byte + payload)
  uint16 length (little-endian, <= 0xffff)
  uint8 type
types: kZeroType=0, kFullType=1, kFirstType=2, kMiddleType=3, kLastType=4
```

Writer 规则：

- 每条逻辑记录依次填满当前 32 KB block；一条记录可分片为 FIRST/MIDDLE/LAST，放得下则 FULL；
- 若 block 剩余空间 < 7 字节，写零填充，切换到下一个 block；
- 空记录也要产生一个 FULL 记录（length=0）；
- 每个物理记录独立 CRC（type + payload），`Mask` 为 `((crc >> 15) | (crc << 17)) + 0xa282ead8`；
- 每次物理记录 Append 后调用 `WritableFile::Flush()`（只保证到 OS），`sync` 写由 DBImpl 调用 `Sync`。

Reader 规则：

- 按 block/记录头读取；校验长度不超过 block 剩余空间；
- `initial_offset > 0` 时跳到目标 block；进入“resync”模式，遇到损坏后寻找下一个 FULL/FIRST 记录重新同步；
- 分片类型必须符合 FIRST→MIDDLE*→LAST，独立 FULL/MIDDLE/LAST 视为 corruption；
- 校验 CRC（恢复 WAL 时总是开启）；报告 `Corruption(bytes, status)` 给 reporter；
- EOF 时丢弃尾部不完整记录；`last_record_offset_`/`end_of_buffer_offset_` 供上层定位。

### 5.3 SSTable

#### 5.3.1 文件整体布局

```
[data block 0] [data block 1] ... [data block N]
[filter block]                 （可选）
[metaindex block]              （key: filter.<policy name> → BlockHandle）
[index block]                  （key: 每个 data block 的 last key；value: BlockHandle）
[footer: 48 bytes]
```

Footer：

```
metaindex_handle (varint64 offset, varint64 size, 最多 20 字节，补零到 20)
index_handle     (同上，20 字节)
magic            fixed64 0xdb4775248b80fb57 (little-endian)
总计 20 + 20 + 8 = 48 字节
```

每个 block 的尾部：

```
block data
uint8 type          (0=none, 1=snappy, 2=zstd)
fixed32 masked_crc32c(block data + type)
```

Block 内部：

```
entry*:
  varint32 shared          与上一个 key 的公共前缀长度
  varint32 non_shared      剩余 key 长度
  varint32 value_length
  key[shared:]             非公共后缀
  value
uint32 num_restarts         （block 末尾）
uint32 restart_offset[num_restarts]
```

- 每 `block_restart_interval` 个条目一个 restart（默认 16）；restart 处 shared=0；
- index block 的 restart interval 强制为 1；
- 前缀压缩减少空间，但 Seek 需要先二分 restart 再线性扫描；`Prev` 需要从所在 restart 重扫。

#### 5.3.2 索引与过滤器

- TableBuilder 维护“pending index entry”：直到下一个 key 到来才确定上一个 block 的 last key，并用 `FindShortestSeparator` 缩短；最后一个索引用 `FindShortSuccessor`。
- metaindex 的 key 为 `filter.<policy name>`，value 是该 filter block 的 BlockHandle。
- FilterBlock：
  - 每 2 KB 源数据一个 Bloom filter（`filter_index = block_offset >> 11`）；
  - 内容 = 各过滤器字节拼接 + offsets 数组；
  - 尾部：`uint32 array_offset`（相对 filter block 末尾的偏移）+ `uint8 base_lg (11)`；
  - 读取时 index = block_offset >> base_lg，若 index < num_filters，按 offsets[index] 找到过滤器；任何越界/解析失败都返回“may match”，避免假阴性。

#### 5.3.3 压缩

- 仅当压缩后大小 < `raw_size - raw_size/8`（严格超过 12.5% 的节省）才使用压缩；否则写原始块，type=0。
- 压缩范围：data、metaindex、index block 均经 `WriteBlock`，可能被压缩；filter block 固定不压缩；footer 不压缩。
- 块尾 CRC 覆盖“压缩内容 + type 字节”。
- C 版首版只实现 type 0；读取 type 1/2 时若未编译对应后端，返回明确的未支持错误；上游对未知 type 返回 `Corruption("bad block type")`。在文档中标注。

### 5.4 MANIFEST / CURRENT / 文件名

文件名（由 `filename.cc` 生成，路径分隔符统一用 `/`）：

| 类型 | 格式 | 示例 |
|---|---|---|
| WAL | `%06llu.log` | `000123.log` |
| 表 | `%06llu.ldb` 或 `%06llu.sst` | `000456.ldb` |
| MANIFEST | `MANIFEST-%06llu` | `MANIFEST-000001` |
| CURRENT | `CURRENT` | 内容 `MANIFEST-000001\n` |
| 锁 | `LOCK` | 空文件 |
| 临时文件 | `%06llu.dbtmp` | `000789.dbtmp` |
| 信息日志 | `LOG`, `LOG.old` | 旧 LOG 重命名为 LOG.old |

`ParseFileName` 必须能从文件名反解 `(number, FileType)`；数字不足 6 位也接受（读取旧文件），生成时补零。

VersionEdit 编码（varint 流，tag 后跟字段）：

| tag | 含义 | 字段 |
|---|---|---|
| 1 | comparator | length-prefixed string |
| 2 | log number | varint64 |
| 3 | next file number | varint64 |
| 4 | last sequence | varint64 |
| 5 | compact pointer | level varint32 + length-prefixed internal key |
| 6 | deleted file | level varint32 + number varint64 |
| 7 | new file | level, number, file_size, smallest (lp), largest (lp) |
| 9 | prev log number | varint64 |

MANIFEST 是这些记录的 WAL 流；第一条通常是全量快照。未知 tag 返回 `Corruption("unknown tag")`，字段解析失败同样报 Corruption；不要自行忽略（与上游 `version_edit.cc` 一致）。

### 5.5 CRC32C

- 多项式：CRC-32C（Castagnoli），反射实现，初始/最终异或 0xffffffff。
- `Extend(crc, data, n)` 必须支持增量；`Mask`/`Unmask` 用于磁盘存储。
- 上游软件实现用 byte 表 + 4 路 stride 表；可选 SSE4.2 加速（`_mm_crc32_u64`），但必须保证结果一致。
- 已知测试向量（来自上游 `util/crc32c_test.cc`）：32 字节全 0 → `0x8a9136aa`；32 字节全 0xff → `0x62a8ab43`；32 字节 `0..31` → `0x46dd794e`；32 字节 `31..0` → `0x113fdb5c`；RFC3720 的 48 字节样本 → `0xd9963a56`。
- WAL 与 SSTable 都使用同一 `crc32c` 实现，但覆盖范围不同（见上）。

### 5.6 兼容性约束清单

- [ ] comparator 名称必须为 `"leveldb.BytewiseComparator"`（除非用户自定义并确保 C++ 侧同名）。
- [ ] filter policy 名称必须为 `"leveldb.BuiltinBloomFilter2"`。
- [ ] table magic、footer 长度、block trailer、CRC 掩码完全一致。
- [ ] WAL block/header/type、CURRENT 内容、文件名一致。
- [ ] VersionEdit tag/字段顺序、MANIFEST 日志格式一致。
- [ ] `kNoCompression` 时块 type=0；C++ 侧必须设置相同选项。
- [ ] 文件号/序列号/last sequence 的更新顺序一致；恢复可接受旧版 `prev_log_number`。
- [ ] 写入 `CURRENT` 时只写 basename + `\n`，不带 db 路径。
- [ ] `.ldb` 与 `.sst` 都要能识别；新文件默认 `.ldb`。

---

## 6. C 语言设计（Design）

### 6.1 总体原则

1. **模块边界与上游一致**：`db/`、`table/`、`util/`、`port/`、`helpers/memenv/`；文件命名对应上游。
2. **公开 API 与内部实现分离**：`include/cleveldb/` 只放用户可用类型；内部结构体定义在 `src/**` 的头文件，不暴露给用户。
3. **不透明句柄**：用户持有 `cldb_t*`、`cldb_iterator_t*`、`cldb_snapshot_t*`、`cldb_cache_t*` 等，内部布局不可见。
4. **显式生命周期**：`_create/_init` 与 `_destroy/_free` 成对；没有析构函数、没有异常、没有隐式清理。
5. **错误用返回值传播**：所有可能失败的函数返回 `cldb_status_t` 或 NULL + status 出参；禁止用全局 errno 作为唯一错误通道。
6. **长度显式**：所有字节串用 `(const char*, size_t)`（Slice），不假设 NUL 结尾；路径单独保证 NUL 结尾。
7. **所有权写进注释和命名**：谁分配、谁释放、借用多久；每个结构体头部注释列出 owned/borrowed 字段。
8. **单线程写、多线程读**：与上游一致；不要引入额外的全局锁或“线程安全容器”掩盖锁设计。

### 6.2 Slice、Status、Buffer

**Slice**（公开，按值传递）：

```c
typedef struct cldb_slice {
  const char *data;
  size_t size;
} cldb_slice_t;

static inline cldb_slice_t cldb_slice_make(const char *data, size_t size);
static inline int cldb_slice_compare(cldb_slice_t a, cldb_slice_t b);
static inline bool cldb_slice_equal(cldb_slice_t a, cldb_slice_t b);
static inline void cldb_slice_remove_prefix(cldb_slice_t *s, size_t n);
```

Slice 不拥有内存；调用者必须保证在回调/调用期间底层数据有效。Cache deleter、Comparator、FilterPolicy 的 key 参数都是借用 Slice。

**Status**（公开，按值返回）：

```c
typedef enum cldb_status_code {
  CLDB_STATUS_OK = 0,
  CLDB_STATUS_NOT_FOUND = 1,
  CLDB_STATUS_CORRUPTION = 2,
  CLDB_STATUS_NOT_SUPPORTED = 3,
  CLDB_STATUS_INVALID_ARGUMENT = 4,
  CLDB_STATUS_IO_ERROR = 5
} cldb_status_code_t;

typedef struct cldb_status {
  cldb_status_code_t code;
  char message[256];   /* NUL-terminated, may be truncated */
} cldb_status_t;
```

- `cldb_status_ok/code/message`、`cldb_status_set`、`cldb_status_io(...)` 等构造器；
- 非 OK 的 `code` 与上游数值一致，便于测试和日志；
- 消息只用于诊断，不参与比较/落盘；截断不影响语义；
- 结构体可安全按值拷贝，不需要 destroy。这是对上游“不可变堆状态”的明确简化。

**Buffer**（内部，替代 `std::string`）：

```c
typedef struct cldb_buf {
  char *data;       /* NUL-terminated for convenience */
  size_t size;      /* bytes used, excluding NUL */
  size_t capacity;  /* allocated bytes, including NUL */
} cldb_buf_t;
```

- `init/free/clear/reserve/resize/append/append_char/append_slice/assign/swap`；
- 所有增长检查 `SIZE_MAX` 溢出；失败返回 false，调用者转成 status；
- `cldb_buf_slice()` 返回借用 Slice；`cldb_buf_cstr()` 返回 NUL 字符串（路径用）；
- 支持自追加：先保存源 Slice，确认 realloc 后源指针可能失效则先拷贝或使用临时 buffer。

### 6.3 C++ 类/虚拟接口 → struct + vtable

上游接口（Env、Cache、Comparator、FilterPolicy、Iterator、SequentialFile 等）全部用“基类 + 函数指针表”实现：

```c
typedef struct cldb_iterator cldb_iterator_t;

typedef struct cldb_iterator_vtbl {
  bool (*valid)(const cldb_iterator_t *);
  void (*seek_to_first)(cldb_iterator_t *);
  void (*seek_to_last)(cldb_iterator_t *);
  void (*seek)(cldb_iterator_t *, cldb_slice_t target);
  void (*next)(cldb_iterator_t *);
  void (*prev)(cldb_iterator_t *);
  cldb_slice_t (*key)(const cldb_iterator_t *);
  cldb_slice_t (*value)(const cldb_iterator_t *);
  cldb_status_t (*status)(const cldb_iterator_t *);
  void (*destroy)(cldb_iterator_t *);
} cldb_iterator_vtbl_t;

struct cldb_iterator {
  const cldb_iterator_vtbl_t *vtbl;
  /* cleanup callback list */
  cldb_iter_cleanup_t *cleanup_head;
  void *cleanup_arg1;
  void *cleanup_arg2;
};
```

约定：

- 具体类型把 `cldb_iterator_t base` 放在**第一个成员**；vtable 方法用 `container_of`/强制转换取回外层对象。
- `destroy` 负责释放对象自身；`RegisterCleanup` 的回调在 `destroy` 内、释放子资源前执行（顺序与上游一致，测试固定）。
- Env/Cache/Comparator/FilterPolicy 相同模式；函数表通常用 `static const`。
- 不要用 `void*` 到处传递；每个接口有明确基类指针。
- 没有 RTTI/dynamic_cast：类型由创建函数和 vtable 保证，错误类型属于编程错误，用 assert 捕获。

### 6.4 模板/RAII/STL 的替代

| C++ | C 方案 |
|---|---|
| `std::string` | `cldb_buf_t` |
| `std::vector<T>` | 动态数组结构体（`T *data; size_t size, capacity;`）+ 泛型宏或按类型手写 |
| `std::deque<Writer*>` | 双向链表或环形缓冲；写者队列用链表即可 |
| `std::set<uint64_t>`（pending_outputs、live files） | 有序 `uint64_t` 动态数组（插入/查找/删除 O(n)）或小型哈希表；DB 规模下文件数不大 |
| `std::set`（VersionEdit deleted files） | 动态数组 + 线性查找；每次 edit 文件数有限 |
| `std::map` | 无直接需要；需要时用排序数组或哈希表 |
| `SkipList<T, Compare>` 模板 | 专门化 `cldb_skiplist`，key 固定为 `const char*`（指向 memtable 编码条目），比较函数指针或 `InternalKeyComparator*` |
| 构造函数/析构函数 | `_init/_destroy`；失败用 `_create` 返回 NULL + status |
| 异常 | 返回 status；`goto cleanup`；分配失败向调用者传播 |
| `std::unique_ptr` | 明确 owner + `_destroy`；不引入智能指针 |
| `std::mutex/condition_variable/thread` | `cldb_mutex_t`/`cldb_condvar_t`/`cldb_thread_t` 包装 pthread |
| `std::atomic` | C11 `<stdatomic.h>`（GCC/Clang） |
| 引用计数 | `uint32_t refs` + `_ref/_unref`，在文档规定的锁下操作；memtable/version 的 unref 可能跨线程，必要时用原子 |

**模板替代的具体策略**：不要为了“泛型”写复杂的宏；LevelDB 实际只用 `SkipList<const char*, KeyComparator>` 和少量容器。直接把 SkipList 写成 `cldb_skiplist`（内部存 `const char *key`，由调用者提供比较函数），既简单又贴近上游生成后的代码。

### 6.5 生命周期、所有权与错误处理

**规则 R1：每个结构体在头文件注释中写清“谁拥有每个指针”。**

**规则 R2：所有 `create` 函数成功时返回对象，失败返回 NULL 并填 status；所有 `destroy` 接受 NULL。**

**规则 R3：错误路径统一 `goto cleanup`：**

```c
cldb_status_t f(void) {
  cldb_status_t s = cldb_status_ok();
  cldb_buf_t b;
  cldb_buf_init(&b);
  if (!cldb_buf_reserve(&b, n)) {
    s = cldb_status_io("out of memory");
    goto cleanup;
  }
  ...
cleanup:
  cldb_buf_free(&b);
  return s;
}
```

- 不在 `goto` 前跳过初始化；所有资源初始化为 0/NULL；
- 一个函数只有一个返回点（除非极短的纯检查函数），便于静态检查；
- 失败路径必须保持对象可安全 `destroy`（部分构造状态可清理）。

**规则 R4：借用 vs 拥有**

- Slice、Iterator 的 key/value、Cache deleter 参数、Comparator 参数：借用，不释放；
- 返回值中通过 `char **value` 返回的字符串：调用者用 `cldb_free` 释放（公开 API 统一）；
- 文件/锁/cache handle/迭代器：调用者负责 destroy/release；
- 内部对象（MemTable/Version/Table）用 refcount，引用持有者负责 unref。

**规则 R5：分配失败**：内部小对象分配失败返回 NULL，向上转成 `CLDB_STATUS_IO_ERROR`（消息 "out of memory"）；不要静默继续。Arena 的大块分配失败同样返回状态；测试中可用 FaultEnv 注入。

**规则 R6：断言**：

- `CLDB_ASSERT` 仅在 debug 构建生效；
- 用于“不可能发生”的内部不变量（例如 `block_offset_ + kHeaderSize + length <= kBlockSize`）；
- 外部输入（损坏文件、用户参数）必须走 status，不用断言。

### 6.6 并发原语与平台层

```
src/port/
  port.h            Linux 平台检查、CLDB_ASSERT、缓存行/对齐辅助
  port_atomic.h     atomic_load/store/fetch_add/CAS 封装
  port_mutex.h/.c   cldb_mutex_t, cldb_condvar_t
  port_thread.h/.c  线程创建/join/detach、sleep、时间
  port_posix.c      Linux/POSIX 实现（pthread、C11 atomics）
```

- **平台检查**：`port.h` 定义 `CLDB_PLATFORM_LINUX`；在其他平台直接 `#error`，不维护多平台分支。
- **原子性**：使用 C11 `<stdatomic.h>`（GCC/Clang），提供 `memory_order_acquire/release/relaxed` 语义映射。
- **条件变量**：所有 wait 必须在 `while (!predicate)` 循环中，使用 `pthread_cond_wait`。
- **线程创建**：使用 `pthread_create`；需要 detach 的场景显式 `pthread_detach`。
- **锁注释**：用命名和注释表达上游的 `GUARDED_BY/REQUIRES`：`db->mutex_`、`_locked` 后缀、每个函数头写 `/* REQUIRES: db->mutex_ */`。
- **不引入线程局部存储**（除非测试需要）；错误通过返回值传播。

### 6.7 Env 设计

公开接口按上游一一对应，但用 C 名称：

```c
typedef struct cldb_env cldb_env_t;
typedef struct cldb_sequential_file cldb_sequential_file_t;
typedef struct cldb_random_access_file cldb_random_access_file_t;
typedef struct cldb_writable_file cldb_writable_file_t;
typedef struct cldb_logger cldb_logger_t;
typedef struct cldb_file_lock cldb_file_lock_t;

struct cldb_env_vtbl {
  cldb_status_t (*new_sequential_file)(cldb_env_t*, const char *fname,
                                       cldb_sequential_file_t **out);
  cldb_status_t (*new_random_access_file)(...);
  cldb_status_t (*new_writable_file)(...);
  cldb_status_t (*new_appendable_file)(...);
  bool (*file_exists)(cldb_env_t*, const char *fname);
  cldb_status_t (*get_children)(cldb_env_t*, const char *dir, cldb_strvec_t *out);
  cldb_status_t (*remove_file)(cldb_env_t*, const char *fname);
  cldb_status_t (*create_dir)(...);
  cldb_status_t (*remove_dir)(...);
  cldb_status_t (*get_file_size)(...);
  cldb_status_t (*rename_file)(...);
  cldb_status_t (*lock_file)(...);
  cldb_status_t (*unlock_file)(...);
  void (*schedule)(cldb_env_t*, void (*fn)(void*), void *arg);
  void (*start_thread)(cldb_env_t*, void (*fn)(void*), void *arg);
  cldb_status_t (*get_test_directory)(...);
  cldb_status_t (*new_logger)(...);
  uint64_t (*now_micros)(cldb_env_t*);
  void (*sleep_for_microseconds)(cldb_env_t*, int micros);
};
```

- `EnvWrapper` 用转发 vtable 实现（方便 FaultEnv/测试）。
- `WriteStringToFile` / `WriteStringToFileSync` / `ReadFileToString` 做成非虚辅助函数。
- Linux Env：`open` 带 `O_CLOEXEC`；`read`/`pread`/`write`；64 KB 写缓冲；`fdatasync`（不可用时回退 `fsync`）；`fcntl(F_SETLK)` + 进程内锁表；`opendir/readdir`；后台线程只启动一次，队列串行执行。`NowMicros` 使用 `gettimeofday`（不保证单调），`SleepForMicroseconds` 使用 `nanosleep` 并处理 EINTR。
- MemEnv：`FileState`（可增长 byte buffer）+ 文件表 + 锁表；所有 Env 方法在内存中实现；用于 log/version/table 测试。
- 路径编码：Linux 上按字节处理（NUL 结尾），不做 UTF-8 校验或编码转换；非 UTF-8 路径按原字节传递。
- 不移植 macOS 的 `F_FULLFSYNC` 分支和 Windows 的 `CreateFile`/`OVERLAPPED` 实现；首版 RandomAccessFile 只用 `pread`，不实现 mmap 优化。

### 6.8 公开 C API 草案

```c
/* include/cleveldb/cldb.h */
typedef struct cldb cldb_t;
typedef struct cldb_snapshot cldb_snapshot_t;
typedef struct cldb_iterator cldb_iterator_t;
typedef struct cldb_write_batch cldb_write_batch_t;

typedef struct cldb_options {
  const cldb_comparator_t *comparator;      /* NULL => bytewise */
  cldb_env_t *env;                          /* NULL => default */
  cldb_logger_t *info_log;                  /* borrowed; NULL => default LOG */
  size_t write_buffer_size;                 /* default 4 MB */
  int max_open_files;                       /* default 1000 */
  cldb_cache_t *block_cache;                /* NULL => 8 MB LRU */
  size_t block_size;                        /* default 4 KB */
  int block_restart_interval;               /* default 16 */
  size_t max_file_size;                     /* default 2 MB */
  cldb_compression_t compression;           /* default NO for C port */
  int zstd_compression_level;               /* default 1; unused if no zstd */
  bool reuse_logs;                          /* default false */
  const cldb_filter_policy_t *filter_policy;/* default NULL */
  bool paranoid_checks;                     /* default false */
  bool create_if_missing;                   /* default false */
  bool error_if_exists;                     /* default false */
} cldb_options_t;

void cldb_options_init(cldb_options_t *opt);

cldb_status_t cldb_open(const cldb_options_t *opt, const char *name, cldb_t **db);
void cldb_close(cldb_t *db);
cldb_status_t cldb_put(cldb_t *db, const cldb_write_options_t *wo,
                       cldb_slice_t key, cldb_slice_t value);
cldb_status_t cldb_delete(cldb_t *db, const cldb_write_options_t *wo,
                          cldb_slice_t key);
cldb_status_t cldb_write(cldb_t *db, const cldb_write_options_t *wo,
                         cldb_write_batch_t *batch);
cldb_status_t cldb_get(cldb_t *db, const cldb_read_options_t *ro,
                       cldb_slice_t key, char **value, size_t *value_size);
void cldb_free(void *p);
cldb_iterator_t *cldb_new_iterator(cldb_t *db, const cldb_read_options_t *ro);
/* iterator: valid/seek/next/prev/key/value/status/destroy */
const cldb_snapshot_t *cldb_get_snapshot(cldb_t *db);
void cldb_release_snapshot(cldb_t *db, const cldb_snapshot_t *snap);
void cldb_compact_range(cldb_t *db, cldb_slice_t *begin, cldb_slice_t *end);
bool cldb_get_property(cldb_t *db, const char *name, char **value, size_t *len);
void cldb_get_approximate_sizes(cldb_t *db, const cldb_range_t *ranges, int n,
                                uint64_t *sizes);
cldb_status_t cldb_destroy_db(const char *name, const cldb_options_t *opt);
cldb_status_t cldb_repair_db(const char *name, const cldb_options_t *opt);
```

细节：

- `cldb_get` 成功时 `*value` 由库分配，调用者 `cldb_free`；`NotFound` 时 `*value=NULL`。
- 迭代器错误（如文件损坏）通过 `status` 暴露；`valid()` 为 false 不代表错误，调用者应检查 `status`。
- `cldb_options_init` 填入与上游默认值一致的值（compression 除外，默认 NO）。
- 公开结构体只增不改；内部结构体不透明，避免 ABI 问题。
- 所有回调（Comparator/FilterPolicy/Cache deleter/Env）都带 `void *arg` 或对象指针，避免依赖全局变量。

### 6.9 目录结构（目标）

```
E:\github\cleveldb\
├─ CMakeLists.txt
├─ cmake/
├─ doc/
│  ├─ plan.md
│  ├─ tasks.md
│  └─ notes/
├─ include/cleveldb/
│  ├─ cldb.h              DB API / options / range
│  ├─ slice.h
│  ├─ status.h
│  ├─ comparator.h
│  ├─ filter_policy.h
│  ├─ cache.h
│  ├─ env.h
│  ├─ iterator.h
│  └─ write_batch.h
├─ src/
│  ├─ port/               port_posix.c、port_*.h/.c（仅 Linux）
│  ├─ util/               coding/hash/random/arena/crc32c/buf/status/
│  │                      logging/options/cache/bloom/env/iterator
│  ├─ db/                 filename/dbformat/write_batch/skiplist/memtable/
│  │                      log_*/version_*/table_cache/builder/db_iter/
│  │                      db_impl/snapshot/repair
│  ├─ table/              block_builder/block/filter_block/format/
│  │                      table_builder/table/two_level_iterator/merger/
│  │                      iterator_wrapper
│  └─ helpers/memenv/
├─ tests/
│  ├─ harness.h/.c
│  ├─ util/ db/ table/ compat/
│  └─ main.c
├─ tools/
│  ├─ cldb_dump.c
│  └─ cldb_bench.c
└─ examples/
   ├─ basic.c
   └─ custom_comparator.c
```

### 6.10 构建与工具链

- CMake ≥ 3.16，C11（`C_STANDARD 11`，`C_EXTENSIONS OFF`）。
- Linux 特性宏与链接：定义 `_GNU_SOURCE`（或等价的 `_POSIX_C_SOURCE=200809L` + `_DEFAULT_SOURCE`），确保 `pread`、`O_CLOEXEC`、`fdatasync`、`nanosleep`、`gettimeofday` 在严格 C11 下可见；链接 pthread（CMake `find_package(Threads REQUIRED)` + `Threads::Threads`）。
- 目标：
  - `cleveldb`（静态库，默认；可选 `BUILD_SHARED_LIBS`）
  - `cleveldb_tests`（CTest）
  - `cldb_dump`、`cldb_bench`（可选 target）
- 第三方依赖：默认仅 libc/pthread；`CLDB_WITH_SNAPPY`/`CLDB_WITH_ZSTD` 默认 OFF，开启时用 `find_path` + `find_library` 且要求头文件与库同时存在；不 vendor 第三方源码，不使用 C++ 测试/基准框架（详见 §1.6）。
- 编译选项（按编译器探测）：
  - GCC/Clang：`-Wall -Wextra -Wpedantic -Wshadow -Wconversion`（`-Wconversion` 可先警告不错误）、`-fno-common`；
  - Debug：`-O0 -g3 -DCLDB_DEBUG=1`；Release：`-O2 -DNDEBUG`；
  - Sanitizer：`-fsanitize=address,undefined`（Linux GCC/Clang）。
- 测试：CTest 注册多个测试可执行文件或单一 runner；每个测试可在 MemEnv 上运行，不污染源码目录。
- 代码格式：clang-format（LLVM 基风格，4 空格缩进，80/100 列二选一并固定）。
- 静态检查：clang-tidy（可选）；`-Werror` 仅在 CI 的专用 job 开启，避免本地工具链差异阻塞学习。
- CI 矩阵（仅 Linux）：GCC + Clang；可选 ASan/TSAN job。

### 6.11 编码规范（摘要）

- 类型：`intN_t/uintN_t`、`size_t`；文件大小/偏移用 `uint64_t`；序列号用 `uint64_t`（只用低 56 位）。
- 命名：公开 `cldb_*`；内部也统一 `cldb_*`，静态函数 `static`；结构体 `_t` 后缀；枚举 `CLDB_*`。
- 头文件保护：`CLEVELDB_<PATH>_H_`。
- 不写变长数组；不在栈上放 4 MB 缓冲；大块用堆/ Arena。
- 所有 `malloc/realloc/calloc` 结果检查；`free(NULL)` 允许。
- 所有整数运算在用于分配/偏移前检查溢出；`size_t` 与 `uint64_t` 转换显式。
- 禁止未定义行为：不要依赖有符号溢出、未对齐访问、严格别名；编码/解码逐字节。
- 日志中不打印原始二进制；用 `cldb_escape_string`。
- 每个 `.c` 顶部注释：对应上游文件、职责、锁要求、所有权。

---

## 7. SDD 阶段与里程碑

| 阶段 | 名称 | 核心产出 | 退出条件（Gate） |
|---|---|---|---|
| P0 | 脚手架 | CMake、测试框架、规范、上游基线记录 | 空项目可在 Linux 构建；测试 runner 可跑 |
| P1 | 基础库 | Slice/Status/Buffer、编码、哈希/随机、Arena、CRC32C、port、Comparator、Iterator、Bloom、LRU Cache、Env/MemEnv、文件名/日志/选项 | 所有 util 单测通过；MemEnv 可用于后续测试；格式基元有 golden 向量 |
| P2 | WAL | log format、Writer、Reader | 分片/边界/校验/重同步测试通过；与上游 WAL 字节兼容 |
| P3 | 内存引擎 | dbformat、WriteBatch、SkipList、MemTable、SnapshotList | 内存读写、序列、快照、并发插入测试通过 |
| P4 | SSTable | BlockBuilder/Block、FilterBlock、Footer/ReadBlock、TableBuilder、Table、TwoLevel/Merging Iterator | 自写自读、Seek/Prev、过滤器、CRC、损坏处理通过；单表 golden |
| P5 | 版本层 | FileMetaData/Version、VersionEdit、Builder、VersionSet、LogAndApply、Recover、Compaction 选择、TableCache | version_set 测试通过；MANIFEST/CURRENT 恢复与损坏测试通过 |
| P6 | DB 核心 | DBImpl、Open/Recover、写路径、读路径、迭代器、flush、后台 compaction、手工 compaction、属性、Destroy | 端到端 DB 测试通过；小 buffer 强制 compaction 后数据正确；快照语义正确 |
| P7 | 工具/修复 | Repair、dump、公开 API、示例、可选压缩 | repair 能救回可读数据；API 示例可运行；工具能解析 DB 文件 |
| P8 | 兼容/加固 | 上游测试全量移植、双向兼容、随机差分、sanitizer、bench | 兼容性矩阵通过；无内存错误；性能同数量级；学习验收完成 |

### 7.1 阶段门（Gate）通用检查

- [ ] 当前阶段所有任务 DONE（代码 + 测试 + note + 自检）。
- [ ] 无已知 P0/P1 缺陷；已知限制写入 plan/tasks 的“已知问题”。
- [ ] 阶段新增格式/行为有 golden test 或上游测试对应。
- [ ] 一次完整的 `ctest` 通过（Debug + Release 至少各一次）。
- [ ] 代码审查：所有权、锁、错误路径、溢出。
- [ ] 学习验收：随机抽 3 个本阶段问题，能脱稿回答并做一个小实验。

---

## 8. 测试与验证策略

### 8.1 单元测试（按模块）

直接移植/改写上游测试，保持用例名和断言语义：

| 模块 | 上游测试参考 | C 测试重点 |
|---|---|---|
| coding | `util/coding_test.cc` | varint 边界/截断/最大长度 |
| crc32c | `util/crc32c_test.cc` | 已知向量、增量、掩码 |
| hash/random | `util/hash_test.cc` + 自建 random 测试（上游无 random_test.cc） | 确定性、边界 |
| arena | `util/arena_test.cc` | 对齐、跨块、内存统计 |
| status | `util/status_test.cc` | 代码/消息/拷贝 |
| cache | `util/cache_test.cc` | refs/erase/prune/capacity/deleter |
| bloom | `util/bloom_test.cc` | 误判率、格式、名称 |
| env | `util/env_test.cc` | 文件/目录/锁/追加/重命名 |
| filename | `db/filename_test.cc` | 解析/生成/CURRENT |
| log | `db/log_test.cc` | 分片/校验/重同步/随机 |
| skiplist | `db/skiplist_test.cc` | 并发插入/查找/迭代 |
| dbformat | `db/dbformat_test.cc` | 排序/解析/比较器 |
| write_batch | `db/write_batch_test.cc` | 编码/合并/插入 |
| memtable | 自建（上游无 memtable_test.cc，行为由 db_test 覆盖） | Get/迭代/内存 |
| table | `table/table_test.cc` | 块/表/seek/approximate |
| version | `db/version_set_test.cc` | 选择/恢复/编辑 |
| db | `db/db_test.cc` | 端到端/快照/迭代/flush |
| recovery | `db/recovery_test.cc` | 日志恢复/复用 |
| corruption | `db/corruption_test.cc` | 截断/篡改/校验 |
| fault injection | `db/fault_injection_test.cc` | 注入失败/关闭 |

### 8.2 格式 Golden Test

对以下内容固定字节向量（hex dump 或文件哈希）：

- fixed/varint 编码；
- 内部键 + WriteBatch；
- 一条/多条 WAL 记录（含跨块、空记录、零填充）；
- 空表、单 key 表、多块表、带 filter 表：footer、index、block trailer；
- VersionEdit 全字段编码；
- Bloom/filter block 布局。

Golden 文件放在 `tests/golden/`，用只读方式打开；更新 golden 必须说明原因并双人/双次验证。

### 8.3 与上游的差分测试

分三层：

1. **编码级**：同一输入分别调用 C 与 C++ 的编码函数，逐字节比较（coding、VersionEdit、WAL、block、footer）。
2. **文件级**：用相同操作和相同 `Options`（`compression=kNoCompression`，固定 comparator）分别生成 DB；比较：
   - 逻辑内容：按 key 排序 dump（忽略文件号/日志时间）；
   - 兼容性：C++ 打开 C 生成的 DB，C 打开 C++ 生成的 DB；
   - 结构：报表文件名集合、层级、文件大小（允许因调度不同而不同）。
3. **随机模型**：同一随机种子生成 Put/Delete/WriteBatch/Get/迭代/快照操作序列，分别施加到 C 版和 C++ 版（或 C 版和内存参考模型），逐操作比较结果；定期重开 DB 验证恢复。

### 8.4 故障注入与崩溃恢复

- **FaultEnv**：包装 Env，在第 N 次 `NewWritableFile/Append/Sync/RenameFile/RemoveFile` 返回错误；验证错误传播、`bg_error_`、关闭清理。
- **截断**：对 WAL/表/MANIFEST 在任意偏移截断；验证 Reader 报告 corruption/丢弃尾记录，DB 仍能打开或返回明确错误。
- **篡改**：翻转 CRC 字段、length、type、footer magic、MANIFEST tag；验证校验和错误处理。
- **崩溃模拟**：在 LogAndApply 的各个阶段（写 MANIFEST 前/后、CURRENT rename 前/后）杀掉/模拟进程，重启验证 I-7/I-8。
- **孤立文件**：手动放入旧的 `.log/.ldb/.dbtmp`，验证 RemoveObsoleteFiles 的保留/删除规则。
- **同步失败**：Sync 返回错误时，后续写必须失败（`bg_error_`），不能继续假装成功。

### 8.5 并发、内存与静态检查

- 并发：多线程读者 + 单写者 + 后台 compaction；快照与迭代器跨越写操作；关闭时后台仍在工作。
- TSAN 跑 DB/迭代器/缓存测试；ASan/UBSan/Valgrind 至少覆盖一种，重点检查 Arena、Block 所有权、Cache handle、Iterator cleanup。
- 泄漏检查：MemEnv 测试中的文件/表/迭代器/快照全部释放；允许的进程级单例（默认 Env、默认 cache 由 DB 拥有）在文档中列出。
- Fuzz：对 log::Reader、Block、VersionEdit 解码、FilterBlock 解码做随机字节输入，要求不崩溃、不越界、返回错误。

### 8.6 性能验证

- 移植 `db_bench` 的核心场景：`fillrandom`、`readrandom`、`readseq`、`overwrite`、`seekrandom`。
- 固定参数（1M keys、16B key、100B value、write_buffer 4MB、cache 8MB）、同一台 Linux 机器、C++ 与 C 各跑 3 次取中位数。
- 目标是同数量级（例如 0.5×–2×），并解释差异来源（C 容器实现、没有 Snappy、系统调用差异）。
- 不在正确性完成前做微优化；优化必须附 before/after 数据。

### 8.7 测试门禁（每个任务/阶段）

- 任务级：新增测试全过 + 相关旧测试全过 + sanitizer 无新报告。
- 阶段级：完整 ctest + 至少一个差分/兼容测试 + 损坏测试。
- 发布级（P8）：双向兼容矩阵、随机差分、长时间压测、内存检查、benchmark 报告、学习验收。

---

## 9. 风险登记册

| 风险 | 概率 | 影响 | 应对 |
|---|---|---|---|
| C 内存/所有权错误导致难查崩溃 | 高 | 高 | 统一所有权规则；ASan；每任务 note 写 owned/borrowed；小步提交 |
| 锁/条件变量错误导致死锁或丢唤醒 | 中 | 高 | 单 mutex 设计；固定锁顺序；谓词循环；TSAN/压力测试 |
| 磁盘格式细节偏差（CRC 范围、footer 填充、tag 编码） | 中 | 高 | 先写 golden + 与上游逐字节 diff；每格式单独任务 |
| 恢复/崩溃顺序理解错误 | 中 | 高 | 按 I-7/I-8 做阶段注入测试；重读 impl.md + db_impl.cc |
| compaction 删除规则导致快照读错 | 中 | 高 | 最小快照测试；规则 A/IsBaseLevelForKey 单元测试；随机模型 |
| Linux Env 语义差异（EINTR、fsync/fdatasync、fcntl 锁、rename 原子性） | 中 | 中 | 以 env_posix.cc 为参考；Linux 专用测试；文件系统前提文档化 |
| 文件系统差异（ext4/xfs/tmpfs/overlayfs 的 fsync/rename 语义） | 中 | 中 | 在目标文件系统上跑恢复/兼容测试；文档写明文件系统前提 |
| 压缩缺失导致互操作失败 | 高 | 中 | 明确测试选项 `kNoCompression`；压缩作为可选任务；启动时警告 |
| 范围蔓延（想一次做完） | 高 | 中 | 严格阶段门；每任务单一目标；非目标写入 plan |
| STL 替代容器复杂度退化 | 中 | 低/中 | 文件数有限；基准验证；必要时替换为哈希/有序结构 |
| 测试框架投入过多 | 低/中 | 低 | 只做最小 registry/断言；不实现 gtest 功能 |

---

## 10. 需求 → 任务追踪矩阵（摘要）

| 需求 | 任务 |
|---|---|
| FR-01 | T604 |
| FR-02 | T302, T605, T617 |
| FR-03 | T606 |
| FR-04 | T607, T608, T615, T616 |
| FR-05 | T305, T610, T616 |
| FR-06 | T201–T204, T603, T617 |
| FR-07 | T303–T306, T602 |
| FR-08 | T401–T409 |
| FR-09 | T501–T512 |
| FR-10 | T507–T512, T609–T612 |
| FR-11 | T111, T404 |
| FR-12 | T112, T509 |
| FR-13 | T613 |
| FR-14 | T614, T701 |
| FR-15 | T113–T115, T117 |
| FR-16 | T109, T111, T113, T117 |
| FR-17 | T802, T803 |
| FR-18 | T702, T805 |
| FR-19 | T703 |
| FR-20 | T601, T609, T617 |
| NFR-01/03 | T204, T506, T617, T803 |
| NFR-02 | T303, T616, T804 |
| NFR-04 | T112, T509, T613 |
| NFR-05 | 全阶段 + T804 |
| NFR-06 | T002, T115, T617 |
| NFR-07 | T001, T114, T804 |
| NFR-08 | T805 |

---

## 附录 A：上游 → C 文件映射

| 上游 | C 目标 | 说明 |
|---|---|---|
| `include/leveldb/slice.h` | `include/cleveldb/slice.h` | 公开 |
| `include/leveldb/status.h`, `util/status.cc` | `include/cleveldb/status.h`, `src/util/status.c` | 内联消息简化 |
| `include/leveldb/options.h`, `util/options.cc` | `include/cleveldb/options.h`, `src/util/options.c` | 默认值 |
| `include/leveldb/comparator.h`, `util/comparator.cc` | `include/cleveldb/comparator.h`, `src/util/comparator.c` | Bytewise + 名称 |
| `include/leveldb/filter_policy.h`, `util/bloom.cc` | `.../filter_policy.h`, `src/util/bloom.c` | 名称/算法一致 |
| `include/leveldb/cache.h`, `util/cache.cc` | `.../cache.h`, `src/util/cache.c` | 分片 LRU |
| `include/leveldb/env.h`, `util/env.cc` | `.../env.h`, `src/util/env.c` | 接口/辅助函数 |
| `util/env_posix.cc` | `src/port/port_posix.c`, `src/util/env_posix.c` | Linux/POSIX（唯一支持平台） |
| `helpers/memenv/memenv.cc` | `src/helpers/memenv/memenv.c` | 内存 Env |
| `util/coding.*` | `src/util/coding.c` | 编码 |
| `util/hash.*`, `util/random.h` | `src/util/hash.c`, `src/util/random.c` | 哈希/随机 |
| `util/arena.*` | `src/util/arena.c` | Arena |
| `util/crc32c.*`, `port/port_stdcxx.h` 的 crc 加速 | `src/util/crc32c.c` | 软件 + 可选 SSE4.2 |
| `util/logging.*` | `src/util/logging.c` | 格式化/转义 |
| `db/filename.*` | `src/db/filename.c` | 名称/解析 |
| `db/log_format.h`, `log_writer.*`, `log_reader.*` | `src/db/log_*.c` | WAL |
| `db/dbformat.*` | `src/db/dbformat.c` | 内部键/比较器 |
| `db/write_batch.*` | `src/db/write_batch.c` | 批量写 |
| `db/skiplist.h` | `src/db/skiplist.c` | 专门化 |
| `db/memtable.*` | `src/db/memtable.c` | 内存表 |
| `db/snapshot.h` | `src/db/snapshot.c` | 快照链表 |
| `table/block_builder.*` | `src/table/block_builder.c` | 块构造 |
| `table/block.*` | `src/table/block.c` | 块读取/迭代 |
| `table/filter_block.*` | `src/table/filter_block.c` | 过滤器块 |
| `table/format.*` | `src/table/format.c` | handle/footer/ReadBlock |
| `table/table_builder.*` | `src/table/table_builder.c` | 表构造 |
| `table/table.*` | `src/table/table.c` | 表读取/缓存 |
| `table/two_level_iterator.*` | `src/table/two_level_iterator.c` | 两层迭代 |
| `table/merger.*`, `table/iterator.*`, `iterator_wrapper.h` | `src/table/merger.c`, `src/util/iterator.c` | 合并/基类 |
| `db/version_edit.*` | `src/db/version_edit.c` | 编辑 |
| `db/version_set.*` | `src/db/version_set.c` | 版本/压缩选择 |
| `db/table_cache.*` | `src/db/table_cache.c` | 表缓存 |
| `db/builder.*` | `src/db/builder.c` | MemTable→Table |
| `db/db_iter.*` | `src/db/db_iter.c` | 用户迭代器 |
| `db/db_impl.*` | `src/db/db_impl.c` | DB 状态机 |
| `db/repair.cc` | `src/db/repair.c` | 修复 |
| `db/dumpfile.cc` | `tools/cldb_dump.c` | dump 工具 |
| `benchmarks/db_bench.cc` | `tools/cldb_bench.c` | 基准 |

## 附录 B：常量/默认值速查

```
kNumLevels = 7
kL0_CompactionTrigger = 4
kL0_SlowdownWritesTrigger = 8
kL0_StopWritesTrigger = 12
kMaxMemCompactLevel = 2
kReadBytesPeriod = 1048576
kMaxSequenceNumber = (1ULL << 56) - 1

log::kBlockSize = 32768
log::kHeaderSize = 7
RecordType: Zero=0, Full=1, First=2, Middle=3, Last=4

BlockHandle::kMaxEncodedLength = 20
Footer::kEncodedLength = 48
kTableMagicNumber = 0xdb4775248b80fb57
kBlockTrailerSize = 5
table compression: None=0, Snappy=1, Zstd=2

filter: kFilterBaseLg = 11 (2 KB), bloom k = floor(bits*0.69), clamp [1,30], min 64 bits
bloom hash seed = 0xbc9f1d34, policy name = "leveldb.BuiltinBloomFilter2"

Options defaults (上游):
  write_buffer_size = 4 MB
  max_open_files = 1000
  block_cache = NULL (DB 内部 8 MB)
  block_size = 4096
  block_restart_interval = 16
  max_file_size = 2 MB
  compression = kSnappyCompression（C 版首版 = kNoCompression）
  zstd_compression_level = 1
  reuse_logs = false
  paranoid_checks = false
  create_if_missing = false
  error_if_exists = false
ReadOptions: verify_checksums=false, fill_cache=true, snapshot=NULL
WriteOptions: sync=false
```

## 附录 C：任务笔记模板

每个任务在 `doc/notes/Txxx.md` 使用：

```markdown
# Txxx：任务名

## 1. 上游阅读
- 文件/函数：
- 调用链：
- 关键常量：

## 2. 原理摘要（合上源码写）
...

## 3. C 设计
- 数据结构：
- 所有权：
- 锁：
- 错误路径：

## 4. 字节格式/状态图
（贴 hexdump、ASCII 图）

## 5. 测试
- 用例：
- 边界/错误：
- sanitizer：

## 6. 实验
- 改了什么：
- 观察结果：
- 为什么：

## 7. 自检问题与答案
1. ...
2. ...

## 8. 遗留问题/后续任务
...
```

---

## 结语

这份 plan 是“规格 + 架构 + 约束”的基线，`tasks.md` 是执行顺序。整个移植过程中，**遇到任何与上游行为不一致的地方，先判断它属于实现 bug、允许的简化，还是格式差异；前者必须修，后者必须记录并加测试**。每个任务完成后写 note、跑测试、做实验、回答问题——这比“把代码敲完”更重要。
