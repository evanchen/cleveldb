# cleveldb 任务分解（tasks.md）

> 配套文档：`doc/plan.md`（架构、格式规范、C 设计、测试策略）。
>
> 上游基线：`E:\github\leveldb`，`1.23-93-gbb74ef7`（commit `bb74ef7`）。
>
> 平台约束：仅支持 Linux（GCC/Clang + pthread + POSIX）；不实现 Windows/macOS 分支。
>
> 使用方式：
> 1. 严格按依赖顺序执行；每个阶段门（Gate）通过后再进入下一阶段。
> 2. 一次只做一个任务；完成后在 Debian 上构建并运行本任务相关测试（含 ASan/UBSan），审阅本次 `git diff`/新增代码，理解并回答“吃透检查点”，再进入下一任务。
> 3. 全局 DoD（见 plan.md §1.4）适用于每个任务：代码 + 测试 + 增量审阅 + sanitizer + 自检。
> 4. 任务中的“上游”是必读代码，不是可选参考；先读懂再写 C。
> 5. 状态标记：`TODO` / `DOING` / `DONE` / `BLOCKED`。建议在任务标题后手工维护。
> 6. 每个任务保持独立、可审阅的增量改动，不要把多个任务混在一个大改动里；这样在 Debian 上跑完后可以直接看本任务的 diff。

---

## 阶段总览

```
P0 脚手架（T001-T004）
   │
P1 基础库（T101-T117）
   ├──────────────┬────────────────┐
P2 WAL（T201-T204）  P3 内存引擎（T301-T306）  P4 SSTable（T401-T409）
   └──────────────┴────────────────┘
                  │
P5 版本/压缩选择（T501-T512）
                  │
P6 DB 核心（T601-T618）
                  │
P7 工具/修复/API（T701-T707）
                  │
P8 兼容/加固/验收（T801-T806）
```

- P1 的 Env/MemEnv/测试框架是后续所有模块的基础。
- P2、P3、P4 之间大部分可并行；但 P3 的 `dbformat` 与 P4 的 `InternalKeyComparator` 共用，建议先完成 T301。
- P5 依赖 P3（MemTable/FileMetaData）与 P4（Table/TableCache 接口）。
- P6 依赖 P1–P5 全部；不要在 P6 里补前面的格式 bug。

## 任务模板

每个任务固定包含：

- **依赖**：前置任务。
- **上游**：必须阅读的文件/函数。
- **目标**：本任务要建立的能力。
- **设计/交付**：C 数据结构、接口、所有权、错误路径要点。
- **验收**：可执行测试/检查。
- **吃透检查点**：完成后应能脱稿回答的问题。
- **实验**：至少一个可观察行为的小实验（部分任务合并到检查点）。

---

# P0：工程脚手架

## T001 工程骨架与 Make

- **依赖**：无
- **上游**：LevelDB 的目录组织方式与构建选项（仅参考，不复制 C++ 构建逻辑）
- **目标**：建立可重复构建的 C11 项目骨架，后续任务只加文件，不改构建结构。
- **设计/交付**：
  - 目录：`include/cleveldb/`、`src/{port,util,db,table,helpers/memenv}/`、`tests/`、`tools/`、`examples/`。
  - Make 目标：`lib`（静态库 `libcleveldb.a`）、`tests`（`cleveldb_tests`）、可选 `tools`（`cldb_dump`/`cldb_bench`）；`-std=c11`。
  - 默认不引入任何第三方库；Snappy/Zstd 只作为 T704 的可选 Make 变量 `CLDB_WITH_SNAPPY`/`CLDB_WITH_ZSTD`（默认 0）。测试框架和基准自建，不依赖 GoogleTest/Google Benchmark。
  - 自动收集 `src/**/*.c`；公开头文件仅从 `include/` 搜索；内部头文件不安装。
  - 平台检测：定义 `CLDB_PLATFORM_LINUX`；Makefile 入口与 `#error` 在非 Linux 平台明确拒绝（本库只支持 Linux）。
  - Linux 特性宏：`_GNU_SOURCE`；链接 pthread（`-pthread`）。
  - 编译选项：GCC/Clang `-Wall -Wextra -Wpedantic -Wshadow`；`BUILD=debug|release` 切换配置。
  - `README.md` 写明 Linux 构建命令、测试命令、目录约定。
- **验收**：
  - 在 Linux 上用 GCC 与 Clang 各成功构建一次。
  - `make test` 能运行一个占位测试；Release/Debug 均可。
  - 默认配置不要求安装 Snappy/Zstd/GoogleTest/Google Benchmark；库和测试程序只链接 libc/pthread，不出现 libstdc++。
  - 无未使用变量/隐式函数声明等基础警告。
- **吃透检查点**：
  - 为什么库优先做成静态库？共享库会带来哪些额外问题？
  - 为什么公开头文件与内部头文件要物理分离？
  - Debug 断言与生产错误处理的边界在哪里？
- **实验**：分别用 Debug/Release 编译一个含 `assert(0)` 的占位测试，观察行为差异。

## T002 轻量测试框架

- **依赖**：T001
- **上游**：`util/testharness.h/.cc`（理解 gtest 风格断言与测试注册方式）
- **目标**：提供无第三方依赖、可在 C 中注册/过滤/断言的测试框架。
- **设计/交付**：
  - `tests/harness.h/.c`：
    - 显式注册表：每个测试文件提供 `void cldb_register_xxx_tests(void)`，在 `tests/main.c` 中调用；避免依赖平台 constructor。
    - `CLDB_TEST(name)` 定义测试函数；`CLDB_RUN_TEST` 注册。
    - 断言：`CLDB_ASSERT_TRUE/FALSE`、`CLDB_ASSERT_EQ_INT/UINT/PTR/STR`；失败记录文件/行号并继续（EXPECT）或终止当前测试（ASSERT）。OK/STATUS 断言依赖状态类型，延后到 T102。
    - 命令行：`--filter=substr`、`--list`、`--repeat=N`；返回非零表示失败。
    - 临时目录辅助延后到 T114（依赖 `cldb_env` 的 `GetTestDirectory`）。
  - `make test`/`make check` 直接运行 `cleveldb_tests`。
- **验收**：
  - `make check` 一次覆盖完整、过滤、列表、重复与故意失败路径。
  - 至少 3 个通过/失败/过滤示例；故意失败时退出码非零且输出定位。
  - 在 Linux 上输出格式稳定、可解析（GCC/Clang 构建一致）。
  - 断言宏不吞掉副作用（例如只求值一次）。
  - ASSERT 通过 longjmp 终止当前测试，后续测试与汇总仍正常执行。
- **吃透检查点**：
  - 如何测试 `static` 函数？为什么不需要把内部符号导出？
  - 为什么注册表比平台 constructor 更可移植？
  - 失败后继续执行与立即终止分别适用于什么场景？
- **实验**：用 `--repeat=100` 跑一个依赖随机种子的测试，观察可重复性。

## T003 C 编码规范与所有权规则

- **依赖**：T001, T002
- **上游**：上游头文件的注释风格、`port/thread_annotations.h` 的锁标注理念
- **目标**：把 plan.md §6 的设计约束落成可执行的检查和文档。
- **设计/交付**：
  - `doc/coding-style.md`：命名、头文件保护、类型、整数溢出、错误传播、`goto cleanup`、锁注释、日志、禁用项（VLA、隐式转换、未对齐访问）。
  - `doc/ownership.md`：每个基础类型的释放责任与引用期限规则；create/destroy 约定；公开 API 的 `cldb_free` 规则。
  - `.clang-format`：Google 基风格、2 空格、80 列、指针右对齐。
  - 示例模块 `src/util/example.{h,c}`：create/destroy、错误返回、`goto cleanup`、锁注释、测试。status 尚未实现，先用本地 result 枚举，T102 再迁移。
- **验收**：
  - 示例模块通过 T002 测试；代码审查清单可逐条勾选。
  - 文档明确回答：分配失败如何处理、Slice 生命周期、回调参数借用规则。
  - 示例覆盖参数错误与整数溢出；文档写清借用窗口和部分构造清理规则。
- **吃透检查点**：
  - 为什么一个函数只保留一个出口？哪些短函数可以例外？
  - 为什么错误消息不落盘、也不参与比较？
  - 内部引用计数与公开对象所有权如何区分？
- **实验**：按规范写一个故意在错误路径漏 free 的函数，用 ASan 找出并修复。

## T004 上游参考约定

- **依赖**：T001
- **上游**：`E:\github\leveldb`（只读参考）
- **目标**：约定如何对照上游源码，避免把上游构建流程引入本仓库。
- **约定**：
  - 上游源码固定在 `E:\github\leveldb`，commit `bb74ef7`；实现前直接阅读对应文件。
  - 不复制上游源码或头文件，不在本仓库添加上游构建脚本，不初始化/构建上游子模块。
  - 需要运行上游行为或产物时，由用户在自己的 Debian 环境独立编译。
  - 差分/兼容测试若需要上游产物，只通过参数传路径，不写入本仓库。
- **验收**：本仓库不含上游源码、构建脚本或构建产物；后续任务引用上游文件时以固定 commit 为准。
- **吃透检查点**：
  - 为什么参考实现与移植实现必须分离构建？
  - 为什么对照代码要固定 commit，而不是跟随 `main`？
- **实验**：无；上游测试由用户独立运行。

---

# P1：基础库

> 本阶段所有模块都应能独立单测；不要依赖 DBImpl。完成后 T117 做阶段门。

## T101 Slice 与 ByteBuffer

- **依赖**：T001, T002, T003
- **上游**：`include/leveldb/slice.h`；全仓库对 `std::string` 的使用（`util/coding.h`、`status.cc`、`logging.cc` 等）
- **目标**：实现按值传递的只读字节视图（Slice）和可变字节缓冲（替代 `std::string`）。
- **设计/交付**：
  - `include/cleveldb/slice.h`：`cldb_slice_t {const char *data; size_t size;}`；make/empty/is_empty/size/data/compare/equal/starts_with/remove_prefix。
  - `src/util/buf.h/.c`：`cldb_buf_t {char *data; size_t size; size_t capacity;}`；init/free/clear/reserve/resize/append/append_char/append_slice/assign/swap/slice/cstr。
  - 保证 `data[size]=='\0'`（方便路径/日志），但 Slice 语义不依赖 NUL。
  - 所有增长检查 `SIZE_MAX` 溢出；自追加必须安全（源指针可能因 realloc 失效）。
- **验收**：
  - 测试二进制内容（含 `\0`、高位字节）、空 Slice、比较的三种结果、前缀删除。
  - Buffer 扩容/清空/自追加/超大增长失败不崩溃；ASan 无越界。
  - 与上游 Slice 行为逐项对照（比较结果、空值）。
- **吃透检查点**：
  - Slice 为什么按值传递？它拥有内存吗？回调中能否保存 Slice 到回调之后？
  - 为什么 C 版需要自己的 Buffer，而不是直接用 `char*` + `strlen`？
  - 自追加时最危险的时序是什么？
- **实验**：构造一个 `Slice` 指向 Buffer，追加导致 realloc，观察悬垂指针；说明为什么 API 要求调用者管理生命周期。

## T102 Status

- **依赖**：T101
- **上游**：`include/leveldb/status.h`、`util/status.cc`
- **目标**：实现与上游错误码一致、可安全拷贝、消息可诊断的状态值。
- **设计/交付**：
  - 枚举值严格对应：OK=0, NotFound=1, Corruption=2, NotSupported=3, InvalidArgument=4, IOError=5。
  - `cldb_status_t {code; char message[256];}`；构造器 `ok/not_found/corruption/not_supported/invalid_argument/io`，带 `context` 与可选 `detail`，格式 `context: detail`。
  - `cldb_status_to_string`（写入调用者 buffer）、`code/message/is_*`。
  - 测试辅助 `tests/status_test_util.h`：`CLDB_EXPECT_OK/ASSERT_OK`、`CLDB_EXPECT_STATUS/ASSERT_STATUS`；只求值一次，失败时打印 code 与 message。
  - 把 T003 示例模块的本地 result 枚举迁移为 `cldb_status_t`，同步更新测试与文档。
  - 阅读上游状态内部格式：4 字节长度 + 1 字节 code + 消息；解释上游为何需要析构和深拷贝，C 版内联为何无需 destroy。
- **验收**：
  - 每个错误码的构造/转换/拷贝测试；长消息截断后仍以 NUL 结尾。
  - 与上游 `Status::ToString()` 的前缀语义对照（`NotFound: `、`Corruption: `、`IO error: ` 等）。
  - 结构体 copy 后独立可用（不共享堆指针）。
  - 状态断言宏能区分 OK 与非 OK，且不重复求值。
- **吃透检查点**：
  - 上游 `Status` 为什么可以按值拷贝而无需引用计数？它是否泄漏内存？为什么可以接受？
  - 为什么错误码是稳定契约，而错误文本不是？
  - 内联定长消息在什么情况下会丢信息？如何避免关键上下文被截断？
- **实验**：用一个 1 KB 文件名构造错误，观察截断；思考生产环境如何平衡诊断与内存。

## T103 编码：fixed/varint/length-prefixed

- **依赖**：T101, T102
- **上游**：`util/coding.h`、`util/coding.cc`
- **目标**：实现所有落盘数字编码，逐字节与上游一致。
- **设计/交付**：
  - fixed32/fixed64 小端逐字节 encode/decode。
  - varint32/varint64：`EncodeVarint*`、`PutVarint*(buf)`、`GetVarint*Ptr(p,limit,*v)`、`GetVarint*(Slice*, *v)`、`VarintLength`。
  - length-prefixed Slice：`PutLengthPrefixedSlice`、`GetLengthPrefixedSlice`。
  - 所有解码必须边界安全；指针版不越界；失败返回 NULL/false。
  - 保留 `GetVarint32Ptr` 的快速路径语义（单字节直接返回）。
- **验收**：
  - 边界值：0、127/128、16383/16384、2^21、2^28、UINT32_MAX、UINT64_MAX。
  - 截断/超长（>5 字节 32 位、>10 字节 64 位）/非规范编码的测试。
  - Golden：固定输入 hexdump；与上游编码逐字节比较。
- **吃透检查点**：
  - 为什么 LevelDB 固定数字用小端？这是否依赖主机端序？
  - varint32 最多 5 字节、varint64 最多 10 字节的推导。
  - 解码时 `shift` 的边界为什么是 `<=28` / `<=63`？多移一位会怎样？
  - `GetVarint32Ptr` 的快速路径为什么安全？
- **实验**：把编码器改成大端（仅测试分支），观察哪些格式测试失败；恢复。

## T104 Hash 与 Random

- **依赖**：T103
- **上游**：`util/hash.h/.cc`、`util/random.h`
- **目标**：实现 LevelDB 内部哈希与 Park–Miller 随机数，保证确定性。
- **设计/交付**：
  - `cldb_hash(data, n, seed)`：与上游相同的 murmur 风格 32 位哈希。
  - `cldb_random_t {uint32_t seed;}`：Next（`seed*16807 % (2^31-1)`）、Uniform、OneIn、Skewed；种子 0/M 修正为 1。
  - 所有乘法在 64 位中进行，避免溢出 UB。
- **验收**：
  - 固定 seed/输入序列与上游一致；边界 seed（0、2147483647）修正。
  - `Uniform(n)` 范围、`OneIn` 粗略频率、`Skewed` 分布测试。
  - 哈希对空串、非 4 字节尾部、任意二进制数据稳定。
- **吃透检查点**：
  - 为什么哈希最后要 `h ^= h >> 16`/`h ^= h >> 24`？
  - Park–Miller 的 `(product >> 31) + (product & M)` 为什么等价于模 M？
  - 为什么 RNG 种子不能为 0 或 M？
  - `Skewed` 的指数偏置在哪些地方有用？
- **实验**：统计 10 万次 `Skewed(nonzero)` 的分布，画直方图（文本即可）。

## T105 Arena

- **依赖**：T102, T103
- **上游**：`util/arena.h/.cc`
- **目标**：实现只增不减、按块分配的内存池，支撑 MemTable/SkipList。
- **设计/交付**：
  - 块大小 4096；`alloc_ptr_/alloc_bytes_remaining_` 指向当前块剩余空间。
  - 小分配从当前块切分；大于 `block_size/4` 的分配单独成块（避免浪费）。
  - `allocate_aligned` 处理 8 字节/指针对齐；`memory_usage()` 统计所有块。
  - 不提供单对象 free；destroy 释放所有块。
- **验收**：
  - 大量不同大小分配、对齐检查、跨块边界；总使用量合理。
  - 超过 1/4 块的大分配不会挤占小分配；ASan 无越界。
  - 与上游 arena_test 的用例对应。
- **吃透检查点**：
  - 为什么 MemTable 用 Arena 而不是逐个 malloc？带来的限制是什么？
  - 为什么大分配要单独成块？阈值为什么是 1/4？
  - Arena 的内存统计包含哪些开销？为什么不是精确的“用户字节数”？
- **实验**：分配 1 万个 32B 对象，打印块数/实际使用/统计值，解释差异。

## T106 CRC32C

- **依赖**：T103
- **上游**：`util/crc32c.h/.cc`、`port/port_stdcxx.h` 中的加速接口
- **目标**：实现 CRC-32C（Castagnoli）的增量计算、掩码/解掩码；保证与上游逐位一致。
- **设计/交付**：
  - `cldb_crc32c_value(data,n)`、`cldb_crc32c_extend(crc,data,n)`、`mask/unmask`。
  - 首版用查表实现（可直接移植上游 byte 表 + stride 表；保留版权头）。
  - 可选：SSE4.2 加速路径，运行时检测；结果必须与软件路径一致。
  - `Mask(crc) = ((crc >> 15) | (crc << 17)) + 0xa282ead8`。
- **验收**：
  - 上游 `crc32c_test` 的所有向量；空数据；增量拼接等于一次性计算。
  - 掩码/解掩码互逆（随机 crc）。
  - 大缓冲（>64 KB）与逐字节调用结果一致。
- **吃透检查点**：
  - CRC32C 与常见 CRC32 的多项式区别是什么？为什么 LevelDB 选它？
  - 为什么存储时要 Mask？它防的是什么错误？
  - `Extend` 的增量性质依赖什么数学性质？
  - 硬件加速不可用时如何保证结果一致？
- **实验**：对比软件表实现与 CPU 加速（若可用）在 1 MB 数据上的耗时与结果。

## T107 port 基础：Linux 断言与原子操作

- **依赖**：T001
- **上游**：`port/port_stdcxx.h`、`port/port.h`、`util/atomic_pointer.h`（若存在/历史版本）、skiplist 的原子用法
- **目标**：提供 Linux/POSIX 上语义明确的原子操作与调试断言。
- **设计/交付**：
  - `port.h`：`CLDB_PLATFORM_LINUX`、`#error` 拒绝非 Linux、`CLDB_ASSERT`、`CLDB_DEBUG`、`CLDB_UNUSED`、缓存行/对齐辅助。
  - `port_atomic.h`：`cldb_atomic_u32/u64/ptr`；load/store/fetch_add/compare_exchange；内存序参数或 acquire/release/relaxed 包装。
  - 使用 C11 `<stdatomic.h>`（GCC/Clang）；不引入 Windows 原子 API 或编译器专用兼容层。
  - 禁止依赖未定义行为的有符号溢出。
- **验收**：
  - 单线程语义测试；两线程计数器；发布/订阅测试（release store + acquire load）。
  - 在 Linux 上分别用 GCC/Clang 编译；内存序包装不会退化成错误语义。
  - 断言在 Debug 生效、Release 不生效。
- **吃透检查点**：
  - acquire/release 与 seq_cst 的区别，LevelDB 哪些地方只需要 relaxed？
  - 为什么 `has_imm_` 可以用 relaxed load，而 skiplist 的 `next_` 需要 acquire？
  - 编译器屏障与 CPU 屏障分别解决什么问题？
- **实验**：去掉发布端的 release（改成 relaxed），在弱内存模型/压力循环下观察是否读到未初始化数据（可能难复现，记录原因）。

## T108 port 同步：pthread Mutex/CondVar/Thread

- **依赖**：T107
- **上游**：`port/port_stdcxx.h`、`util/mutexlock.h`、`util/env_posix.cc` 的线程用法
- **目标**：基于 pthread 实现 C 版互斥量、条件变量、线程创建/等待/分离和 sleep。
- **设计/交付**：
  - `cldb_mutex_t`、`cldb_condvar_t`：init/destroy/lock/unlock/wait/signal/broadcast。
  - pthread：`pthread_mutex_t` + `pthread_cond_t`；默认非递归、错误检查语义明确。
  - `cldb_thread_t`：`pthread_create/join/detach`；sleep 毫秒/微秒（`nanosleep`，处理 EINTR）。
  - 提供 RAII 风格的 `CLDB_MUTEX_LOCK(m)` 宏或显式 lock/unlock，禁止提前 return 漏 unlock；统一 `goto cleanup`。
- **验收**：
  - 生产者/消费者测试；多线程计数；广播唤醒所有等待者。
  - 条件变量等待循环处理虚假唤醒。
  - 线程创建失败返回 status；join/detach 生命周期正确。
- **吃透检查点**：
  - 为什么条件变量必须放在 `while (!predicate)` 循环里？
  - 丢失唤醒（lost wakeup）如何产生？LevelDB 的写者队列如何避免？
  - pthread mutex 默认是非递归的；为什么 LevelDB 不需要递归锁？
  - 为什么后台线程要 detach，而测试线程可能 join？
- **实验**：写一个故意只用 `if` 而不用 `while` 的等待，用多线程压力尝试触发唤醒丢失（记录是否复现及原因）。

## T109 Comparator 与 Bytewise

- **依赖**：T101, T102
- **上游**：`include/leveldb/comparator.h`、`util/comparator.cc`
- **目标**：实现比较器接口和字节序比较器；名称必须与磁盘格式兼容。
- **设计/交付**：
  - vtable：compare、name、find_shortest_separator、find_short_successor。
  - Bytewise：按无符号字节字典序比较（`memcmp` + 长度）；空串小于非空。
  - `name` 返回常量 `"leveldb.BytewiseComparator"`。
  - separator：在 `start < limit` 的前提下，尝试缩短 start 且仍 `< limit`；successor：尝试让 start 变短但保持 `>= start`；两者都允许原样返回。
- **验收**：
  - 与上游 comparator 行为对照的边界用例：前缀、相同、空、二进制字节。
  - separator/successor 的性质测试：结果仍在合法区间；不改变排序。
  - Name 字符串逐字节一致。
- **吃透检查点**：
  - 为什么 MANIFEST 要保存 comparator 名称？名称不一致时打开 DB 会发生什么？
  - separator 缩短 key 的收益是什么？为什么必须保证 `< limit`？
  - successor 为什么可能返回原 key？调用方如何安全使用？
- **实验**：对一组索引键调用 separator/successor，比较缩短前后的表大小（T405 后可做真实测量）。

## T110 Iterator 基类、空/错误迭代器与 cleanup

- **依赖**：T101, T102
- **上游**：`include/leveldb/iterator.h`、`table/iterator.cc`
- **目标**：定义所有迭代器的公共接口、生命周期和清理回调机制。
- **设计/交付**：
  - vtable：valid/seek_to_first/seek_to_last/seek/next/prev/key/value/status/destroy。
  - 基类含 cleanup 链表；`cldb_iter_register_cleanup(it, fn, arg1, arg2)`；destroy 时执行（顺序与上游一致并测试固定）。
  - `cldb_new_empty_iterator()`、`cldb_new_error_iterator(status)`。
  - 规则：key/value 返回借用 Slice，直到下一次移动/销毁；status 只在迭代器失效时报告错误。
- **验收**：
  - 空迭代器行为；错误迭代器保留 status；cleanup 调用次数/参数/顺序。
  - 重复 destroy 禁止（文档 + assert）；未注册 cleanup 的销毁。
  - 用 cleanup 测试资源释放（malloc 计数）。
- **吃透检查点**：
  - 为什么用 `RegisterCleanup` 而不是让每个迭代器的析构函数知道 DB 引用？
  - cleanup 回调参数的生命周期由谁保证？
  - `valid()==false` 与 `status()!=OK` 的区别？调用者应如何检查？
- **实验**：在 cleanup 中记录顺序；对比注册 A→B 与 B→A 的执行顺序，并解释是否符合上游。

## T111 FilterPolicy 与 Bloom Filter

- **依赖**：T103, T104, T109
- **上游**：`include/leveldb/filter_policy.h`、`util/bloom.cc`、`util/bloom_test.cc`
- **目标**：实现布隆过滤器策略和精确的 LevelDB 格式/名称。
- **设计/交付**：
  - vtable：create_filter(keys[], n, dst)、key_may_match(key, filter)、name。
  - 算法：`k = floor(bits_per_key * 0.69)`，clamp [1,30]；至少 64 位；`h = Hash(key, 0xbc9f1d34)`；`delta = rotl(h,17)`；每次 `h += delta`；按位设置。
  - 名称 `"leveldb.BuiltinBloomFilter2"`；bits_per_key 默认 10。
  - 空 keys 的 filter 行为与上游一致（创建最小 filter/不匹配语义）。
- **验收**：
  - 误判率测试（大规模随机 key，bits_per_key 10/1/30）。
  - 边界：k 被 clamp、filter 小于 64 位、空 key、k>30 时 may_match 返回 true。
  - 与上游 filter 字节逐字节比较（固定输入）。
- **吃透检查点**：
  - 双哈希为什么能近似多个独立哈希？`delta` 为什么用循环右移 17？
  - 0.69 的来源和 clamp 上下限的原因。
  - 为什么 bloom 只能回答“可能在/一定不在”？过滤器损坏时为什么选择“可能匹配”？
  - policy 名称为什么必须持久化？
- **实验**：画出 bits_per_key 与误判率/内存的关系表，验证最优 k 附近。

## T112 分片 LRU Cache

- **依赖**：T101, T104, T108, T109
- **上游**：`include/leveldb/cache.h`、`util/cache.cc`、`util/cache_test.cc`
- **目标**：实现与上游语义一致的缓存（供 block cache/table cache 使用）。
- **设计/交付**：
  - Cache vtable：insert/lookup/release/value/erase/new_id/prune/set_capacity/total_charge。
  - 16 个 shard（`kNumShardBits=4`）；每 shard 独立 mutex、哈希表、循环双向 LRU 链表和 in-use 链表。
  - `LRUHandle` 思想：可变长 key 紧跟在结构体后；`refs`、`in_cache`、`charge`、`hash`。
  - refs 状态机；`Erase`/`Insert` 替换时延迟 deleter；容量 0 不缓存；`SetCapacity` 缩容并驱逐 LRU 尾部。
  - `NewId` 全局原子/加锁递增，供 block cache key 使用。
- **验收**：
  - Lookup/Release/Erase/Prune 的引用计数与 deleter 调用时机。
  - 容量驱逐顺序（LRU）、charge 统计、SetCapacity 缩容（含仍被客户端引用的条目）。
  - 并发 Lookup/Insert/Erase 压测；ASan/TSAN 无问题。
- **吃透检查点**：
  - `refs==1 && in_cache` 与 `refs>=2 && in_cache` 分别代表什么状态？两种链表为何不会同时在？
  - 为什么 cache 自己持有一个引用？deleter 为什么不能在 Erase 时立刻调用？
  - 为什么分 16 个 shard？容量如何分配到 shard？SetCapacity 时仍被引用的条目如何处理？
  - `Insert` 返回的 handle 与 cache 内部引用是什么关系？
- **实验**：插入超过容量的条目，记录驱逐顺序；调用 SetCapacity 缩小，观察仍被外部引用的条目是否保留。

## T113 Env 接口、Logger、EnvWrapper 与文件辅助

- **依赖**：T101, T102, T108
- **上游**：`include/leveldb/env.h`、`util/env.cc`
- **目标**：定义平台无关的 I/O/线程/时间接口和日志抽象。
- **设计/交付**：
  - 基类 + vtable：NewSequentialFile、NewRandomAccessFile、NewWritableFile、NewAppendableFile、FileExists、GetChildren、RemoveFile、CreateDir、RemoveDir、GetFileSize、RenameFile、LockFile、UnlockFile、Schedule、StartThread、GetTestDirectory、NewLogger、NowMicros、SleepForMicroseconds。
  - `RemoveFile/RemoveDir` 为主名，保留 `DeleteFile/DeleteDir` 兼容包装（内部互调，避免递归）。
  - SequentialFile（Read/Skip）、RandomAccessFile（Read(offset,n,scratch)）、WritableFile（Append/Close/Flush/Sync）、Logger（Logv）、FileLock。
  - `EnvWrapper`：持有 target，逐项转发；用于 FaultEnv/测试。
  - 非虚辅助：WriteStringToFile/Sync、ReadFileToString、Log(logger, fmt, ...)。
  - `cldb_env_default()`：进程级单例（Linux/POSIX，由 T114 提供实现）。
- **验收**：
  - 用 mock Env 验证所有转发；文件辅助函数的错误传播（创建失败、Append 失败、Close 失败、Sync 失败）。
  - `Log` 的 varargs 转发；NULL logger 安全。
  - 接口所有权：每个 New* 返回的对象由调用者 destroy。
- **吃透检查点**：
  - 为什么 SequentialFile 不要求线程安全，而 RandomAccessFile 要求？
  - `WritableFile::Flush` 与 `Sync` 的分工是什么？`Close` 是否隐含 Flush？
  - EnvWrapper 如何帮助故障注入？转发时哪些错误必须保留？
  - 为什么 `DeleteFile/DeleteDir` 需要兼容别名？它们和 Remove 的默认实现如何互调而不递归？
- **实验**：写一个 mock Env 让 `Append` 第 2 次失败，观察 `WriteStringToFile` 的错误返回和文件清理。

## T114 Linux Env：文件、目录、锁、Logger、后台线程与时间

- **依赖**：T113, T108
- **上游**：`util/env_posix.cc`、`util/posix_logger.h`（只保留 Linux 需要的分支）
- **目标**：在 Linux 上实现完整的默认 Env：文件 I/O、目录、文件锁、Logger、后台工作队列、线程和时间；这是唯一的真实平台 Env。
- **设计/交付**：
  - 文件：`open` 带 `O_CLOEXEC`；NewWritableFile/NewAppendableFile（`O_APPEND`）；顺序读 `read`（EINTR 重试）；随机读 `pread`（首版不实现 mmap 路径）；可写文件 64 KB 缓冲；Append/Close/Flush；Sync 使用 `fdatasync`（不可用时回退 `fsync`），不移植 macOS `F_FULLFSYNC` 分支。
  - 目录/文件操作：`opendir/readdir`（与上游 stock Env 一致，保留 `.`/`..`，由上层 `ParseFileName` 过滤）、`unlink`、`mkdir/rmdir`、`rename`、`stat`、`FileExists`、`GetChildren`、`GetFileSize`。
  - 文件锁：`fcntl(F_SETLK)` + 进程内已锁文件表；同一进程重复锁同一路径必须失败；Unlock 释放。
  - Logger：`FILE*` + `flockfile`/mutex；时间戳与 `posix_logger.h` 格式一致。
  - 后台队列：`Schedule` 用 mutex + condvar + FIFO 队列；首次调用按需创建后台线程并 detach；回调严格串行执行。`StartThread` 用 `pthread_create` + detach。
  - 时间：`NowMicros` 与上游一致使用 `gettimeofday`（不保证单调）；`SleepForMicroseconds` 用 `nanosleep` 并处理 EINTR。
  - `GetTestDirectory`：`TEST_TMPDIR` 或 `/tmp/leveldbtest-<euid>`；`cldb_env_default()` 进程级单例（不随进程退出销毁）。
  - 测试辅助 `tests/temp_dir.h/.c`：在 `GetTestDirectory` 下创建唯一子目录，测试结束递归清理。
  - 错误映射：ENOENT → NotFound，其余 errno → IOError（与 `env_posix.cc` 一致）；不引入 macOS/Windows 条件编译。
- **验收**：
  - 在 Linux 上移植并跑通完整 `env_test`：读写/追加/随机读/跳过/重命名/子目录/文件大小/锁冲突/测试目录。
  - `GetChildren` 与上游 stock Env 一致，包含 `.`/`..`；`DestroyDB` 测试据此期望 2 个条目。
  - 同一进程重复 LockFile 失败；第二个进程抢锁失败；Unlock 后可重新加锁。
  - 调度 1000 个任务：并发度为 1、顺序为 FIFO、空闲后仍能唤醒。
  - 时间测量误差可接受；默认 Env 重复获取返回同一指针。
  - 测试临时目录互不冲突，清理后不残留。
  - 只包含 Linux/POSIX 实现；非 Linux 平台在 Make 入口或编译期明确报错。
- **吃透检查点**：
  - `pread` 为什么天然适合 RandomAccessFile 的线程安全？
  - 上游 stock Env 的 `GetChildren` 为什么保留 `.`/`..`？谁负责过滤它们？
  - `fcntl` 锁在同进程内为什么不够？`flock` 与 `fcntl` 的差异？
  - 为什么 Linux 上首选 `fdatasync`？上游 `SetCurrentFile` 在 rename 后没有显式同步目录；掉电时可能发生什么？
  - LevelDB 为什么只用一个后台线程串行执行 `Schedule`？这对 compaction 有什么影响？
  - `NowMicros` 不单调会带来什么问题？哪些地方不能依赖它做超时判断？
  - 不做 mmap 优化会损失什么？为什么首版可以接受？
- **实验**：调度 1000 个任务，记录最大并发数与执行顺序；再用 `ulimit -n` 限制 fd 数量，观察 Env 打开文件失败时的错误传播（勿影响系统）。

## T115 MemEnv（内存文件系统）

- **依赖**：T113, T108
- **上游**：`helpers/memenv/memenv.cc`、`helpers/memenv/memenv_test.cc`
- **目标**：提供完全内存的 Env，供 log/version/table/DB 测试使用。
- **设计/交付**：
  - 文件表：路径 → `FileState {buf, refs, ...}`；支持创建/删除/重命名/子文件列表。`GetChildren` 只返回文件表条目，不返回 `.`/`..`（与 stock Env 不同，需在测试辅助层处理）。
  - SequentialFile（读位置）、RandomAccessFile（无状态）、WritableFile（追加/截断/同步 no-op）、AppendableFile。
  - LockFile：记录已锁路径，重复锁失败；Unlock 释放。
  - Logger：写入内存或 stderr（测试可配置）；NowMicros/Sleep；GetTestDirectory。
  - 生命周期与并发：内部互斥锁保护文件表；文件对象引用计数或复制必要状态。
- **验收**：
  - 移植 memenv_test：创建/写/读/追加/重命名/删除/目录列表/锁/随机读。
  - 重启模拟：释放 Env 前文件内容可被新 Env 实例读取（若设计为持久化）；否则明确 MemEnv 生命周期。
  - 作为 T202/T203/T505/T506 测试环境可用。
- **吃透检查点**：
  - MemEnv 与真实文件系统最容易出现语义差异的地方（fsync、rename 原子性、子目录、`.`/`..`）？
  - 为什么测试要用 MemEnv？它不能替代哪些测试？
  - 文件对象与文件状态的所有权如何划分？
- **实验**：在 MemEnv 上写一个 WAL 并重放，观察无真实 I/O 的测试速度。

## T116 文件名、日志格式化与 Options 默认值

- **依赖**：T101, T102, T113
- **上游**：`db/filename.h/.cc`、`db/filename_test.cc`、`util/logging.h/.cc`、`include/leveldb/options.h`、`util/options.cc`
- **目标**：实现文件命名/解析、CURRENT 写入、数字/转义工具和选项默认值。
- **设计/交付**：
  - FileType：kLogFile、kDBLockFile、kTableFile、kDescriptorFile、kCurrentFile、kTempFile、kInfoLogFile。
  - 生成：`LogFileName`、`TableFileName`（.ldb）、`SSTTableFileName`（.sst）、`DescriptorFileName`、`CurrentFileName`、`LockFileName`、`TempFileName`、`InfoLogFileName`、`OldInfoLogFileName`。
  - `ParseFileName(name, *number, *type)`：严格匹配上游模式；拒绝路径分隔符、非法后缀。
  - `SetCurrentFile`：写临时文件 → Sync → Rename 到 CURRENT；内容为 MANIFEST basename + `\n`。
  - `ConsumeDecimalNumber`（溢出安全）、NumberToString、EscapeString、AppendEscapedStringTo。
  - Options 默认值与 plan.md 附录 B 一致；`cldb_options_init`。
- **验收**：
  - 移植 filename_test 全部用例；CURRENT 内容/原子替换测试。
  - 文件名边界：编号 0、999999、超大编号、`.sst`/`.ldb` 识别、伪文件名拒绝。
  - 日志转义对控制字符/高位字节；Options 默认值逐项断言。
- **吃透检查点**：
  - 为什么文件名要固定 6 位补零？解析为何又允许不足 6 位？
  - CURRENT 为什么写 basename 而不是完整路径？临时文件+rename 解决什么？
  - LOG 与 LOG.old 的轮转顺序；日志创建失败时 DB 如何继续？
  - 上游 `SetCurrentFile` 为什么只同步临时文件、不显式 fsync 目录？这会留下什么崩溃窗口？
  - C 版默认 compression 为什么改成 NO？这会破坏哪个测试前提？
- **实验**：手工构造 `MANIFEST-1`、`000001.log`、`000001.sst` 等文件名，跑 ParseFileName，检查与生成函数互逆。

## T117 P1 阶段门：基础层联调

- **依赖**：T101–T116
- **上游**：`util/*_test.cc`、`db/filename_test.cc`
- **目标**：确认基础库可以作为后续模块的可靠地基。
- **验收（全部满足）**：
  - 全部 P1 单测通过；Debug + Release 各一遍。
  - 在 Linux 上用 ASan/UBSan 跑完 P1 测试；无泄漏（允许的单例除外）。
  - MemEnv 可被外部测试使用；默认 Env 在 Linux 上可用。
  - 关键 golden：编码、CRC、Bloom、文件名、CURRENT 已固定。
  - 代码审查：每个模块头文件写明所有权、错误路径和锁要求。
  - 非 Linux 平台在 Make 入口或编译期被明确拒绝。
- **吃透检查点（抽样）**：
  - 从 `cldb_buf_t` 到 `cldb_slice_t` 的所有权转换是否有悬垂风险？
  - Cache 的 deleter 与 Iterator cleanup 在资源释放顺序上的异同？
  - Env 抽象为什么能让后续所有文件 I/O 可测试？
  - 只支持 Linux 之后，Env 和构建系统具体简化了哪些分支？代价是什么？
- **实验**：用 MemEnv 跑一个“写 1000 条随机记录到 WAL 再读回”的小程序（可先用 T202/T203 的雏形），观察无磁盘路径。

# P2：WAL（Write-Ahead Log）

## T201 Log Format 常量与记录类型

- **依赖**：T103, T106
- **上游**：`db/log_format.h`、`db/log_writer.cc::InitTypeCrc`
- **目标**：固定 WAL 的物理布局和类型语义，为 Writer/Reader 提供共同定义。
- **设计/交付**：
  - `kBlockSize=32768`、`kHeaderSize=7`、`kMaxRecordType=4`。
  - RecordType：Zero=0、Full=1、First=2、Middle=3、Last=4。
  - 预计算每种 type 的 CRC 初值（对单字节 type 求 CRC），或提供等价函数。
  - 物理 record：`fixed32 masked_crc` + `uint16 length` + `uint8 type` + payload。
  - 文档图：一个 block 内多条 record、跨 block 分片、尾部零填充。
- **验收**：
  - 常量/枚举值与上游一致；type CRC 值与上游 `InitTypeCrc` 一致。
  - 一个手写 hexdump 的解析测试（固定字节，不依赖 Writer）。
- **吃透检查点**：
  - 为什么每个**物理分片**都单独校验，而不是整条逻辑记录一个 CRC？
  - 为什么 CRC 覆盖 type 字节？只覆盖 payload 会漏掉什么错误？
  - 32 KB block 的选择与写放大/恢复粒度有什么关系？
- **实验**：手写一个 FULL 记录（type=1，payload="hello"）的 12 字节 hexdump，用 Reader（T203 后）验证。

## T202 LogWriter

- **依赖**：T201, T113
- **上游**：`db/log_writer.h/.cc`、`db/log_test.cc` 的写入用例
- **目标**：实现记录分片、块边界处理、CRC 和普通/追加模式。
- **设计/交付**：
  - `cldb_log_writer_t {cldb_writable_file_t *dest; int block_offset; uint32_t type_crc[5];}`。
  - `create(dest)` 与 `create_with_length(dest, dest_length)`（后者用于 reuse_logs，block_offset = dest_length % 32768）。
  - `add_record(slice)`：循环切分；block 剩余 <7 时写零填充并换块；计算 FULL/FIRST/MIDDLE/LAST；空记录也写一条 FULL。
  - `emit_physical_record`：构造 7 字节头，CRC over type+payload，Mask；Append header+payload；调用 `Flush`；更新 block_offset。
  - Writer 不拥有 dest；不负责 Sync（由 DBImpl 在需要时调用）。
- **验收**：
  - 长度 0、1、7、32760、32761、32768、100000 的记录；跨块类型序列正确。
  - block 尾部剩余 0–7 字节的行为；dest_length 非零时的分片位置。
  - 与上游固定输入 golden 逐字节一致；错误传播（dest Append/Flush 失败）。
- **吃透检查点**：
  - 为什么 block 剩余 <7 字节要补零，而不是继续拆分？
  - 空记录为什么要产生 FULL(length=0)？恢复时如何区分“空记录”和“块尾填充”？
  - 为什么每条物理记录后调用 `Flush` 而不是等 `Close`/`Sync`？
  - `length` 是 uint16，为什么单条记录仍可能超过 65535 字节？
- **实验**：用不同长度的记录填满两个 block，hexdump 观察零填充、FULL/FIRST/LAST 和 CRC 字段。

## T203 LogReader

- **依赖**：T202, T115
- **上游**：`db/log_reader.h/.cc`、`db/log_test.cc`
- **目标**：实现健壮的记录读取、CRC 校验、分片重组、初始偏移和损坏重同步。
- **设计/交付**：
  - Reader 字段：sequential file、reporter、checksum 开关、initial_offset、buffer、eof、last_record_offset、end_of_buffer_offset、fragmented record 状态、resyncing。
  - `read_record(record, scratch)`：填 buffer → 循环读物理记录 → FULL 直接返回；FIRST 开始累积；MIDDLE 追加；LAST 结束；类型不合法报告 corruption。
  - `read_physical_record`：块边界处理（跳过填充）、长度校验、CRC（`Unmask` 后比较）、EOF/截断处理。
  - `skip_to_initial_block`：根据 initial_offset 定位到对应 block；进入 resync 模式时寻找下一个 FULL/FIRST。
  - reporter 回调：`(bytes_dropped, status)`；reporter 生命周期由调用者保证。
  - `last_record_offset` / `end_of_buffer_offset` 语义。
- **验收**：
  - 写读往返（含随机长度、跨块、空记录）逐条相等。
  - 初始偏移（block 中间/记录中间/末尾）行为合理；损坏后重同步。
  - 每种损坏：错误 CRC、错误 type、超长 length、截断 payload、块尾垃圾、LAST 缺少 FIRST；不崩溃且报告。
  - 移植 `log_test` 的核心用例；随机 fuzz 不越界。
- **吃透检查点**：
  - 为什么恢复 WAL 时即使 `paranoid_checks=false` 也强制校验 CRC？
  - resync 模式如何决定跳过多少字节？只看 type 足够吗？
  - FIRST/MIDDLE/LAST 状态机遇到非法序列时为什么丢弃已积累片段？
  - initial_offset 为什么需要“跳到 block 开头”而不是精确字节？
  - `last_record_offset` 是谁的偏移？DB 哪部分会用到？
- **实验**：手工篡改一条记录的 CRC/length/type，记录 reporter 的 bytes_dropped 和 Reader 后续恢复位置。

## T204 P2 阶段门：WAL 差分、故障注入与验收

- **依赖**：T004, T203
- **上游**：`db/log_test.cc`、`db/log_writer.h`
- **目标**：证明 C 版 WAL 与上游字节兼容，并能从损坏中恢复到最后一条完整记录。
- **验收**：
  - 固定输入记录序列的 WAL 字节与上游 `log::Writer` 输出逐字节一致（用 C++ 辅助程序或预先保存的 golden）。
  - C 版 Reader 能读取上游生成的 WAL；上游 Reader 能读取 C 版 WAL（若 C++ 辅助可用）。
  - 截断/翻位/类型破坏的注入矩阵通过；恢复时只丢损坏尾部。
  - 相关测试纳入 `make check`；ASan/UBSan 无报告。
- **吃透检查点**：
  - 哪些差异会破坏 WAL 兼容性（CRC 范围、Mask、类型编号、块大小）？
  - 为什么“读到 EOF”不等于“文件完整”？
  - 如果一条逻辑记录跨三个 block，恢复时第二块损坏，读者应保留什么？
- **实验**：生成 1 万条随机记录，随机截断到任意字节，统计可恢复记录数并与“最后一个完整物理记录”理论值比较。

---

# P3：内存引擎

## T301 dbformat：内部键与比较器

- **依赖**：T103, T109, T111
- **上游**：`db/dbformat.h/.cc`、`db/dbformat_test.cc`
- **目标**：实现 LevelDB 的内部键编码、解析、排序和 LookupKey。
- **设计/交付**：
  - 常量：kNumLevels=7、kMaxSequenceNumber=(1<<56)-1、ValueType（Deletion=0、Value=1）、kValueTypeForSeek=kTypeValue。
  - `append_internal_key(buf, user_key, seq, type)`：user key + fixed64(seq<<8|type)。
  - `parse_internal_key`：长度 ≥8，解析 tag；失败返回 false；`extract_user_key`。
  - `ParsedInternalKey`、`InternalKeyComparator`（user comparator + tag 降序）、`InternalFilterPolicy`（比较/创建过滤器前去掉 8 字节 tag，Name 转发）。
  - `LookupKey`：200 字节内联缓冲，保存 user key + 内部键 + slice；get 时可直接用于 memtable。
  - user key 比较与 internal key 比较分离：前者给用户/文件范围，后者给 skiplist/合并。
- **验收**：
  - 与上游 dbformat_test 对照：编码/解析/排序/最大序列号/错误键。
  - 排序矩阵：相同 user key、不同 sequence/type 的组合；确认 tag 大在前。
  - InternalFilterPolicy 的 key 变换正确；空 user key；最大长度。
- **吃透检查点**：
  - 为什么同一 user key 的 sequence 要降序？如果升序，读路径会怎样？
  - 为什么 LookupKey 使用 `kValueTypeForSeek=kTypeValue` 而不是 deletion？
  - InternalFilterPolicy 为什么要剥掉 8 字节 tag？不剥会怎样？
  - 内部键比较与用户键比较分别在哪些场景使用？
  - 解析失败时各调用方应如何处理？为什么恢复/compaction 不愿丢弃错误键？
- **实验**：构造同 key 多版本（seq/type 不同），打印 InternalKeyComparator 排序结果；再按 user key 分组解释。

## T302 WriteBatch

- **依赖**：T101, T103, T301
- **上游**：`include/leveldb/write_batch.h`、`db/write_batch.cc`、`db/write_batch_internal.h`、`db/write_batch_test.cc`
- **目标**：实现批量写的字节格式、遍历、合并和插入回调。
- **设计/交付**：
  - `cldb_write_batch_t {cldb_buf_t rep;}`；rep 头 12 字节：fixed64 sequence、fixed32 count。
  - put/delete：追加 tag(1/0)、varint key_len、key、[varint value_len、value]；count++。
  - clear/approximate_size/append；append 去掉源 12 字节头并累加 count；自追加安全。
  - `set_contents(slice)` 借用外部内容？明确所有权：是复制还是借用；在 C 版建议复制到内部 buf，避免悬垂。
  - Handler vtable（put/delete）；`iterate` 逐条回调；`insert_into` 使用 handler（MemTableInserter 在 T304 提供），每条记录 sequence 递增。
  - `sequence/count/byte_size/set_sequence` 访问器。
- **验收**：
  - 与上游 write_batch_test 对照：编码、顺序、count、append、Clear、Iterate。
  - 自追加、空 batch、超大 value、批量 sequence 连续性。
  - malformed batch（长度越界）不越界，返回 corruption。
- **吃透检查点**：
  - 为什么 sequence 放在 batch 头而不是每条记录？
  - `Append` 为什么可以只复制 12 字节之后的内容？
  - 一个 batch 内的每条操作如何获得唯一 sequence？
  - Handler 是同步回调还是可保存的迭代器？为什么这样设计？
- **实验**：hexdump 一个含 Put/Delete 的 batch，手工解析每个字段；再让 sequence 从 100 开始插入，检查内部键 tag。

## T303 SkipList（专门化实现）

- **依赖**：T105, T107, T301
- **上游**：`db/skiplist.h`、`db/skiplist_test.cc`
- **目标**：实现 MemTable 使用的无锁读、单写者跳表。
- **设计/交付**：
  - `kMaxHeight=12`；随机高度：`height=1; while (height<kMaxHeight && rnd.OneIn(4)) height++`。
  - 节点：key 为 `const char*`（指向 MemTable 条目/arena），`next[]` 原子指针；头节点拥有最大高度。
  - 插入：`find_greater_or_equal` 记录每层 prev；创建节点；未达高度时接在 head 后；逐层 release store。
  - 查找：`find_greater_or_equal`、`find_less_than`、`find_last`；比较函数由 `void *arg` + compare 回调提供（比较 MemTable 编码键）。
  - 迭代器：正向 next；反向 Prev 用 `find_less_than`（从 head 搜索）；不提供删除。
  - 读无锁；写必须由外部互斥锁串行化（DB mutex）。
- **验收**：
  - 移植 skiplist_test 的核心：随机插入/查找、顺序遍历、反向遍历、重复 key 行为。
  - 高度分布大致符合 1/4（统计）；max height 不越界。
  - 单写多读压力测试；ASan/TSAN 无数据竞争（读路径）。
- **吃透检查点**：
  - 为什么读可以无锁？插入时如何保证读者看到完整的 next 指针？
  - release/acquire 分别加在哪一侧？改成 relaxed 会破坏什么？
  - `max_height_` 的更新为什么可以用 relaxed？
  - 反向迭代为什么需要 O(log n) 重新搜索，而不是维护 prev 指针？
  - 为什么不允许删除？这对 MemTable 生命周期有什么简化？
- **实验**：插入 10 万随机 key，统计每层节点数与理论分布；观察重建/查找平均比较次数。

## T304 MemTable

- **依赖**：T105, T301, T302, T303
- **上游**：`db/memtable.h/.cc`、`db/memtable_test.cc`
- **目标**：实现内存表：编码条目、Add/Get/NewIterator、Arena 与引用计数。
- **设计/交付**：
  - 条目编码：`varint32 internal_key_size | internal_key | varint32 value_size | value`。
  - Add(seq,type,key,value)：拼接内部键到临时 buf，Arena 分配条目，SkipList 插入；比较器解析长度并调用 InternalKeyComparator。
  - Get(LookupKey)：skiplist seek 到 ≥ lookup key；解析条目 tag；同 user key 且 tag 满足条件则返回值/删除状态；否则未命中。
  - NewIterator：遍历 skiplist，返回内部键 Slice + value Slice；借用 MemTable 内存。
  - `ref/unref`；ApproximateMemoryUsage = Arena 使用量。
  - `MemTableInserter`：WriteBatch Handler，调用 Add 并对每条记录递增 sequence。
- **验收**：
  - 移植 memtable_test：Put/Delete/覆盖、不同 sequence、Get、迭代顺序、内存增长。
  - 快照序列边界：seq == snapshot、seq > snapshot、删除后的旧值。
  - 与 WriteBatch 集成：一个 batch 的连续 sequence 正确；ASan 无泄漏。
- **吃透检查点**：
  - 条目为什么把 internal key 长度放在最前？SkipList 比较器如何在不复制 key 的情况下比较？
  - Get 找到第一个 ≥ lookup 的条目后，为什么还要比较 user key？可能命中什么？
  - 为什么 MemTable 需要引用计数？谁在持有引用？
  - 迭代器返回的 Slice 生命周期到什么时候？为什么不能越过 MemTable 销毁？
- **实验**：插入 1 万条并打印 `ApproximateMemoryUsage`，计算每条平均开销（含 arena 块浪费）。

## T305 SnapshotList

- **依赖**：T101, T301
- **上游**：`db/snapshot.h`
- **目标**：实现快照链表，提供最老快照序列号。
- **设计/交付**：
  - `SnapshotImpl {SequenceNumber sequence; prev/next;}`；循环双向链表 + 头节点。
  - `new(seq)` 插入尾部（最新）；`delete(snap)` 摘链；`oldest()` 返回头部元素；`empty()`。
  - 快照对象不拥有数据，只是序列号。
- **验收**：
  - 新增/删除/最老/空链；乱序删除；释放最老后 oldest 更新；重复删除禁止（assert/文档）。
  - 与 DB 结合前先做纯链表测试。
- **吃透检查点**：
  - 为什么快照只需要 sequence，不需要复制数据或阻止 compaction 读写？
  - 为什么最老快照决定 compaction 的 `smallest_snapshot`？
  - 快照链表的插入顺序为什么可以假定 sequence 递增？
- **实验**：创建 5 个快照，删除中间/最老，观察 `oldest()`；再思考 compaction 的可见边界。

## T306 P3 阶段门：内存引擎集成与随机测试

- **依赖**：T301–T305
- **上游**：`db/memtable_test.cc`、`db/write_batch_test.cc`、`db/skiplist_test.cc`
- **目标**：证明内存引擎在写入、读取、迭代、快照、并发方面自洽。
- **验收**：
  - 随机操作序列（Put/Delete/Get/迭代）与 `std::map` 参考模型（C 版用有序数组/哈希）逐操作一致。
  - 单写多读并发压测；快照序列下读取稳定。
  - WriteBatch 的原子插入与 sequence 分配测试；ASan/TSAN（读路径）通过。
- **吃透检查点**：
  - 内存引擎的“真相”由哪些结构共同决定（Arena、SkipList、MemTable、WriteBatch）？
  - 如果 sequence 分配在 batch 中途失败，状态会怎样？为什么批量写要求整批成功/失败？
  - 迭代器顺序与 InternalKeyComparator 的关系；用户迭代器还需要做什么？
- **实验**：把 MemTable 的 `write_buffer_size` 设得极小（后续 T605），观察 flush 触发；此处先用内存模型测试 sequence 边界。

---

# P4：SSTable

## T401 BlockBuilder

- **依赖**：T103, T109
- **上游**：`table/block_builder.h/.cc`、`table/block_builder_test.cc`（若存在）
- **目标**：实现数据块/索引块的前缀压缩和 restart 数组。
- **设计/交付**：
  - `block` 缓冲、`restarts[]`、`counter`、`finished`、`last_key`。
  - add(key,value)：计算 shared/non_shared；写 varint 三连 + key 后缀 + value；每 `restart_interval` 个条目写 restart offset（shared=0）。
  - finish：追加所有 restart offset 和 `num_restarts`；标记 finished；之后禁止 add。
  - reset/empty/last_key/current_size_estimate。
  - index block 使用 restart_interval=1（由 TableBuilder 设置）。
- **验收**：
  - 固定条目的字节布局 golden（shared/non_shared 计算正确）。
  - restart 边界：空块、1 条、恰好 interval、超过 interval、重复 key。
  - 与上游 BlockBuilder 输出逐字节一致；finish 后 add 被拒绝。
- **吃透检查点**：
  - 前缀压缩如何减少空间？restart 点为什么必须保存完整 key？
  - restart_interval 变大/变小的空间与 Seek 成本 trade-off。
  - 为什么索引块用 interval=1？代价是什么？
  - `shared` 长度总是与上一个 key 比较吗？restart 点为什么 shared=0？
- **实验**：用 interval=16 与 1 分别构建相同数据块，比较大小和随机 Seek 的比较次数。

## T402 Block 读取与 Block::Iter

- **依赖**：T401, T103, T106
- **上游**：`table/block.h/.cc`、`table/block_test.cc`（若存在）
- **目标**：解析 block、实现 Seek/Next/Prev 和损坏检测。
- **设计/交付**：
  - Block 结构：data Slice、restart 数组、num_restarts，以及数据归属、堆分配与可缓存标志。
  - 构造：读取末尾 4 字节 num_restarts，校验大小；计算 restart 起始位置。
  - Iter：`Seek` 先二分 restart（比较该 restart 的完整 key），再线性扫描；`SeekToFirst/Last`；`Next`；`Prev` 回到前一个 restart 重扫或从当前 restart 回退。
  - key 重建：保存当前 key buffer 和 restart offset；`shared` 必须 ≤ 上一个 key 长度。
  - 损坏：越界 restart、非法 varint、shared 越界 → status=Corruption，迭代器 invalid。
  - 借用原始 block 数据；cachable 时由 block cache 管理生命周期。
- **验收**：
  - 与 BlockBuilder 往返：顺序/反向/随机 Seek 的结果正确。
  - 边界：空块、单条、restart=1、前缀极长、key/value 为二进制。
  - 篡改 restart/长度/shared：不越界并报告 corruption；ASan。
- **吃透检查点**：
  - Seek 为什么不能全局二分每个 key？restart 二分 + 线性扫描的复杂度与正确性。
  - Prev 为什么要从 restart 重扫？如果维护 prev 指针会有什么代价？
  - `shared` 越界为什么是 corruption 而不是断言？
  - Block 数据的所有权有哪几种来源？谁负责释放？
- **实验**：构造 1000 个共享前缀的 key，测量 block 大小与 Seek 平均线性扫描长度。

## T403 BlockHandle、Footer、ReadBlock 与压缩类型

- **依赖**：T103, T106, T113
- **上游**：`table/format.h/.cc`、`table/table_builder.cc` 的 WriteRawBlock、`port/port_stdcxx.h` 的压缩接口
- **目标**：实现 SSTable 元数据编解码和原始块读取（含校验/所有权/压缩分派）。
- **设计/交付**：
  - BlockHandle：varint64 offset + varint64 size；encode/decode；最大编码 20 字节。
  - Footer：metaindex_handle + index_handle，各自补齐到 20 字节；magic `0xdb4775248b80fb57` 小端；总 48 字节。
  - `read_block(file, options, handle, result)`：读 size+5 字节；检查截断；`verify_checksums` 时 CRC over data+type；按 type 分派。
  - type=0：直接使用；区分 heap_allocated/cachable；类型 1/2 首版返回 NotSupported/Corruption（与上游无库时的行为对齐）。
  - 压缩钩子接口（供 T704 扩展）；块 trailer 5 字节。
- **验收**：
  - Footer 编解码与上游逐字节一致（含 padding 和 magic）。
  - BlockHandle 边界（0、最大 varint、20 字节上限）。
  - ReadBlock：正常/截断/CRC 错误/未知 type；cachable 与 heap_allocated 所有权正确；ASan。
- **吃透检查点**：
  - Footer 为什么把两个 handle 各补到 20 字节？变长编码如何保证固定 48 字节？
  - CRC 为什么覆盖 type 字节？如果只覆盖压缩数据会漏掉什么？
  - `cachable=false` 的两种来源（mmap/外部缓冲）分别意味着什么？
  - 没有 Snappy/Zstd 时，读到压缩块应返回什么？为什么不能静默当原始数据？
- **实验**：手工构造 48 字节 footer（空 handle + magic），用 Decode 验证；再篡改 magic 观察错误。

## T404 FilterBlock

- **依赖**：T103, T111, T401
- **上游**：`table/filter_block.h/.cc`、`table/filter_block_test.cc`
- **目标**：实现按 2 KB 区间分组的过滤器块构造与读取。
- **设计/交付**：
  - `kFilterBaseLg=11`（2 KB）。
  - Builder：`start_block(block_offset)` 计算 filter_index；若 offsets 数量增长，先把上一个 filter 写入 result；`add_key` 把 key 加入当前 policy；`finish` 生成最后一个 filter，追加 offsets 数组、array_offset、base_lg。
  - Reader：验证 size ≥5，读取 base_lg/array_offset/num；index = block_offset >> base_lg；越界或非法 → may_match=true。
  - 每个 filter 由 policy 创建；空 key 集合的处理与上游一致。
- **验收**：
  - 与上游 filter_block_test 对应：多个 block、空 block、跨 2 KB、多个过滤器边界。
  - filter block 字节布局 golden；Reader 在截断/越界时返回 may_match。
  - 与 Bloom policy 集成测试。
- **吃透检查点**：
  - 为什么按 2 KB 分组？过滤器粒度与读放大/空间的关系。
  - offsets 数组 + array_offset 的布局如何做到从 block 末尾反向解析？
  - 为什么越界/损坏必须当作“可能匹配”，而不能当作“不存在”？
  - 空的 filter 区间如何表示？index 计算为什么用 `>> 11`？
- **实验**：构造长度为 0/1/2047/2048/4096 的 block，观察 filter 数量与偏移数组。

## T405 TableBuilder

- **依赖**：T401, T403, T404, T109
- **上游**：`table/table_builder.h/.cc`、`include/leveldb/table_builder.h`、`table/table_test.cc`
- **目标**：把有序内部键值流写成与上游兼容的 SSTable。
- **设计/交付**：
  - 字段：file、offset、data_block、filter_block、index_block、last_key、pending_index_entry、pending_handle、num_entries、closed、options、status。
  - add(key,value)：必要时 flush 数据块；维护“上一块的 last key”作为 pending index entry；用 `FindShortestSeparator` 缩短索引 key。
  - flush：filter start_block/AddKey；压缩判断（首版 no compression；若 hook 可用，按 `compressed < raw - raw/8`）；WriteRawBlock（type + masked CRC）。
  - finish：写 filter block（若有）、metaindex（`filter.<name>`）、index（用 `FindShortSuccessor` 缩短最后一个 key）、footer；Sync + Close。
  - abandon：不完成文件；ChangeOptions（未 finish 前）；NumEntries/FileSize。
  - 空表行为、文件名为调用者给定。
- **验收**：
  - 写空表、单条、多块、带 filter；与上游生成的表逐字节/golden 比较（固定 options）。
  - 索引键缩短后的 Seek 正确；block 边界/压缩阈值边界。
  - 写入失败（Env 注入）时状态正确、文件清理由调用者处理；ASan。
- **吃透检查点**：
  - 为什么索引项要等到下一个 key 才能确定？pending index entry 的作用。
  - 为什么压缩必须“严格超过 12.5% 的节省”才采用？压缩阈值与读 CPU 的权衡。
  - 最后一个索引键为什么用 `FindShortSuccessor`，而不是分隔器？
  - metaindex 的 key 为什么包含 policy 名称？filter block 的 handle 如何写入？
  - 表的写入顺序（data→filter→metaindex→index→footer）与崩溃一致性有什么关系？
- **实验**：用 no compression 与“总是压缩”（假压缩）构建同一批数据，比较文件大小和读取校验；解释阈值。

## T406 Table：打开、读取与 InternalGet

- **依赖**：T402, T403, T405, T112
- **上游**：`table/table.h/.cc`、`include/leveldb/table.h`、`table/table_test.cc`
- **目标**：打开已有 SSTable，按内部键读取，使用 block cache/filter。
- **设计/交付**：
  - Open：读取文件大小，seek 到末尾 48 字节读 Footer，校验 magic，读 index block；失败清理。
  - ReadMeta/ReadFilter：解析 metaindex，惰性初始化 filter；失败时 filter 视为不可用（不得假阴性）。
  - BlockReader：block cache key = cache_id(8B) + offset(8B)；charge=size；handle 生命周期；无 cache 时直接 ReadBlock。
  - NewIterator：TwoLevelIterator(index_iter, data_block_reader)。
  - InternalGet：先 filter（若配置），index Seek 找到数据块，ReadBlock，Block::Iter Seek；返回 Found/Deleted/NotFound/Corrupt；不可用 filter 时继续查。
  - ApproximateOffsetOf：按索引/块在文件中的偏移估算。
  - Table 拥有 RandomAccessFile？明确所有权（上游 Table 析构关闭文件）；C 版由 TableCache 管理。
- **验收**：
  - 打开自建/上游表；Get 命中/删除/未命中；过滤器 false positive 不影响正确性。
  - block cache 命中/未命中/容量 0；fill_cache=false 不缓存；verify_checksums 开启时篡改块报错。
  - 损坏 footer/magic/index/块：不崩溃、返回 Corruption；ASan。
- **吃透检查点**：
  - InternalGet 为什么先查 filter 再查 index？filter 只能排除一部分块。
  - block cache 的 key 为什么需要 cache_id？只用 offset 会出什么问题？
  - Table 的 cache handle 与 block 数据的生命周期关系；谁释放 block？
  - fill_cache=false 时 Block 的数据来自哪里？cachable 标志如何影响释放？
- **实验**：用 1 字节/8 MB block cache 分别读同一表，统计命中与 ReadBlock 次数；记录 approximate offset 曲线。

## T407 TwoLevelIterator

- **依赖**：T110, T406
- **上游**：`table/two_level_iterator.h/.cc`、`table/two_level_iterator_test.cc`（若存在）
- **目标**：用索引迭代器按需打开数据块，实现单文件内的 concatenating 迭代。
- **设计/交付**：
  - 状态：index_iter、data_iter、data_block_handle、data_iter_factory、arg、status。
  - valid/seek/seek_to_first/seek_to_last/next/prev：定位 index，读 BlockHandle，打开数据块，数据块耗尽则前进索引。
  - 空块跳过；同一 handle 复用 data_iter；status 聚合（index/data 任一错误）。
  - 销毁顺序：先 data 后 index；清理 factory 资源。
- **验收**：
  - 单文件内正/反/Seek 与整表线性扫描一致；空块、连续空块、单块、多块。
  - 数据块读取失败；索引耗尽；重复 Seek；ASan。
- **吃透检查点**：
  - 为什么需要两层迭代？直接为每个数据块建迭代器会有什么问题？
  - data block handle 相同时为什么要复用迭代器？
  - 反向 SeekToLast 如何定位最后一个非空块？
  - TwoLevelIterator 与 MergingIterator 的职责边界是什么？
- **实验**：在索引中插入空数据块（构造测试表），验证迭代器跳过且 Seek 结果正确。

## T408 MergingIterator 与 IteratorWrapper

- **依赖**：T110, T301, T402
- **上游**：`table/merger.h/.cc`、`table/iterator_wrapper.h`、`table/merger_test.cc`
- **目标**：实现多路归并迭代器，供 memtable/L0/各层合并使用。
- **设计/交付**：
  - IteratorWrapper：缓存 valid/key，减少虚调用；保存子迭代器指针。
  - MergingIterator：children 数组、current_、direction；FindSmallest/FindLargest 线性扫描；Next/Prev 推进当前并重新选择。
  - 切换方向时重新定位所有子迭代器（forward→reverse 用 SeekToLast/Seek 到当前 key 之后，reverse→forward 同理）。
  - 空 children/单 child 快速路径；status 聚合。
  - 比较器使用 InternalKeyComparator。
- **验收**：
  - 移植 merger_test：多路有序流、重复 key、空流、单流、双向遍历。
  - 方向切换后的结果与重新创建迭代器一致；随机模型；ASan。
- **吃透检查点**：
  - 正向迭代时为什么只需看 current_？反向切换为什么要重新定位所有子迭代器？
  - 重复内部键在合并中如何排序？是否可能完全相同？
  - IteratorWrapper 缓存 valid/key 的收益与失效时机。
  - MergingIterator 的 O(N) 选择在层数/文件数下的成本；LevelDB 为什么接受？
- **实验**：用 20 个内存子迭代器合并 1 万条记录，测量每次 Next 的比较次数；对比堆优化。

## T409 P4 阶段门：SSTable golden、自洽与损坏测试

- **依赖**：T401–T408
- **上游**：`table/table_test.cc`、`table/filter_block_test.cc`、`table/format.cc`
- **目标**：证明表格式与上游兼容，并能安全处理损坏输入。
- **验收**：
  - 固定条目集生成的表与上游逐字节一致（no compression、固定 options/restart）。
  - C 版读取上游表、上游读取 C 版表（若 C++ 辅助可用）；Seek/Prev/Get/ApproximateOffsetOf 结果一致。
  - 损坏矩阵：footer magic、handle 越界、块 CRC、块 type、restart、filter 越界；不崩溃且合理报错。
  - 单元测试全部通过；ASan/UBSan 无报告。
- **吃透检查点**：
  - SSTable 的“自描述”体现在哪些字段？哪些错误只能靠 CRC 发现？
  - 索引块的 key 为什么是数据块最后一个 key 的缩短形式？Seek 如何保证不越过目标？
  - 如果 block cache 命中了损坏块，校验还会发生吗？这对 `verify_checksums` 有何影响？
- **实验**：用十六进制编辑器篡改一个表的 CRC/type/magic，记录三种错误的返回路径；恢复文件。

# P5：版本管理与压缩选择

## T501 FileMetaData 与 Version 核心

- **依赖**：T301, T405, T408
- **上游**：`db/version_edit.h`（FileMetaData）、`db/version_set.h/.cc`（Version）
- **目标**：实现“某一时刻各层文件布局”的不可变版本对象及其查找/范围查询。
- **设计/交付**：
  - `cldb_file_meta_t {int refs; int allowed_seeks; uint64_t number, file_size; InternalKey smallest, largest;}`。
  - `allowed_seeks` 初始 1<<30；新加入版本后设为 `max(file_size >> 14, 100)`。
  - `Version`：`files[7]` 动态数组、版本链表 prev/next、refs、file_to_compact/level、compaction_score/level、vset 指针。
  - 构造时 refs=0；VersionSet 持有/释放；销毁时对每个 FileMetaData `refs--`，归零则 free（FileMetaData 在版本间共享）。
  - 函数：AddFile/RemoveFile（用于 Builder 构造版本）、FindFile（二分）、SomeFileOverlapsRange、Get、ForEachOverlapping、GetOverlappingInputs（L0 特殊处理）、OverlapInLevel、PickLevelForMemTableOutput、UpdateStats、RecordReadSample、DebugString/NumFiles。
- **验收**：
  - 合成文件集合：FindFile 二分边界、范围重叠、L0/L1 的 Get 顺序。
  - `allowed_seeks` 递减与 seek compaction 标记；`RecordReadSample` 只在多文件匹配时计数。
  - FileMetaData 引用计数：版本销毁后仍被新版本引用不释放；最后一个释放才 free；ASan。
  - PickLevelForMemTableOutput：最多到 L2、grandparent 重叠限制。
- **吃透检查点**：
  - 为什么 Version 必须不可变？如果直接在 current 上增删文件会破坏什么？
  - FileMetaData 为什么要在多个 Version 之间共享？引用计数由谁维护？
  - L0 为什么不能二分查找？L1+ 为什么可以？
  - `GetOverlappingInputs(0, ...)` 为什么需要“重启扫描”以纳入新发现的 L0 文件？
  - `PickLevelForMemTableOutput` 为什么最多只到 L2？
- **实验**：构造 3 个版本共享部分文件，打印 refs 变化；删除旧版本后确认共享文件仍存活。

## T502 VersionEdit 编解码

- **依赖**：T103, T301, T501
- **上游**：`db/version_edit.h/.cc`、`db/version_edit_test.cc`
- **目标**：实现 MANIFEST 增量记录的精确编码/解码。
- **设计/交付**：
  - 字段：comparator、log_number、prev_log_number、next_file_number、last_sequence；compact_pointers（level + internal key）；deleted_files（level+number 集合）；new_files（level + FileMetaData）。
  - tag：1 comparator、2 log、3 next file、4 last sequence、5 compact pointer、6 deleted file、7 new file、9 prev log。
  - EncodeTo：只写 has_* 字段；new/deleted 逐条；varint/length-prefixed。
  - DecodeFrom：解析直至输入耗尽；未知 tag 返回 `Corruption("unknown tag")`，字段缺失/截断返回 Corruption；不要自行放宽。
  - Clear/DebugString/LevelSummary（可选）。
- **验收**：
  - 全字段 round-trip；字段出现顺序不同仍可解析；重复字段行为与上游一致。
  - 与上游 version_edit_test 对照；固定 edit 的编码逐字节 golden。
  - 截断/越界/未知 tag：返回 Corruption，不越界。
- **吃透检查点**：
  - 为什么 VersionEdit 用“增量 + tag”而不是固定结构？
  - 为什么 deleted/new file 都要带 level？同一文件号会出现在不同层吗？
  - 哪些字段必须出现在每次 LogAndApply 的 edit 中，哪些只在全量快照中？
  - 解码时如何处理未知 tag？为什么这关系到向前兼容？
- **实验**：手工编码一个只含 log_number 的 edit，再叠加 decoder 日志，观察字段顺序。

## T503 VersionSet::Builder 与 LevelFileNumIterator

- **依赖**：T501, T502
- **上游**：`db/version_set.cc` 的 `VersionSet::Builder`、`Version::LevelFileNumIterator`
- **目标**：把基础版本 + VersionEdit 合并成新版本，并提供按文件范围遍历的迭代器。
- **设计/交付**：
  - Builder 保存 base_（可能为 NULL）、每层 added_files 和 deleted_files。
  - `Apply(edit)`：记录 comparator/log/next/last；收集 deleted；new_files 按层加入，保持按 smallest key 排序（插入排序/后排序）。
  - `SaveTo(v)`：对每层合并 base 与 added（双指针），跳过 deleted；`MaybeAddFile` 校验 level>0 不重叠并增加 FileMetaData refs。
  - Builder 析构：释放 added 中未被保存的 FileMetaData；base_ 引用管理。
  - `LevelFileNumIterator`：按 InternalKeyComparator 在有序文件数组中前进/后退，key 返回 largest 内部键，value 是 fixed64 文件号 + fixed64 文件大小（供 table cache 使用）。
- **验收**：
  - 移植 version_set_test 的 Builder 用例：新增/删除/多层/重叠/边界。
  - 排序稳定性；`FileMetaData` refs 无泄漏。
  - LevelFileNumIterator 正/反/Seek 与文件数组一致；空层。
- **吃透检查点**：
  - 为什么 Builder 要保留 base 和 added 两套数据，而不是直接改 base？
  - 合并时如何保证 L1+ 不重叠？L0 为什么允许重叠？
  - `MaybeAddFile` 为什么要增加 FileMetaData refs？
  - LevelFileNumIterator 的 value 为什么同时包含文件号和文件大小？
- **实验**：构造 base 版本与一个 edit，打印每层合并前后的文件号和 refs。

## T504 VersionSet 核心状态与查询

- **依赖**：T503, T116
- **上游**：`db/version_set.h/.cc`（VersionSet 除 LogAndApply/Recover/Compaction 选择外的部分）
- **目标**：实现版本集合、文件号分配、存活文件集合和统计查询。
- **设计/交付**：
  - 字段：env/dbname/options/table_cache/icmp、next_file_number（从 2 开始）、manifest_file_number、last_sequence、log_number、prev_log_number、descriptor_file/log、dummy_versions/current、compact_pointer[7]。
  - `new_file_number`、`reuse_file_number`、`mark_file_number_used`、last_sequence get/set、log/prev/manifest 访问器。
  - `append_version`：加入双向链表，ref，替换 current，释放旧 current。
  - `NumLevelFiles/NumLevelBytes`、`MaxNextLevelOverlappingBytes`、`AddLiveFiles`、`ApproximateOffsetOf`、`NeedsCompaction`。
  - `Finalize` 计算每层 score/compaction_level（L0 计数/4；Ln 字节/10MB*10^(n-1)）。
- **验收**：
  - 文件号分配/回收/标记已用；version 链表 refs；AddLiveFiles 包含所有版本文件。
  - Finalize 的 score 边界（L0=4、L1=10MB、L2=100MB）；NeedsCompaction。
  - ApproximateOffsetOf 与文件顺序一致；空版本。
- **吃透检查点**：
  - 为什么 `next_file_number_` 从 2 开始？1 去哪了？
  - `manifest_file_number_` 为什么由 Recover 填充，而不是构造时分配？
  - AddLiveFiles 为什么遍历“所有版本”而不是只看 current？
  - 为什么 L0 的 score 用文件数，而 L1+ 用字节数？
- **实验**：构造不同文件大小/数量的版本，打印 Finalize 选中的层和 score。

## T505 LogAndApply 与 CURRENT/MANIFEST 提交

- **依赖**：T504, T202, T116, T115
- **上游**：`db/version_set.cc::LogAndApply`、`SetCurrentFile`、`VersionSet::ReuseManifest` 的调用关系
- **目标**：实现原子版本提交：MANIFEST 追加、Sync、CURRENT 切换、新版本安装和失败回滚。
- **设计/交付**：
  - 进入时补全 edit：缺 log/prev 则填当前值；`SetNextFile(next_file_number_)`、`SetLastSequence(last_sequence_)`。
  - 先用 Builder 把 edit 应用到 current 生成候选 Version，并 Finalize；候选版本在提交成功前不安装。
  - `descriptor_log_ == NULL` 时（首次提交或未复用 MANIFEST）：用 `manifest_file_number_`（NewDB 为 1，Recover 后为 MANIFEST 中的 next_file）创建新 MANIFEST；写 `WriteSnapshot`（comparator、compact pointers、current 的所有文件），再追加本次 edit；Sync；`SetCurrentFile`（写 `CURRENT.tmp` + rename，若 Env 支持目录 sync 则一并做）。
  - **复用** MANIFEST 时：直接把 edit 追加到 descriptor_log 并 Sync，不改写 CURRENT。
  - 成功后：AppendVersion（候选变成 current），更新 `log_number_`/`prev_log_number_`；`last_sequence_`/`next_file_number_` 由调用方在提交前通过 SetLastSequence/NewFileNumber 推进，LogAndApply 只负责把它们写入 edit。
  - 失败：删除候选 Version；若本次新建 MANIFEST，则关闭/删除该文件、descriptor 置空、CURRENT 不变。已有 MANIFEST 的追加失败会留下不确定尾部，由背景错误阻止后续提交，恢复时依赖记录 CRC 丢弃尾部。
  - 注意：写 MANIFEST 期间释放 DB mutex；前提是同一时刻只有一个 LogAndApply，且 current 在成功前不变。
- **验收**：
  - NewDB 生成 MANIFEST-000001；随后首次 LogAndApply 按 `manifest_file_number_` 创建新 MANIFEST 并原子切换 CURRENT（reuse_logs=true 时可复用旧 MANIFEST，不切 CURRENT）。
  - 连续多次 edit：MANIFEST 追加记录，重启 Recover 得到相同版本。
  - 注入 Append/Sync/Rename 失败：候选版本不安装、current 不变、新建 MANIFEST 被清理、CURRENT 仍指向旧文件；已有 descriptor 的失败进入背景错误。
  - CURRENT 内容/临时文件行为；golden。
- **吃透检查点**：
  - 为什么第一次要写“全量快照”记录？没有它会怎样？
  - CURRENT 的 rename 为什么是提交点？写 MANIFEST 与写 CURRENT 的先后能否交换？
  - 为什么写 MANIFEST 时要释放 DB mutex？为什么这是安全的？
  - 失败后为什么不能留下一个“部分写入”的 CURRENT？
  - `descriptor_log_` 在什么情况下换新文件？旧 MANIFEST 何时可删？
- **实验**：在 MemEnv 包装器中让 `RenameFile` 失败，观察 CURRENT 是否仍指向旧 MANIFEST、DB 状态是否一致。

## T506 VersionSet::Recover 与 ReuseManifest

- **依赖**：T505, T203, T116, T115
- **上游**：`db/version_set.cc::Recover`、`ReuseManifest`、`db/version_set_test.cc`
- **目标**：从 CURRENT/MANIFEST 重建版本状态，并处理旧版/损坏/复用场景。
- **设计/交付**：
  - 读 CURRENT（必须在末尾有 `\n`，否则 Corruption）；解析 MANIFEST 文件号。
  - 用 log::Reader 读取 MANIFEST 的 VersionEdit 记录；Builder 逐条 Apply；最后 SaveTo 生成 current。
  - 校验：comparator 名匹配；必须出现 next_file_number、last_sequence、log_number；prev log 可为 0；未知/缺失字段报 Corruption。
  - `ReuseManifest`：仅当 `options.reuse_logs=true`、CURRENT 指向的文件名可解析为 descriptor、且文件大小 < max_file_size 时，用 `NewAppendableFile` 打开并创建 `log::Writer`（以文件长度初始化 block offset）；否则留待下次 LogAndApply 写新 MANIFEST。comparator 校验发生在 Recover 的 edit 解码阶段。
  - 设置 `manifest_file_number_`、next_file_number、last_sequence、log/prev。
- **验收**：
  - 用 T505 生成的 MANIFEST 恢复，版本文件/序列/log 号完全一致。
  - 缺 CURRENT/空 CURRENT/无换行/坏 magic/截断/未知 comparator/缺 next file/缺 last sequence 的错误路径。
  - ReuseManifest 成功与拒绝（reuse_logs=false、大小超限、文件名不合法）路径；comparator 不同应在 Recover 阶段报错。
  - 与上游 version_set_test 的 Recover 用例对照。
- **吃透检查点**：
  - 为什么 CURRENT 必须以换行结尾？去掉换行会破坏什么？
  - Recover 为什么要求日志中的 next file/last sequence 必须存在？
  - ReuseManifest 为什么要求 `reuse_logs=true`？为什么还要求“文件大小 < max_file_size”？
  - 恢复时发现 MANIFEST 引用的表文件缺失，应该怎么做？为什么不能静默忽略？
  - 读取 MANIFEST 的 log::Reader 与 WAL 的 Reader 是同一实现吗？
- **实验**：手工截断 MANIFEST 最后一条记录，恢复看是否安全丢弃尾记录；再删除 CURRENT 观察错误。

## T507 Compaction 选择：PickCompaction / SetupOtherInputs

- **依赖**：T501, T504
- **上游**：`db/version_set.cc::PickCompaction`、`SetupOtherInputs`、`AddBoundaryInputs`、`GetRange*`
- **目标**：按 score/seek 触发选择输入文件，并处理 L0 重叠、边界文件与扩展。
- **设计/交付**：
  - `PickCompaction`：优先 size compaction（score≥1），否则 seek compaction；L0 选择后扩展全部重叠 L0；构造 Compaction 对象并 Ref 输入版本。
  - compact_pointer 轮转；找不到时回绕第一个文件。
  - `SetupOtherInputs`：计算 inputs[1]（level+1 重叠）；`AddBoundaryInputs`；尝试扩大 inputs[0]（若 level+1 输入不变且总大小 < 25×max_file_size）；计算 grandparents（level+2）；更新 compact pointer 为 inputs[0] 的最大键。
  - `GetOverlappingInputs` 对 L0 的用户键范围/内部键范围语义；重启扫描。
- **验收**：
  - 移植 version_set_test 的选择用例；构造多层版本验证 score 选择、轮转、seek 文件。
  - L0 多个重叠文件全部纳入；边界文件（同 user key 跨文件）被纳入。
  - 扩展条件/上限/拒绝扩展；grandparents 正确。
- **吃透检查点**：
  - 为什么 size compaction 优先于 seek compaction？
  - compact_pointer 轮转解决什么问题？没有它会怎样？
  - 边界文件如果不加入，Get 会返回什么错误结果？用具体例子说明。
  - 扩大 inputs[0] 的收益与 25× 上限的来源。
  - L0 为什么要把所有重叠文件一起 compaction？
- **实验**：构造 L0 文件 A/B 范围重叠，再构造 L1 文件与其重叠；打印 Pick/Setup 后的 inputs 和 grandparents。

## T508 Compaction 辅助与 MakeInputIterator

- **依赖**：T507, T405, T408, T509（接口可先用桩）
- **上游**：`db/version_set.cc::MakeInputIterator`、`Compaction::{IsTrivialMove, AddInputDeletions, IsBaseLevelForKey, ShouldStopBefore, ReleaseInputs, MaxOutputFileSize}`
- **目标**：实现 compaction 的输入迭代器和各种判定逻辑。
- **设计/交付**：
  - `MakeInputIterator`：L0 每个文件一个 Table iterator；高层用 TwoLevelIterator + LevelFileNumIterator；最后 MergingIterator；ReadOptions 关闭 fill_cache、按 paranoid_checks 校验。
  - `IsTrivialMove`：1 个 input[0]、0 个 input[1]、grandparent ≤ 10×max_file_size。
  - `IsBaseLevelForKey`：从 level+2 到 6 逐层扫描，维护每层指针；若可能包含该 user key 则 false。
  - `ShouldStopBefore(key)`：累计 grandparents 中 largest < key 的字节，超过 10×max_file_size 返回 true 并重置计数。
  - `AddInputDeletions`、`ReleaseInputs`、MaxOutputFileSize。
- **验收**：
  - 合成文件集合测试 IsBaseLevelForKey/ShouldStopBefore 的边界。
  - MakeInputIterator 输出与手工合并一致（含 L0 重叠/高层多文件）。
  - trivial move 条件表；grandparent 边界恰好等于阈值。
- **吃透检查点**：
  - 为什么 MakeInputIterator 对 L0 用多个文件迭代器，对 L1+ 用 concatenating iterator？
  - trivial move 为什么要求没有 level+1 重叠？文件直接移动后 L1 的顺序如何保持？
  - `IsBaseLevelForKey` 检查的是更深层还是更浅层？删除标记为什么依赖它？
  - `ShouldStopBefore` 的字节累计为什么只看 largest < 当前 key 的 grandparent 文件？
- **实验**：构造 grandparent 重叠刚好在阈值上下，测试 trivial move 与输出切分。

## T509 TableCache

- **依赖**：T406, T112, T116
- **上游**：`db/table_cache.h/.cc`
- **目标**：缓存已打开的 Table，支撑 Get 和迭代器。
- **设计/交付**：
  - 以 fixed64 文件号为 key 的 LRU cache；值 = TableAndFile（Table* + RandomAccessFile*）。
  - `FindTable(number,size)`：先尝试 `.ldb`，失败再 `.sst`；调用 Table::Open；插入 cache；析构时删除 Table/文件。
  - `NewIterator(options,number,size)`：FindTable + Table::NewIterator；注册 cleanup 调用 cache release。
  - `Get(options,number,size,key,saver)`：FindTable + Table::InternalGet；释放 handle。
  - `Evict(number)`：删除 cache 项并释放。
  - 容量 = max_open_files - 10。
- **验收**：
  - 打开 `.ldb`/`.sst`；不存在/损坏文件的错误；缓存命中复用同一 Table。
  - 迭代器 cleanup 释放 handle；高压力打开的句柄数不超过配置；Evict 后重新打开。
  - Table 析构顺序（Table 依赖文件）；ASan。
- **吃透检查点**：
  - 为什么 table cache 的 key 是文件号而不是文件名？
  - 为什么先试 `.ldb` 再试 `.sst`？旧 DB 兼容性。
  - 迭代器注册 cleanup 的必要性：如果忘记释放 handle 会发生什么？
  - TableCache 容量与 `max_open_files` 的关系；文件描述符何时真正关闭？
- **实验**：把 max_open_files 设为 20，打开大量表并遍历，观察 Evict/文件重开（可打印计数）。

## T510 版本层测试移植与 MANIFEST Golden

- **依赖**：T501–T509
- **上游**：`db/version_set_test.cc`、`db/version_edit_test.cc`、`db/filename_test.cc`
- **目标**：用上游用例验证版本构造、选择、恢复和编码。
- **验收**：
  - 移植 version_set_test 主要用例（Builder/Finalize/Compaction 选择/Get）。
  - MANIFEST 固定 edit 序列 golden；与上游编码逐字节一致。
  - CURRENT/MANIFEST 恢复测试通过；无泄漏。
- **吃透检查点**：
  - version_set_test 的哪些用例依赖具体文件号/顺序？C 实现如何保证确定性？
  - 为什么版本测试可以完全在 MemEnv 上跑？
- **实验**：随机生成合法 VersionEdit 序列，apply 后 SaveTo，再编码/解码，比较等价性。

## T511 版本/MANIFEST 故障注入

- **依赖**：T506, T505
- **上游**：`db/corruption_test.cc`、`db/recovery_test.cc` 的相关场景
- **目标**：验证版本恢复在损坏/失败下不会造成错误状态。
- **验收**：
  - CURRENT：缺失、空、无换行、指向不存在 MANIFEST、内容多余路径。
  - MANIFEST：截断、CRC 翻转、未知 tag、缺 next file/last sequence、comparator 不匹配。
  - LogAndApply：Append/Sync/Rename/Sync 目录失败；新建 MANIFEST 失败时 current 未变、可安全重试；已有 MANIFEST 追加失败时进入背景错误，重启可丢弃损坏尾部。
  - 恢复后文件号不倒退、不覆盖存活文件；ASan。
- **吃透检查点**：
  - 哪些损坏可以在“丢弃尾部记录”后继续？哪些必须整体失败？
  - 为什么 comparator 不匹配必须拒绝打开，而不能按字节序继续？
  - 如果 MANIFEST 写成功但 CURRENT 未切换，重启后会发生什么？
- **实验**：在 MANIFEST 中间翻转一个字节，记录恢复错误；替换为末尾截断，比较行为差异。

## T512 P5 阶段门：合成版本 + 恢复 + 选择

- **依赖**：T501–T511
- **目标**：在不依赖 DBImpl 的情况下证明版本层、MANIFEST 恢复与压缩选择可以独立正确工作。
- **验收**：
  - 无 DBImpl 情况下，用合成 FileMetaData 构造多层版本，完成 Pick/Setup/MakeInputIterator 的只读验证。
  - 生成 MANIFEST/CURRENT，重启后恢复出等价版本。
  - 故障注入矩阵通过；所有版本测试 + golden 通过；ASan。
- **吃透检查点**：
  - 不看源码画出 VersionSet/Version/FileMetaData/VersionEdit 的关系和引用计数。
  - 给定一组文件，手工计算 score、compaction inputs、grandparents、是否 trivial move。
- **实验**：手算一个 3 层版本的选择结果，再与程序输出对照。

# P6：DB 核心（写路径、读路径、恢复、后台压缩）

> P6 是把 P2–P5 的零件组装成真正可用的 DB。建议先让单线程基本路径跑通（T601–T606），再补迭代器/快照（T607–T608），最后打开后台压缩与恢复测试（T609–T617）。每个任务完成后都要能回答：**这一步破坏了哪个不变量会怎样**。

## T601 DBImpl 骨架、选项 Sanitize 与生命周期

- **依赖**：T505, T509, T116
- **上游**：`db/db_impl.h/.cc`（构造、`SanitizeOptions`、析构、`CloseHelper`）、`db/db_impl.cc` 顶部常量
- **目标**：建立 DB 对象、持有各子系统、实现安全关闭。
- **设计/交付**：
  - `cldb_db` 字段：env/options/icmp、table_cache、db_lock、shutting_down（原子）、background_compaction_scheduled、manual_compaction、mutex/condvar、writer 队列、mem/imm、has_imm、logfile/logfile_number/log、versions、snapshots、pending_outputs、seed、stats、tmp_batch、bg_error。
  - `SanitizeOptions`：包装 comparator/filter；`ClipToRange`：max_open_files `[74,50000]`、write_buffer `[64KB,1GB]`、max_file `[1MB,1GB]`、block_size `[1KB,4MB]`；默认 info_log（轮转 `LOG`→`LOG.old`）、默认 block_cache 8MB。
  - `TableCacheSize = max_open_files - 10`；`kNumNonTableCacheFiles=10`。
  - info_log/block_cache 的“是否由本对象拥有”标志；仅拥有时销毁。
  - 关闭顺序：置 shutting_down → 等待后台任务退出（在 mutex 上 wait）→ 解锁 DB 文件锁 → 释放 versions/mem/imm/log/table_cache → 释放自有日志/缓存。
  - 公开 API 约定：`cldb_close` 必须在所有 iterator/snapshot 释放后调用（上游同样要求 DB 生命周期覆盖 iterator）。
- **验收**：
  - 选项默认值与上游 `Options` 一致；越界值被正确裁剪（边界测试 73/74/50000/50001）。
  - 默认日志/缓存创建与所有权；自定义对象不被释放。
  - 打开→关闭、失败打开→关闭均无泄漏；ASan/LSan。
  - 关闭时若有后台任务，析构会等待而不是 UAF。
- **吃透检查点**：
  - 为什么要裁剪 `max_open_files` 等参数，而不是信任用户？
  - `owns_info_log_`/`owns_cache_` 解决什么所有权问题？
  - 析构为什么必须先等后台任务，再释放 table cache/versions？
  - `shutting_down_` 为什么是原子变量，而不是只在 mutex 下读写？
  - 锁文件在 DB 生命周期中的角色是什么？
- **实验**：把 max_open_files 设为 1/100000，打印 Sanitize 后的值与 TableCache 容量。

## T602 BuildTable 与 WriteLevel0Table（memtable 落盘）

- **依赖**：T405, T501, T504, T509
- **上游**：`db/builder.cc::BuildTable`、`db/db_impl.cc::WriteLevel0Table`
- **目标**：把 MemTable 迭代器写成一个合法 SSTable，并生成 FileMetaData。
- **设计/交付**：
  - `BuildTable(dbname, env, options, table_cache, iter, meta)`：`SeekToFirst`；无有效键则不创建文件；否则建 TableBuilder，逐条 Add；smallest 取首键、largest 取末键；Finish → Sync → Close；用 TableCache 重新打开校验；失败或空表删除文件。
  - `WriteLevel0Table(mem, edit, base)`：分配文件号并加入 `pending_outputs`；**释放 mutex** 执行 BuildTable；成功后按 `base->PickLevelForMemTableOutput` 选择层级（恢复时 base=NULL 固定 L0）；文件非空才 `edit->AddFile`；更新 CompactionStats。
  - `pending_outputs` 在构建结束后移除；失败也要移除并删除残留文件。
- **验收**：
  - 落盘文件的 smallest/largest/file_size 与内容一致；重新打开可读；空 memtable 不产生文件。
  - BuildTable 中途失败（写失败/Sync 失败）删除半成品，edit 不引用它。
  - 指定 base 时，不重叠的小文件被放到 L1/L2；与 L1/L2 重叠时回到 L0。
  - 表建立后可通过 TableCache 正常打开/读取；失败路径不得留下损坏文件或悬挂句柄。
- **吃透检查点**：
  - `WriteLevel0Table` 为什么要临时释放 mutex？此时谁保证 memtable 不会被修改？
  - 为什么只有 `file_size > 0` 才加入 VersionEdit？
  - 为什么 BuildTable 完成后要“用 TableCache 打开一遍”？
  - memtable 落盘到 L0 与直接落到 L1/L2 的收益/风险分别是什么？
- **实验**：构造一个只含删除标记的 memtable，观察 SSTable 与后续读取结果。

## T603 WAL 恢复：RecoverLogFile 与 reuse_logs

- **依赖**：T203, T304, T502, T505, T602
- **上游**：`db/db_impl.cc::RecoverLogFile`
- **目标**：把 WAL 中的 WriteBatch 重放进 memtable，必要时转为 L0 表，并支持复用最后一个日志。
- **设计/交付**：
  - 打开 `NNNNNN.log`，log::Reader **始终启用 checksum**（即使 `paranoid_checks=false`）；Reporter 记录损坏；paranoid 时把 Corruption 作为失败，否则忽略。
  - 记录 `<12` 字节视为损坏；每条 record 作为 WriteBatch：`SetContents` → `InsertInto(mem)`；统计 `sequence + count - 1` 更新 max_sequence。
  - memtable 超过 `write_buffer_size` 时 `WriteLevel0Table`，`save_manifest=true`，释放 mem 并新建。
  - `reuse_logs && last_log && compactions==0`：`GetFileSize` + `NewAppendableFile` 成功则复用该日志，`log::Writer(lfile, lfile_size)`；mem 直接作为 `mem_`（空日志则新建 memtable）。
  - 未复用则把剩余 mem 落 L0；返回错误则 Open 失败。
- **验收**：
  - 单条/多条/跨 block 分片记录、空记录、尾部截断、CRC 翻转、超大 batch。
  - 恢复后 memtable 内容、max_sequence 与预期一致；超过 buffer 自动分表。
  - reuse_logs=true 时旧日志被追加而不是新建；复用后新写入可被再次恢复。
  - paranoid=false 时跳过损坏但保留后续完整记录；paranoid=true 时返回 Corruption。
- **吃透检查点**：
  - 为什么恢复时 checksum 必须始终开启？关闭它可能引入什么更严重的问题？
  - 为什么损坏一条记录是“跳过整条”，而不是尽量解析？
  - `max_sequence` 为什么用 `sequence + count - 1`？count=0 时呢？
  - reuse_logs 为什么要求 `last_log` 且没有发生 memtable 落盘？
  - 复用日志时 `log::Writer` 的初始文件长度为什么必须传入？
  - 恢复日志顺序为什么必须按文件号排序？
- **实验**：写 100KB 数据但把 write_buffer_size 调到 32KB（会被裁剪到 64KB），制造恢复期多次落盘，观察 L0 文件数与日志复用条件。

## T604 NewDB、Recover、Open 与 Close

- **依赖**：T603, T506（`RemoveObsoleteFiles`/调度先用桩，后续任务完成后接入）
- **上游**：`db/db_impl.cc::NewDB/Recover/DB::Open`、`include/leveldb/db.h`
- **目标**：实现数据库的创建、打开、恢复和关闭全流程。
- **设计/交付**：
  - `NewDB`：MANIFEST 文件号固定为 1；edit 写入 comparator、log=0、next_file=2、last_sequence=0；Sync/Close 后 `SetCurrentFile`；失败删除 MANIFEST。
  - `Recover`：CreateDir（错误可忽略）→ LockFile；CURRENT 不存在时按 `create_if_missing` 新建或报错；存在且 `error_if_exists` 报错；`VersionSet::Recover`；收集目录文件并核对 `expected` 存活文件，缺失即 Corruption；收集 `number >= log_number || number == prev_log` 的日志并排序；逐日志恢复并 `MarkFileNumberUsed`；更新 LastSequence。
  - `DB::Open`：Recover 后若无 mem（无日志）创建新日志+memtable，edit 记录 log number；`save_manifest` 时设置 prev_log=0 并 LogAndApply；RemoveObsoleteFiles；MaybeScheduleCompaction；失败销毁 impl。
  - `Close`：按 T601 顺序；确保 log 先关闭后释放。
- **验收**：
  - 空目录 + create_if_missing=false → InvalidArgument；create_if_missing=true → CURRENT/MANIFEST-000001/LOG。
  - error_if_exists=true 对已有 DB 报错；重复打开同一目录被文件锁拒绝（含跨进程）。
  - 写入后关闭再打开，数据/序列号一致；MANIFEST 引用的表缺失时报 Corruption。
  - 恢复多个日志（含 prev_log_number 旧格式）按序重放。
  - 打开失败不留下锁/文件句柄；ASan。
- **吃透检查点**：
  - NewDB 为什么把 next_file_number 设为 2？MANIFEST 为什么用编号 1？
  - CURRENT 不存在与数据库损坏的区别是什么？`create_if_missing` 为什么只在前者生效？
  - 为什么恢复时要核对“MANIFEST 声明但目录中不存在”的文件？
  - 日志号 `>= log_number` 为什么包括大于？prev_log_number 为什么还要兼容？
  - 为什么打开后要立刻 `RemoveObsoleteFiles` 和调度压缩？
- **实验**：手工删除一个 `MANIFEST` 中引用的 `.ldb`，记录 Open 的错误内容；恢复文件后确认可再次打开。

## T605 写路径：Writer 队列、组提交与 MakeRoomForWrite

- **依赖**：T302, T202, T504, T602（调度接口未实现时用空桩，阶段门前接通）
- **上游**：`db/db_impl.cc::Write/BuildBatchGroup/MakeRoomForWrite`、`db/write_batch_internal.h`
- **目标**：实现多线程安全、可组提交、支持 sync 的写路径。
- **设计/交付**：
  - 每个写请求封装 `Writer{batch, sync, done, status, cv}`，在 mutex 下入 FIFO；只有队首执行写入，其余等待；队首组批并唤醒整组。
  - `BuildBatchGroup`：初始最大 1MB；若首个 batch ≤128KB，则上限为首个大小 +128KB；**非 sync 队首不得吞并 sync 跟随者**；超过上限停止；用 tmp_batch 避免修改调用者 batch。
  - 序列号：`SetSequence(last_sequence+1)`；batch 内计数递增；LastSequence 在日志+memtable 都成功后更新。
  - 临界区：分配序列号后释放 mutex，执行 `log->AddRecord`（写失败即失败）、可选 `logfile->Sync`（失败标记 sync_error 并 RecordBackgroundError）、`InsertInto(mem)`；重新加锁后更新 LastSequence。
  - `MakeRoomForWrite(force)` 循环顺序：bg_error → L0 ≥8 且允许延迟（解锁 sleep 1ms，只延迟一次）→ mem 未满且非 force → imm 非空则等待 → L0 ≥12 则等待 → 切换新日志/memtable：新建日志失败则 `ReuseFileNumber` 并返回错误；关旧日志失败记录背景错误但继续切换；imm=旧 mem、has_imm=true、mem=新 memtable、MaybeScheduleCompaction。
  - `Write(nullptr)` 仅用于等待/强制切换，不写日志。
- **验收**：
  - 并发多写者顺序与序列号单调；组提交减少日志调用（用计数 Env 验证）。
  - sync 语义：sync 队在非 sync 队首后不被合并；sync 组只 Sync 一次；Sync 失败后后续写全部失败。
  - 小 batch 不会被大 batch 无限拖住；1MB/128KB 边界。
  - memtable 填满→imm→等待→落盘→继续；L0 8/12 阈值的延迟/阻塞行为。
  - 日志切换失败路径不泄漏文件号/句柄。
- **吃透检查点**：
  - 为什么必须由队首统一分配序列号？并发写入能否先写 WAL 再排序？
  - 为什么写 WAL 和 memtable 时可以释放 mutex？谁在保护 mem_？
  - 为什么 LastSequence 要等日志与 memtable 都成功后再推进？
  - 为什么 sync 跟随者不能被非 sync 队首合并？
  - 为什么 L0 到 8 要“减慢”而不是直接阻塞？12 的硬阈值从何而来？
  - 切换日志时旧日志 Close 失败为什么反而记录背景错误？
- **实验**：用带计数的 Logger/Env 统计 1000 个并发小写的 AddRecord/Sync 次数；再用 sync 混合写观察分组边界。

## T606 Get 读路径

- **依赖**：T304, T406, T501, T605
- **上游**：`db/db_impl.cc::Get`、`db/version_set.cc::Version::Get`
- **目标**：实现按快照序列读取 memtable→imm→SSTable 的完整路径。
- **设计/交付**：
  - mutex 下确定 snapshot（显式 snapshot 或 LastSequence），对 mem/imm/current 各 Ref；释放 mutex 后读取。
  - `LookupKey(user_key, snapshot)`；mem.Get → imm.Get → `current->Get`；记录 GetStats。
  - 重新加锁后 `current->UpdateStats`，需要时 MaybeScheduleCompaction；最后 Unref 三者。
  - Version::Get：L0 按文件号从新到旧；L1+ 用 FindFile 二分；每层取第一个 `largest >= key` 的文件；依次查表；处理 deletion/旧版本；返回 NotFound。
  - 错误传播：表打开/CRC/IO 错误返回，不把错误当 NotFound。
- **验收**：
  - 写入→删除→覆盖→快照读；mem/imm/L0/L1 各层命中。
  - L0 重叠文件时新值优先；L1+ 只查一个文件。
  - 不存在的 key 返回 NotFound；空 value 与 NotFound 区分。
  - 破坏一个表 → Get 返回 Corruption/IOError。
  - seek compaction 统计：多次未命中触发 allowed_seeks 递减并调度压缩。
- **吃透检查点**：
  - 为什么 snapshot 必须在锁内确定？在锁外读 LastSequence 会看到什么异常？
  - Ref mem/imm/version 分别防止哪些对象在读取期间被释放？
  - 为什么 L0 要从新到旧逐个查，而 L1+ 可以二分？
  - deletion 标记在 Get 中如何终止查找？找不到更新版本时返回什么？
  - 读放大在哪些层发生？bloom filter 在哪一步节省 IO？
- **实验**：构造固定文件布局，打印每次 Get 访问的文件号；打开/关闭 bloom 对比。

## T607 NewInternalIterator 与 DBIter

- **依赖**：T304, T408, T501, T606
- **上游**：`db/db_impl.cc::NewInternalIterator`、`db/db_iter.h/.cc`
- **目标**：合并 mem/imm/各层迭代器，并在用户视角隐藏旧版本与删除标记。
- **设计/交付**：
  - 锁内取 latest_snapshot；收集 mem iterator、imm iterator、`current->AddIterators`；构造 MergingIterator；对 mem/imm/current Ref；注册 cleanup（锁内 Unref，处理 imm=NULL）；`seed=++seed_`；解锁返回。
  - DBIter 状态：iter_、sequence_、direction_、saved_key_/saved_value_、valid_、bytes_until_read_sampling_。
  - 正向：跳过 `seq > snapshot` 或同 user key 的旧条目；跳过 deletion；保留最新值。
  - 反向：`FindPrevUserEntry` 保存候选，遇到更新版本/删除时清除。
  - 读取采样：初始/重置周期为 `Uniform(2*kReadBytesPeriod)`（默认约 0–2MB），累计读取字节超过周期时调用 `DBImpl::RecordReadSample(user_key)` 并累加新周期；key/value 访问遵守方向语义。
  - Iterator cleanup 必须在 DB 存活期间执行；测试覆盖“先销毁迭代器，再关闭 DB”的正常顺序。
- **验收**：
  - 与 std::map 模型对拍正/反向遍历；重复键、删除、快照、Seek/SeekToFirst/SeekToLast。
  - 迭代器固定快照：迭代过程中写入不可见；删除后旧版本仍可见（快照内）。
  - 各层均有数据时顺序正确；空 DB。
  - cleanup 后 mem/imm/version 引用计数归零；ASan。
- **吃透检查点**：
  - 合并迭代器为什么按 InternalKey 排序就能让同一 user key 的新版本先出现？
  - DBIter 的 `sequence_` 如何把内部多版本折叠成用户视图？
  - 反向迭代为什么不能用正向逻辑简单反转？
  - NewInternalIterator 为什么在构造期间一直持锁？
  - 读采样为什么放在 DBIter，而不是 Version::Get？
- **实验**：向同一 key 写 5 个版本再删除，分别用快照 0/2/4/最终遍历，打印可见值。

## T608 迭代器与快照生命周期

- **依赖**：T305, T607
- **上游**：`db/db_impl.cc::NewIterator/GetSnapshot/ReleaseSnapshot/RecordReadSample`、`db/snapshot.h`
- **目标**：让快照与迭代器在并发写/压缩下保持稳定，并正确释放引用。
- **设计/交付**：
  - `GetSnapshot`：锁内 `snapshots_.New(LastSequence)`；`ReleaseSnapshot`：锁内删除。
  - `NewIterator`：显式 snapshot 用其序列号，否则用 NewInternalIterator 返回的 latest；构造 DBIter。
  - 快照对象属于 DB，release 后指针失效；重复 release/使用已释放快照属使用者错误（文档说明，调试构建可断言）。
  - DB 关闭前置条件：所有 iterator/snapshot 已释放；测试覆盖“先释放迭代器再关闭”。
- **验收**：
  - 快照期间写入/删除/compaction 不改变快照读结果；释放后新快照看到最新值。
  - 多个快照并存、乱序释放；oldest snapshot 控制压缩可丢弃下界。
  - 迭代器持有期间发生 memtable 切换与 compaction，内容仍完整。
  - 泄漏检测：大量快照/迭代器创建释放后无增长。
- **吃透检查点**：
  - 为什么快照只需保存一个 sequence number，不需要复制数据？
  - 多个快照时，哪个决定 compaction 的 `smallest_snapshot`？
  - 迭代器为什么必须 Ref 住 mem/imm/version，而不能只保存指针？
  - 如果用户先 close DB 再销毁迭代器，会发生什么？API 如何约束？
- **实验**：创建 3 个快照后连续覆盖同一 key，打印每个快照读到的值与 compaction 后文件数。

## T609 后台调度、CompactMemTable 与 trivial move

- **依赖**：T505, T507, T602（不依赖写路径；测试可用内部夹具直接构造 mem/imm）
- **上游**：`db/db_impl.cc::MaybeScheduleCompaction/BGWork/BackgroundCall/CompactMemTable/BackgroundCompaction` 的 trivial move 分支
- **目标**：安全地串行化后台任务，并把 imm 落盘或直接移动文件。
- **设计/交付**：
  - `MaybeScheduleCompaction` 条件：已调度 / shutting_down / bg_error / 无 imm 且无 manual 且 `!NeedsCompaction()` 时不调度；否则置标志并 `env->Schedule(BGWork)`。
  - `BackgroundCall`：加锁；shutdown/error 时跳过；否则 BackgroundCompaction；清标志；再次 MaybeSchedule；SignalAll。
  - `CompactMemTable`：Ref base → WriteLevel0Table(imm) → shutdown 检查 → edit 设 prev_log=0、log=当前日志号 → LogAndApply → 成功则 Unref imm、has_imm=false、RemoveObsoleteFiles；失败 RecordBackgroundError。
  - trivial move：非手动、`IsTrivialMove()` 时从原层 RemoveFile、加到下一层、LogAndApply；失败记录背景错误。
  - 错误粘滞：`bg_error` 一旦设置，写路径返回该错误，后台不再调度。
- **验收**：
  - 单后台线程：并发触发只调度一次；结束后若仍需压缩会再次调度。
  - 小 write_buffer 强制 imm；落盘后 imm 为空、L0 有新文件、旧日志可删。
  - trivial move 后文件号不变、层数+1、内容可读；不满足条件时走真正 compaction。
  - shutdown 中不启动新任务；后台错误后写入返回同一错误。
  - 调度/唤醒无丢失（压力测试）。
- **吃透检查点**：
  - 为什么后台任务必须串行？Env::Schedule 的语义如何保证？
  - `background_compaction_scheduled_` 与 `shutting_down_` 如何配合避免 UAF？
  - CompactMemTable 为什么先 Ref base 再落盘？
  - LogAndApply 成功后为什么才能 Unref imm？失败后 imm 还能重试吗？
  - trivial move 为什么只改 MANIFEST，不重写数据？它依赖哪些不变量？
- **实验**：用 MemEnv 的调度钩子记录 BackgroundCall 次数；构造只含一个不重叠 L0 文件的场景，观察 trivial move 的 MANIFEST edit。

## T610 DoCompactionWork 与结果安装

- **依赖**：T508, T609
- **上游**：`db/db_impl.cc::DoCompactionWork/OpenCompactionOutputFile/FinishCompactionOutputFile/InstallCompactionResults/CleanupCompaction`
- **目标**：执行一次完整 compaction：合并输入、丢弃过期数据、切分输出、原子安装。
- **设计/交付**：
  - `smallest_snapshot` = 最老快照或 LastSequence；`MakeInputIterator`；释放 mutex 执行。
  - 每处理到 imm 存在则加锁优先 `CompactMemTable`，再解锁继续。
  - 丢弃规则：
    - (A) 同一 user key 且 `last_sequence_for_key <= smallest_snapshot` → 旧版本丢弃；
    - (B) deletion 且 `seq <= smallest_snapshot` 且 `IsBaseLevelForKey` → 删除标记可丢。
  - 输出：builder 为空时 `OpenCompactionOutputFile`（新文件号 + pending_outputs）；首键/末键维护 smallest/largest；`ShouldStopBefore` 或 `FileSize >= MaxOutputFileSize` 时 Finish；Finish 做 Sync/Close/重新打开校验。
  - 统计 bytes_read/written；成功后 `InstallCompactionResults`：AddInputDeletions + 输出 AddFile(level+1) + LogAndApply；失败 RecordBackgroundError。
  - `CleanupCompaction` 在失败/关闭时 Abandon builder、关闭文件、清理 pending_outputs。
- **验收**：
  - 小 buffer + 多层数据触发 compaction 后内容正确、文件数下降。
  - 快照存在时旧版本保留；无快照时旧版本/删除标记被丢弃。
  - 输出切分符合 2MB/祖父 10× 规则；空输出不加入 edit。
  - 注入输出写失败/LogAndApply 失败：输入文件仍在、无半成品被引用、清理句柄。
  - 压缩期间并发读写正确；关闭时能中止。
- **吃透检查点**：
  - 丢弃规则 (A) 为什么用 `<= smallest_snapshot` 而不是 `<`？
  - 删除标记在什么情况下才能真正删除？如果 `IsBaseLevelForKey` 判断错误会丢数据吗？
  - 为什么 compaction 输出必须写新文件，不能原地改旧文件？
  - 为什么先 `AddInputDeletions` 再加输出？顺序反了会怎样？
  - 输出文件校验失败为什么必须让整个 compaction 失败？
  - compaction 期间为什么可以释放 DB mutex，但读路径仍安全？
- **实验**：构造 key 在 L0/L1/L2 都有旧版本，设置快照与不设快照各跑一次，对比输出条目数与删除标记。

## T611 RemoveObsoleteFiles 与磁盘垃圾回收

- **依赖**：T504, T610
- **上游**：`db/db_impl.cc::RemoveObsoleteFiles`
- **目标**：安全删除不再被任何版本/日志/待生成文件引用的文件。
- **设计/交付**：
  - 锁内执行；`bg_error` 非 OK 直接返回（无法确定新版本是否提交）。
  - live = pending_outputs + `versions_->AddLiveFiles`。
  - 遍历目录：日志保留 `number >= LogNumber || number == PrevLogNumber`；MANIFEST 保留 `number >= ManifestFileNumber`；表/临时文件按 live 集合；CURRENT/LOCK/LOG 永远保留。
  - 先收集再删除；删除失败记录日志（不改变 DB 状态）。
  - 调用点：Open、CompactMemTable、compaction 完成、手动压缩。
- **验收**：
  - 旧日志、旧 MANIFEST、被替换的 SSTable、失败残留临时文件被删除；live 文件一个不删。
  - bg_error 后不删除任何文件。
  - pending_outputs 中的输出文件不被误删；MANIFEST 编号回退保护。
  - 文件删除失败不导致崩溃/错误状态。
- **吃透检查点**：
  - 为什么 MANIFEST 用 `>=` 保留，而表文件用精确 live 集合？
  - 为什么后台错误后必须停止 GC？
  - 为什么临时文件也必须在 pending_outputs 中？
  - 如果删除正在被迭代器使用的旧表会怎样？live 集合如何覆盖它？
- **实验**：手动在 DB 目录放入 fake `.sst`/`.log`/`.dbtmp`，观察哪些被清理、哪些保留。

## T612 CompactRange 与手动压缩

- **依赖**：T610, T609
- **上游**：`db/db_impl.cc::CompactRange/TEST_CompactRange/TEST_CompactMemTable`
- **目标**：实现用户触发的范围压缩，语义与上游一致。
- **设计/交付**：
  - 先扫描 L1..L6 计算 `max_level_with_files`；`TEST_CompactMemTable()`（通过 `Write(nullptr)` 等待）确保 L0 清空。
  - 对 0..max_level_with_files-1 逐层 `TEST_CompactRange`。
  - ManualCompaction 用内部键表示范围：begin = `(user, kMaxSequence, kValueTypeForSeek)`，end = `(user, 0, type 0)`；NULL 表示无界。
  - 锁内循环：idle 时安装 manual_compaction 并调度；否则等待；结束后等待后台任务彻底退出；异常时取消。
  - 后台对手动任务调用 `CompactRange`（选文件），每次只处理一段；未完成则更新 begin 继续，直到 done 或错误/shutdown。
- **验收**：
  - 空 DB、全范围、半开/闭区间、单 key 范围；压缩后 Get/迭代正确。
  - 多段范围最终全部完成；文件数下降；MANIFEST 正确。
  - shutdown/bg_error 时能中止并返回/等待，不悬挂。
- **吃透检查点**：
  - 为什么 begin 用 kMaxSequence、end 用序列 0 和 type 0？
  - 为什么手动压缩每次只做一段，而不是一次性选完整个范围？
  - `TEST_CompactMemTable` 为什么用写 nullptr 的方式等待？
  - max_level_with_files 为什么从 1 开始统计，而压缩从 level 0 开始？
- **实验**：写入跨越 L0–L3 的 key，调用 CompactRange 前后打印各层文件数与 sstables 属性。

## T613 GetProperty 与 GetApproximateSizes

- **依赖**：T504, T601
- **上游**：`db/db_impl.cc::GetProperty/GetApproximateSizes`
- **目标**：提供诊断属性和范围大小估算。
- **设计/交付**：
  - 属性前缀 `leveldb.`；实现 `num-files-at-level<N>`、`stats`、`sstables`、`approximate-memory-usage`；未知/格式错误返回 false。
  - stats 输出格式与上游一致（层级、文件数、MB、时间、读写 MB）；sstables 用 Version::DebugString。
  - approximate-memory-usage = block_cache→TotalCharge + mem/imm 估算。
  - GetApproximateSizes：对每个 range 以 kMaxSequence/kValueTypeForSeek 构造内部键，调用 ApproximateOffsetOf，limit<start 时返回 0。
- **验收**：
  - 各属性字符串格式与上游一致；非法 level/未知属性。
  - 近似大小单调、包含边界、空范围；与实际文件大小同数量级。
  - 并发写入时不崩溃（允许估算波动）。
- **吃透检查点**：
  - 为什么属性名要带 `leveldb.` 前缀？C 版是否保留同名以兼容工具？
  - approximate sizes 为什么是“近似”，误差来自哪里？
  - 为什么估算也使用内部键而不是用户键？
- **实验**：写入已知大小数据，比较估算值与实际 SSTable 字节数。

## T614 DestroyDB

- **依赖**：T604, T116
- **上游**：`db/db_impl.cc::DestroyDB`
- **目标**：安全删除整个数据库目录中的 LevelDB 文件。
- **设计/交付**：
  - GetChildren；目录不存在返回 OK。
  - LockFile 成功才继续；遍历并 `ParseFileName`，删除除 LOCK 外的所有 LevelDB 文件；记录第一个删除错误。
  - 解锁后删除 LOCK，最后 RemoveDir（忽略非空错误）。
  - 不删除目录中的非 LevelDB 文件。
- **验收**：
  - 正常 DB 删除后目录消失/仅剩外部文件；再次打开可重建。
  - 目录不存在返回 OK；锁被占用时报错且不删数据；删除失败传播错误。
  - 不影响其他 DB。
- **吃透检查点**：
  - 为什么先锁再删、最后删 LOCK？
  - 为什么目录不存在算成功？
  - 为什么最后 RemoveDir 的错误可以忽略？
- **实验**：在 DB 目录放一个 `keep.txt`，DestroyDB 后确认其保留。

## T615 db_test 基础用例移植

- **依赖**：T601–T614
- **上游**：`db/db_test.cc` 的基础部分、`db/autocompact_test.cc`
- **目标**：用上游行为测试锁定 DB 语义。
- **验收**：
  - 移植：Empty、Get/ Put/ Delete、Overwrite、Sequence、Open/Close、Recover、CompactRange、ApproximateSizes、Destroy、Properties、SmallWriteBuffer 强制压缩、Reopen。
  - 数据模型对拍；错误路径；测试可并行运行（每个用例独立临时目录）。
  - 全部通过、无泄漏。
- **吃透检查点**：
  - 哪些 db_test 用例依赖 `SpecialEnv` 注入？C 版如何用 EnvWrapper 替代？
  - 为什么测试要覆盖“重开”而不只是同进程读？
- **实验**：把某个用例的 write_buffer/max_file 调到极小，观察测试是否仍通过以及触发路径。

## T616 并发、快照与迭代器压力测试

- **依赖**：T605–T608, T615
- **上游**：`db/db_test.cc` 并发部分、`issues/issue178_test.cc`、`issues/issue200_test.cc`
- **目标**：验证多线程读写、组提交、迭代器/快照生命周期。
- **验收**：
  - 多写者 + 多读者 + 迭代器 + 快照 + 后台压缩并发运行，无数据竞争/崩溃。
  - 快照隔离；迭代器始终看到一致快照；关闭时等待后台。
  - 小 max_open_files、频繁表打开/淘汰；文件描述符不泄漏。
  - TSAN（Linux）/压力循环通过；至少覆盖一次错误注入。
- **吃透检查点**：
  - 哪些共享状态只在 mutex 下访问？哪些用原子变量？
  - 迭代器/快照引用计数与 DB 关闭的竞态如何避免？
  - 写队列的唤醒条件是否存在丢失唤醒风险？
- **实验**：8 写 8 读持续 30 秒，同时每 10ms 创建/释放快照，记录吞吐与最大延迟。

## T617 崩溃恢复与故障注入验收

- **依赖**：T603, T604, T610, T611, T616
- **上游**：`db/recovery_test.cc`、`db/corruption_test.cc`、`db/fault_injection_test.cc`
- **目标**：验证不同崩溃点下“已确认写入不丢、未确认可丢、DB 可恢复且不误报”。
- **设计/交付**：
  - FaultEnv 支持在 Append/Sync/Close/Rename/Remove/Create 等调用前后注入失败或“崩溃”（直接停止后续 IO）。
  - 崩溃点矩阵：WAL 写前/写后/ Sync 前/ Sync 后；memtable 切换；BuildTable 中；MANIFEST 追加/ Sync 前；CURRENT rename 前/后；compaction 输出各阶段；RemoveObsoleteFiles。
  - 恢复后校验：sync 成功返回的数据必须存在；非 sync 允许丢失尾部；版本引用文件完整；无非法 MANIFEST。
  - 损坏注入：WAL/SSTable/MANIFEST/CURRENT 截断、字节翻转、缺失文件；paranoid 开关两种行为。
- **验收**：
  - 每个崩溃点重复 N 次（随机种子固定），恢复成功或明确失败，不崩溃/死循环。
  - 已 Sync 写永不丢失；恢复后继续读写正常。
  - corruption_test/recovery_test/fault_injection_test 的可移植用例通过。
  - ASan/LSan；错误路径无 FD 泄漏。
- **吃透检查点**：
  - 每个崩溃点分别依赖哪条提交顺序保证？
  - 为什么 MANIFEST 追加成功但 CURRENT 未切换时旧版本仍可用？
  - 为什么 compaction 输出必须先于 MANIFEST 引用存在？
  - 哪些损坏可以修复，哪些只能报错？RepairDB 的边界在哪里？
- **实验**：在 CURRENT rename 前后各“崩溃”一次，重启比较 DB 内容差异。

## T618 P6 阶段门：单线程正确性 + 并发 + 崩溃恢复

- **依赖**：T601–T617
- **目标**：证明完整 DB 在正常、并发、故障三种条件下的行为符合规格，可以进入工具/API 阶段。
- **验收**：
  - 无故障环境下 db_test 基础/并发用例全绿；重开数据一致。
  - 崩溃矩阵通过；sync 持久性证明；错误传播符合 FR-20。
  - 代码审阅：锁顺序、引用计数、错误清理路径逐项检查；ASan/TSAN。
- **吃透检查点**：
  - 不看源码画出写路径与读路径的时序图，标出锁的释放/获取点。
  - 解释一次“写入→memtable 满→imm→L0→compaction→MANIFEST 安装”的完整状态变化。
  - 列举至少 6 个崩溃点及各自的恢复依据。
- **实验**：把 DB 的流程图与 `doc/plan.md` 的不变量清单逐条对照，标记实现位置。

# P7：工具、修复与公开 API

## T701 RepairDB

- **依赖**：T405, T406, T502, T505, T603
- **上游**：`db/repair.cc`
- **目标**：在 MANIFEST/CURRENT 损坏或丢失时，从日志和表中尽可能恢复数据。
- **设计/交付**：
  - FindFiles：列出目录；收集 MANIFEST、日志号、表号；`next_file_number = max(已见编号+1, 1)`；空目录报错。
  - ConvertLogFilesToTable：逐日志读取（**不校验 checksum**，损坏处跳过并继续）；小于 12 字节记录跳过；坏 batch 忽略并继续；把可解析内容写入新表；把原日志归档到 `<db>/lost/`。
  - ExtractMetaData：对每个表先用 `.ldb`、再用 `.sst` 获取大小；扫描所有条目得到 smallest/largest/max_sequence；扫描失败则 RepairTable——把可读条目复制到新表、归档原文件、空表则放弃。
  - WriteDescriptor：生成临时 MANIFEST：comparator、log=0、next_file、last_sequence=max；**所有表都放 L0**；写一条记录；Close 后归档所有旧 MANIFEST；重命名为 `MANIFEST-000001`；SetCurrentFile；失败删除临时文件。
  - 归档策略：移动到 `lost/`，不直接删除；记录日志。
- **验收**：
  - MANIFEST 丢失但表/日志完好：修复后可读，日志中的最新数据被保留。
  - 单个表 CRC/截断损坏：可读部分被复制到新表；不可读表被归档。
  - 修复后 next_file 大于所有已见编号；不会覆盖存活文件；last_sequence 不低于表中最大序列。
  - 损坏日志/坏 batch 不阻止其余日志修复；空目录/无文件返回错误。
  - 修复过程幂等（再次修复不破坏结果）；ASan。
- **吃透检查点**：
  - 为什么修复器把所有表放到 L0？这对读取正确性有什么代价？
  - 为什么转换日志时不校验 CRC，而正常恢复时校验？
  - RepairTable 为什么只能保留“可读条目”？复制后为什么归档原表而不是删除？
  - 为什么 next_file_number 要取最大已见编号+1？
  - 修复后 `log_number=0` 意味着什么？旧日志会怎样？
- **实验**：删掉 CURRENT 和一个表的部分尾部，运行 RepairDB，比较修复前后可读 key 集合。

## T702 dump/诊断工具

- **依赖**：T203, T406, T502
- **上游**：`db/dumpfile.cc`、`db/leveldbutil.cc`、`include/leveldb/dumpfile.h`
- **目标**：提供可独立运行的 `cldbtool dump`，解析日志、MANIFEST、SSTable。
- **设计/交付**：
  - 按 `ParseFileName` 推断类型；未知类型报 InvalidArgument。
  - 日志：log::Reader（checksum=true），每条记录按 WriteBatch 解析，输出 `offset/sequence/put/del`，转义不可打印字符；记录过短/坏 batch 输出错误但继续。
  - MANIFEST：逐条 VersionEdit 解码并输出 DebugString；损坏报告 offset/bytes。
  - 表：打开 Table（默认 comparator 仅用于顺序遍历，不做 Seek/Prev），输出 `'user' @ seq : val/del => 'value'`；坏键转义输出；迭代器错误写到输出。
  - stdout WritableFile 适配器；工具不依赖 DB 打开，不修改文件。
- **验收**：
  - 对正常 DB 的 LOG/MANIFEST/.ldb 输出可读；与上游 dumpfile 输出语义一致。
  - 截断/CRC 错误文件不崩溃，输出 corruption 信息并非零退出。
  - 未知文件名/不存在文件错误清晰。
- **吃透检查点**：
  - 为什么 dump 表时不能使用数据库的自定义 comparator？
  - 为什么 dump 工具对损坏应继续而不是立即退出？
  - DumpFile 为什么能独立于 VersionSet 工作？
- **实验**：dump 一个 compaction 后的 MANIFEST，手工还原出每层文件布局。

## T703 公开 C API 与示例

- **依赖**：T601–T614, T109, T111, T113
- **上游**：`include/leveldb/db.h`、`options.h`、`write_batch.h`、`iterator.h`、`snapshot.h`、`cache.h`、`filter_policy.h`、`env.h`、`comparator.h`
- **目标**：交付稳定、可嵌入、所有权明确的 C API（草案见 `plan.md` §6.8）。
- **设计/交付**：
  - `include/cleveldb/cldb.h`：不透明 `cldb_t`、`cldb_iterator_t`、`cldb_snapshot_t`、`cldb_write_batch_t`；`cldb_options_t` + `cldb_options_init`；读写选项；`cldb_slice_t`；`cldb_status_t`。
  - API：open/close/put/delete/write/get/free/new_iterator（valid/seek/seek_to_first/seek_to_last/next/prev/key/value/status/destroy）/get_snapshot/release_snapshot/compact_range/get_property/get_approximate_sizes/destroy_db/repair_db；WriteBatch 的 create/put/delete/clear/destroy。
  - 错误模型：所有可能失败的操作返回 `cldb_status_t`（内联 message）；`cldb_get` 的 value 由库分配、`cldb_free` 释放；NotFound 时 `*value=NULL`。
  - 选项默认值与上游一致（compression 默认 NO）；`cldb_options_init` 幂等；只增不改的 ABI 约定与版本宏。
  - 线程安全说明：DB 可多线程使用；iterator/snapshot 非线程安全；close 前必须释放所有依赖对象。
  - 示例：`examples/basic.c`（put/get/iterate/snapshot）、`examples/batch.c`；用 C 编译器编译链接；C++ 调用方也可包含头文件。
- **验收**：
  - 示例在 Linux 构建运行；纯 C99/C11 编译无警告（`-Wall -Wextra -Werror`）。
  - API 测试覆盖每个函数、错误码、所有权、NULL 参数；valgrind/ASan 无泄漏。
  - 公开头文件不包含内部结构体；内部头文件不被安装。
  - 用 C++ 程序调用 C API 可正常链接。
- **吃透检查点**：
  - 为什么公开 API 用不透明指针，而不是暴露结构体布局？
  - `cldb_status_t` 内联 message 与上游堆分配 Status 的取舍是什么？
  - 返回字符串的所有权为什么必须统一？
  - 迭代器/快照为什么要求用户先释放再 close？
- **实验**：用示例程序写入 10 万条并遍历，检查 valgrind 报告。

## T704 可选压缩后端（Snappy/Zstd）

- **依赖**：T403, T405, T406, T116
- **上游**：`table/format.cc`、`table/table_builder.cc`、`port/port_stdcxx.h`、上游构建脚本的 Snappy/Zstd 检测
- **目标**：支持读取/写入 Snappy 与 Zstd 压缩块，保持与上游磁盘格式一致。
- **设计/交付**：
  - 构建开关：Make 变量 `CLDB_WITH_SNAPPY=1`/`CLDB_WITH_ZSTD=1`（默认关闭）；开启时同时找到头文件和库，定义 `CLDB_HAVE_SNAPPY=1`/`CLDB_HAVE_ZSTD=1` 并链接，构建期缺依赖直接报错。
  - Snappy：只使用 `snappy-c.h` 的 C API（`snappy_max_compressed_length`、`snappy_compress`、`snappy_uncompressed_length`、`snappy_uncompress`），不直接调用 C++ `snappy::*`；注意系统 `libsnappy.so` 自身可能传递依赖 libstdc++，这是可接受的库依赖，不来自本项目源码。
  - Zstd：使用 `zstd.h` 的 C API（`ZSTD_compressBound`、`ZSTD_compress`、`ZSTD_getFrameContentSize`、`ZSTD_decompress`、`ZSTD_isError`）；一次性 API 足够，压缩字节不要求与上游逐字节相同，但必须是合法 zstd frame。
  - 第三方库不复制源码/子模块；Debian/Ubuntu 包为 `libsnappy-dev`、`libzstd-dev`，Fedora 为 `snappy-devel`、`libzstd-devel`；启用时在 `THIRD_PARTY_NOTICES` 记录许可证。
  - TableBuilder：当 `compression != kNoCompression` 时调用对应压缩；**仅当压缩后大小 < `raw - raw/8`（严格超过 12.5% 的节省）** 才使用，否则写原块；块 trailer 写 0/1/2；zstd 使用 `zstd_compression_level`（默认 1）。
  - ReadBlock：按 trailer type 分发；未知类型返回 Corruption；校验 CRC 覆盖“块内容（压缩态）+ type byte”；解压失败报 Corruption。
  - 请求了未编译的后端时：与上游一致，写路径静默回退为未压缩块（type=0）；读路径遇到不支持的 type=1/2 返回 Corruption（错误文案区分“length/contents”）。
  - 压缩范围：data block、metaindex block、index block 都经 `WriteBlock`，可被压缩；filter block 用 `WriteRawBlock` 固定 `kNoCompression`；footer 永不压缩。
- **验收**：
  - 四种组合 round-trip：no/no、snappy、zstd；12.5% 阈值上下；不可压缩数据回退 no。
  - CRC 覆盖范围字节级 golden；损坏压缩块/未知 type 的错误。
  - C 写 → C++ 读、C++ 写 → C 读（启用对应库）；C++ 默认 Snappy DB 可读。
  - 默认构建（两开关关闭）不需要安装 Snappy/Zstd，测试程序不链接 libstdc++；分别开启两个开关都能构建并通过测试。
  - 未启用库时设置压缩不崩溃，写路径回退为 type=0；读取上游 Snappy/Zstd 块返回明确的 Corruption。
- **吃透检查点**：
  - 为什么压缩单位是 block 而不是整个 SSTable？
  - 12.5% 阈值解决什么问题？阈值以下仍压缩会怎样？
  - 哪些块会被压缩？filter block 为什么必须保持未压缩？
  - block trailer 的 type 为什么也参与 CRC？
  - Zstd level 放在哪里？同一 DB 不同块可以混用压缩类型吗？
  - Snappy/Zstd 格式与 C++ 实现如何保证互操作？
  - Snappy 的 C API 与 C++ `snappy::*` 输出格式是什么关系？为什么 C 项目不能直接调用 C++ 接口？
  - 系统 `libsnappy` 为什么会传递依赖 libstdc++？如果要求进程完全不加载 libstdc++，有哪些选择、代价是什么？
  - 为什么 Zstd 压缩字节不要求与上游逐字节相同，而块格式、type byte 和 CRC 必须完全一致？
- **实验**：用可压缩与随机数据各写一个块，打印压缩前后大小与最终 type。

## T705 MemEnv 公共化与自定义 Env 示例

- **依赖**：T115, T113, T117
- **上游**：`helpers/memenv/memenv.h/.cc`、`util/env_test.cc`
- **目标**：提供内存 Env 供测试/教学，并演示如何实现自定义 Env/Comparator/FilterPolicy。
- **设计/交付**：
  - `cldb_memenv_create()`：完整实现文件/目录/锁/日志/时间/调度；文件内容存内存，支持 append/random access/rename；锁语义与真实 Env 一致。
  - 示例 `examples/custom_env.c`：EnvWrapper 统计读写字节/延迟；`examples/custom_comparator.c`（如反序或大小写不敏感）与自定义 FilterPolicy。
  - 文档说明自定义回调的线程安全、生命周期、返回值约定。
- **验收**：
  - 在 MemEnv 上跑 db_test 的子集与崩溃恢复；数据不落盘。
  - 自定义 Env 统计准确；自定义 comparator 的 DB 可读写且迭代顺序正确。
  - 内存 Env 的并发锁/POSIX 语义测试；无泄漏。
- **吃透检查点**：
  - MemEnv 为什么能复用同一 DB 实现而不改一行 DB 代码？
  - 自定义 comparator 必须满足哪些性质？不满足时 LevelDB 会怎样失败？
  - 自定义 FilterPolicy 的 key 变换与误判率如何影响正确性/性能？
  - Env 的哪些方法可以返回 NotImplemented，哪些绝不能？
- **实验**：在 MemEnv 上创建 DB 并模拟一次“重启”（保留内存文件表），验证恢复。

## T706 文档与 API 参考整合

- **依赖**：T117, T204, T306, T409, T512, T618（各阶段门）, T701–T705
- **目标**：让仓库本身成为可学习、可维护的 LevelDB C 实现。
- **设计/交付**：
  - `doc/index.md`：公开 API 语义、快照、同步写、比较器、缓存/过滤器、线程与所有权。
  - `doc/architecture.md`（或引用 `plan.md`）：读写路径、版本管理、压缩、恢复。
  - `doc/format.md`：字节级磁盘格式（可与 plan 附录互链）。
  - `README.md`：Linux 构建、测试、工具、示例、兼容性声明与已知限制。
  - `doc/compatibility.md`：与 C++ LevelDB 兼容矩阵（无压缩/可选压缩、可读/可写）。
- **验收**：
  - 新读者按 README 能在 Linux 上构建并运行示例/测试。
  - 文档中的命令/API 与实际代码一致（CI 或脚本校验示例编译）。
  - 关键设计差异（与上游不同或暂未实现的部分）在文档中有明确说明。
- **吃透检查点**：
  - 如果让你向同事讲 30 分钟 LevelDB，你会用哪几张图？文档是否支持？
  - 哪些实现差异是“有意为之”，哪些是“暂未实现”？是否写清楚？
- **实验**：按文档从零构建一次，记录所有需要补充的步骤。

## T707 P7 阶段门：修复、工具、API 与文档

- **依赖**：T701–T706
- **目标**：确认数据库不仅能跑，还能被诊断、修复、嵌入和教会别人。
- **验收**：
  - RepairDB 在损坏矩阵上能救回可读数据并把不可读文件归档。
  - dump 工具可解析日志/MANIFEST/表；错误输入不崩溃。
  - 公开 C API 示例与 API 测试通过；Linux 上 GCC/Clang 构建通过；无公开内部头。
  - 文档齐全，兼容性声明准确。
- **吃透检查点**：
  - 修复器与正常恢复的取舍分别是什么？何时应选择哪一个？
  - 工具链如何帮助定位一次真实的数据损坏？
- **实验**：故意破坏一个测试 DB，依次用 dump、RepairDB、正常 Open 处理，记录三者行为差异。

# P8：兼容性、加固与验收

## T801 上游测试全量移植

- **依赖**：T615, T616, T617, T701, T703, T704
- **上游**：`db/*_test.cc`、`table/*_test.cc`、`util/*_test.cc`、`helpers/memenv/memenv_test.cc`、`issues/issue*_test.cc`
- **目标**：把上游行为测试尽可能完整地移植到 C 测试框架，作为最终回归基线。
- **设计/交付**：
  - 逐文件清单（按依赖顺序）：
    - util：arena、bloom、cache、coding、crc32c、env（Linux 分支）、hash、logging、status；
    - db：dbformat、filename、log、write_batch、version_edit、version_set、skiplist；
    - table：filter_block、table；
    - db 集成：db、autocompact、recovery、corruption、fault_injection；
    - issues：178、200、320；
    - memenv、no_destructor（C 等价断言/可标记 N/A 并说明）。
  - 补齐 C 测试框架能力：临时目录/清理、测试夹具、参数化、死亡/断言检查（如需要）、内存统计、超时、随机种子输出。
  - 上游 gtest 断言逐条映射；对依赖 C++ 特性的用例（如 `SpecialEnv`、thread-local）用 EnvWrapper/Linux 平台层等价实现；无法移植的用例记录原因并给出替代覆盖。
- **验收**：
  - 所有可移植用例通过；跳过清单为零或每项有明确理由与替代测试。
  - 测试互不干扰、可并行/重复运行；随机测试种子可复现。
  - 单测总运行时间可接受；CI 脚本一键执行。
- **吃透检查点**：
  - 哪些用例真正验证了 LevelDB 的核心不变量，哪些只是 API 便利性？
  - 上游测试里的“隐含假设”（文件号、层数、顺序）在 C 实现中是否仍成立？
  - 如果某个测试只能通过改实现才能过，应该先怀疑测试还是实现？
- **实验**：随机挑 3 个上游测试，先不看实现，凭测试名预测它验证的行为，再运行验证。

## T802 与 C++ LevelDB 的双向磁盘兼容

- **依赖**：T801, T704, T706
- **上游**：`db/filename.cc`、`table/format.cc`、`db/version_edit.cc`、C++ 侧 `leveldb::DB`
- **目标**：证明 C 实现写出的 DB 能被同版本 C++ 打开，反之亦然。
- **设计/交付**：
  - 构建 C++ 参考程序（使用 `E:/github/leveldb` 源码；无压缩配置），提供 create/verify/list 命令。
  - 兼容矩阵：
    1. C++ 建库（无压缩）→ C 读/写 → C++ 再读；
    2. C 建库 → C++ 读/写 → C 再读；
    3. 双方交替执行 Put/Delete/CompactRange/重开；
    4. 启用 Bloom filter（同名 `leveldb.BuiltinBloomFilter2`）与禁用两种；
    5. T704 后：C++ Snappy/Zstd 写 → C 读，C Snappy/Zstd 写 → C++ 读；
    6. `.ldb` 与旧 `.sst` 文件名兼容。
  - 固定使用相同 comparator 名称、block/restart/max_file/write_buffer 选项；比较逻辑内容而非文件字节（文件号/压缩选择允许不同）。
  - 明确不支持项（如未来新增 tag、未知压缩类型）以及检测方式。
- **验收**：
  - 矩阵全部通过；C++ 打开 C DB 无 corruption；C 打开 C++ DB 无 corruption。
  - MANIFEST/WAL/表/过滤器互操作；交替写入后数据一致。
  - 兼容脚本可重复执行并输出矩阵报告 `doc/compatibility.md`。
- **吃透检查点**：
  - 哪些磁盘结构是“格式契约”，哪些只是实现细节（文件号、层选择）？
  - 为什么 comparator 名称必须精确匹配？过滤器名称呢？
  - 压缩块类型字段如何影响向前兼容？未知 type 应如何处理？
  - 如果 C 版块大小与 C++ 不同，会影响互操作吗？为什么？
- **实验**：用 C++ 建库并在 C 中 compact，再用 C++ 打开并检查 MANIFEST 中的层布局变化。

## T803 随机化差分测试与模型检验

- **依赖**：T801, T802
- **目标**：用模型和 C++ 参考实现发现边界/并发/恢复缺陷。
- **设计/交付**：
  - 内存模型：有序 map<user_key, (value or deleted, seq)>；维护快照序列集合；实现 get/iterate/range 的期望值。
  - 随机操作流：Put/Delete/WriteBatch（含重复 key）/Get/迭代/Seek/快照/CompactRange/重开/小 buffer；参数（key 空间、value 大小、batch 大小、压缩）随机化并记录种子。
  - 差分 oracle：
    - 与内存模型比较可见值/迭代序列；
    - 与 C++ LevelDB 比较相同操作流后的完整内容与快照视图；
    - 重开后与模型比较；
    - 可选的崩溃注入后只允许丢失未 sync 的尾部。
  - 失败时打印最小复现操作序列（缩减/重放）；保留 DB 目录。
- **验收**：
  - 固定种子回归集 + 长时间随机跑（至少数小时）无差异。
  - 覆盖所有层、所有迭代方向、删除/覆盖、快照、批量原子性。
  - 发现的问题都有最小用例并已修复/记录。
- **吃透检查点**：
  - 模型为什么不记录物理文件，而只记录用户可见状态？
  - 快照在模型中如何表示？compaction 后模型如何验证旧版本仍可见？
  - 差分测试与单元测试各自能发现哪类问题？
- **实验**：用一个会“忘记快照”的故意 bug 跑差分，确认测试能定位。

## T804 Sanitizer、模糊测试与长时间压力

- **依赖**：T801, T803
- **目标**：证明内存安全、线程安全、资源不泄漏，并覆盖解码器边界。
- **设计/交付**：
  - 构建矩阵（Linux）：Debug/Release；ASan+UBSan；LSan；TSan；Valgrind（可选）。
  - 全测试套件在 sanitizer 下运行；并发测试在 TSan 下运行。
  - 解码器 fuzz 入口：WAL 记录、VersionEdit/MANIFEST、SSTable block/footer/filter、WriteBatch；使用 libFuzzer 或简单随机变异，语料来自正常文件。
  - 长时间压力：多线程读写/迭代/快照/压缩/重开循环；小 max_open_files；磁盘将满/删除失败等 Env 注入；监控 RSS/FD/句柄数。
- **验收**：
  - ASan/UBSan/TSan 无报告；Valgrind 无 definitely lost。
  - fuzz 至少达到设定时长/迭代次数，无崩溃/越界/死循环；发现的问题有回归用例。
  - 压力测试无 FD 增长、无内存无界增长；关闭/重开稳定。
- **吃透检查点**：
  - 哪些 bug 只有 TSan 能发现？为什么普通压力测试可能漏掉？
  - LSM 实现中最容易出现的资源泄漏点有哪些？
  - 解码器 fuzz 与语义测试的边界在哪里？
- **实验**：在 fuzz 入口注入一个长度字段溢出，观察 sanitizer 报告栈。

## T805 db_bench 移植与性能报告

- **依赖**：T801, T802, T804
- **上游**：`benchmarks/db_bench.cc`、`benchmarks/db_bench_log.cc`
- **目标**：建立可重复的性能基线，确认 C 实现与 C++ 同数量级。
- **设计/交付**：
  - 移植核心工作负载：fillseq、fillrandom、overwrite、fillsync、readseq、readrandom、readmissing、readhot、seekrandom、compact、readwhilewriting（可选）。
  - 参数：num、value_size、key_size、threads、write_buffer_size、max_file_size、compression、sync、benchmarks 列表；随机种子固定。
  - 输出：ops/sec、平均/p99 延迟、总字节、DB 大小、编译/运行环境；生成 `doc/benchmark.md` 与原始 CSV。
  - 与同一台 Linux 机器上的 C++ LevelDB 对照（相同选项与文件系统）；分析主要差异（编译优化、Env 实现、锁、CRC 实现）。
  - 性能目标：核心读写吞吐/延迟与 C++ 同数量级（建议 0.5×–2×），无明显退化；若超出，给出原因与改进项。
- **验收**：
  - 一键脚本完成 C 与 C++ 对照并产出报告。
  - 结果可重复（同机多次波动可解释）；性能回归可比较。
  - 识别并记录至少 1 个热点优化机会（不要求实现）。
- **吃透检查点**：
  - fillrandom 与 fillseq 的写放大差异来自哪里？
  - readrandom 的延迟由哪些层/缓存决定？
  - 组提交、bloom filter、block cache 如何影响基准结果？
  - 为什么不能简单比较两台不同机器的绝对值？
- **实验**：在同一 DB 上先跑 fillrandom 再跑 readrandom，改变 block_cache 大小，观察命中率与吞吐。

## T806 最终审计与学习验收

- **依赖**：T801–T805
- **目标**：确认实现、文档、测试、兼容性和学习目标全部闭环。
- **设计/交付**：
  - 追溯矩阵：逐条核对 `plan.md` 的 FR-01–FR-20、NFR-01–NFR-08 与任务/测试/文档的对应关系，修正文档不一致。
  - 发布检查：干净目录构建（Linux、GCC 与 Clang）；全部测试、兼容矩阵、sanitizer、benchmark；工具/示例可运行。
  - 许可证与归属：保留上游 `LICENSE`/`AUTHORS` 版权声明，README 明确“LevelDB 的 C 语言学习性移植”，不暗示官方；列出与上游的差异和未实现项。若启用 Snappy/Zstd，更新 `THIRD_PARTY_NOTICES` 与 README 的依赖说明。
  - 学习验收（口述/书面）：
    1. 画出 LSM 读写路径、版本引用关系、压缩选择流程；
    2. 解释 WAL 分片与 CRC、MANIFEST/CURRENT 提交、组提交、快照与 compaction 丢弃规则；
    3. 说明 Iterator 栈（DBIter→Merging→TwoLevel/Block）与 bloom/filter/block cache 的作用；
    4. 列举崩溃点与恢复依据，解释 sync 的边界；
    5. 解释为什么 L0 特殊、trivial move、seek compaction、文件号分配；
    6. 对照 C++/C 差异说明每个设计取舍。
  - 每个任务的增量代码已审阅、吃透检查点已自检；未解决问题列入 TODO。
- **验收**：
  - 追溯矩阵无缺口或每项缺口有明确说明；所有阶段门通过。
  - 从干净 clone 按 README 一次构建成功；测试/兼容/工具全部通过。
  - 能不看源码回答上述 6 组问题，并现场修改一个小功能/写一个小测试验证理解。
- **吃透检查点**：
  - LevelDB 最关键的 5 个不变量是什么？实现中分别由哪些代码保证？
  - 如果要把 write_buffer 改小 10 倍，哪些路径/阈值会受影响？
  - 如果新增一种压缩算法，需要改哪些层？磁盘兼容如何保持？
  - 如果要求支持并发 compaction，现有设计哪些假设会被打破？
- **实验**：从零按文档实现一个最小功能变更（如新增属性），走完编码→测试→文档→兼容检查流程。

