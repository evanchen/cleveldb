# cleveldb 所有权与生命周期规则

| 项目 | 内容 |
|---|---|
| 适用任务 | T003 |
| 相关文档 | `doc/coding-style.md`、`doc/plan.md` §6.5 |
| 示例 | `src/util/example.{h,c}` |

C 没有析构函数和所有权类型，资源释放只能靠**约定 + 注释 + 审查**保证。本文件定义全项目的释放责任与引用期限规则；每个结构体在头文件中按第 4 节模板标注。

## 1. 术语

| 术语 | 含义 | 谁释放 |
|---|---|---|
| 持有并负责释放 | 结构或对象保存指针，并负责释放一次 | 持有者 |
| 仅引用 | 只保存指针，不负责释放 | 原持有者 |
| 移交 | 调用后由被调用者负责释放 | 接收者 |
| value type | 按值拷贝即可，无堆资源 | 无 |
| internal refcount | 内部共享对象按引用计数存活 | 最后一个 unref 者 |
| public handle | 用户通过 create/open 获得的不透明对象 | 用户通过对应 API |

`const` 只表示只读，**不表示不负责释放**；`T *out` 也不自动表示移交。必须看函数注释。

## 2. 公开 API 规则

### 2.1 对象与句柄

- `cldb_open`/`cldb_*_create` 成功返回的对象由调用者拥有，必须用配对的 `cldb_close`/`cldb_*_destroy` 释放。
- 销毁函数接受 NULL；销毁后句柄立即失效，重复销毁是编程错误。
- 公开对象布局不透明；用户不得 `sizeof`、栈分配或直接访问字段。
- 库不会替用户销毁自定义对象（自定义 Env/Cache/Comparator/FilterPolicy），除非文档明确写了 `owns_*`。

### 2.2 借用返回值

- 迭代器 `key()`/`value()` 返回的 Slice 指向迭代器或底层表的内部存储，只在**下一次移动/Seek/销毁前**有效。
- 快照指针 `cldb_get_snapshot` 返回的是 DB 管理的借用句柄；必须配对 `cldb_release_snapshot`，释放后不得再用。
- `cldb_slice_t` 参数、回调参数、比较器/过滤器收到的 key 都是借用，只在调用期间有效。
- 函数返回内部指针时必须写明有效窗口；没有写明的一律视为借用至下一次修改。
- 允许返回内部只读字符串（如错误消息），但调用者不得 `free`，且不得跨线程修改源对象。

### 2.3 堆内存与 `cldb_free`

- 公开 API 通过 `char **value` 或类似出参返回的堆内存，统一由调用者用 `cldb_free` 释放。
- `cldb_get`：成功时 `*value` 由库分配；`NotFound` 时 `*value = NULL`；其他错误时也不返回可释放指针。
- `cldb_free(NULL)` 允许；不要用 `free` 释放库返回的块，避免分配器/模块边界不一致。
- `cldb_free` 只用于文档标注“调用者释放”的返回值，不用于普通栈变量或借用指针。
- 空结果与分配失败必须区分：空值用 `NULL`/size 0，分配失败用错误状态。

### 2.4 options、回调与外部对象

- `cldb_options_t` 在 `cldb_open` 时**按值复制**；调用返回后调用者可以修改或释放 options 结构本身。
- options 中的指针（comparator、filter_policy、block_cache、env）不会被复制对象：它们借用给 DB，调用者必须保证其生命周期覆盖整个 DB。
- 自定义对象是否由 DB 销毁由明确的 `owns_*` 字段决定；默认不拥有。
- 回调收到 `void *arg` 与 Slice 都是借用；回调不得保存 Slice 指针，除非文档说明底层存储稳定。
- 回调不得在库持内部锁时重入 DB；需要重入的场景必须由任务设计明确并测试。
- 默认 Env/Cache 由库管理；自定义实例由调用者创建和销毁。

### 2.5 生命周期顺序

- DB 关闭前必须先销毁其迭代器、释放快照；关闭后这些句柄全部失效。
- 自定义 Env/Cache/Comparator/FilterPolicy 必须比 DB 活得久。
- WriteBatch 由调用者创建/销毁；`cldb_write` 只借用，不接管，返回后调用者可继续复用或销毁。
- WriteBatch 内部必须复制 key/value；调用者传入的 Slice 不需要在 batch 生命周期内保持有效。
- Env 创建的文件对象由创建者/使用者配对销毁；DB 在关闭时释放自己打开的文件。

## 3. 内部类型规则

| 类型 | 所有权规则 | 释放方式 |
|---|---|---|
| `cldb_slice_t` | 不拥有底层内存；只保证指针 + 长度 | 无（value type） |
| `cldb_buf_t` | 拥有 `data`；`slice()` 返回借用视图，扩容/释放后失效 | `cldb_buf_free` |
| `cldb_status_t` | 内联 message，按值拷贝安全，无堆指针 | 无 |
| `cldb_write_batch_t` | 调用者拥有；内部缓冲拥有 key/value 副本 | `cldb_write_batch_destroy` |
| `cldb_memtable_t` | 内部共享；`ref/unref` 保护生命周期 | 最后一个 unref 释放 |
| `cldb_version_t` | 内部共享；版本引用由 DB/迭代器/快照持有 | 最后 unref，在 DB 锁规则下 |
| `cldb_table_t` | TableCache 拥有；缓存句柄固定表存活 | cache release / 淘汰 |
| `cldb_iterator_t` | 调用者拥有具体迭代器 | vtable `destroy` / 公开 destroy |
| `cldb_env_t` 及文件对象 | 默认 Env 为进程单例；自定义 Env 调用者拥有 | 不销毁默认；文件对象显式 destroy |
| `cldb_logger_t` | 创建者拥有；DB/Env 不越权释放外部 logger | 对应 destroy/close |

要点：

- 内部 refcount 只解决“还有谁在用”的生存问题，不对外暴露；公开对象仍由明确的 create/destroy 管理。
- 内部共享对象在跨线程传递前必须按任务规定持有引用；不能只借一个裸指针穿过锁边界。
- 内部容器的元素逐个注明释放责任：动态数组存指针时写清元素由谁释放，存 Slice 时写清数据来源与有效范围。

## 4. 结构体字段注释模板

```c
typedef struct cldb_example {
  pthread_mutex_t mutex; /* 保护 name/value */
  char *name;            /* 由本对象释放；受 mutex 保护 */
  int64_t value;         /* 受 mutex 保护 */
} cldb_example_t;

typedef struct cldb_foo {
  char *path;              /* destroy 释放 */
  const cldb_env_t *env;   /* 仅引用；调用者保证比本对象活得久 */
  cldb_buf_t scratch;      /* destroy 释放 data */
  cldb_iterator_t *iter;   /* destroy 前先销毁 iter */
} cldb_foo_t;
```

规则：

- 每个指针字段都写清由谁释放、只引用时的有效范围，并指出保护它的锁（若有）。
- 指针 + 长度成对出现；不要只存指针让读者猜长度归属。
- 数组字段写清元素所有权；`void *` 必须带类型说明或回调约定。
- 结构体头注释写整体生命周期和锁要求，字段注释写局部细节。

## 5. create/destroy 与部分构造

以下模板使用已实现的 `cldb_status_*`。

```c
cldb_status_t cldb_foo_create(const char *name, cldb_foo_t **out) {
  cldb_status_t s = cldb_status_ok();
  cldb_foo_t *foo = NULL;

  if (out == NULL) return cldb_status_invalid_argument("out is NULL", NULL);
  *out = NULL;
  foo = calloc(1, sizeof(*foo));
  if (foo == NULL) {
    s = cldb_status_io("out of memory", NULL);
    goto cleanup;
  }
  foo->name = copy_string(name); /* 返回由调用者 free 的副本，失败为 NULL */
  if (foo->name == NULL) {
    s = cldb_status_io("out of memory", NULL);
    goto cleanup;
  }
  *out = foo;
  foo = NULL;
cleanup:
  cldb_foo_destroy(foo); /* destroy(NULL) 安全 */
  return s;
}
```

- 失败路径必须能用统一的 destroy 清理已构造部分。
- 不在失败路径返回“半可用”对象；输出参数保持 NULL。
- `_destroy` 内部按获取逆序释放，并允许 NULL。
- 引用计数对象用 `_ref/_unref`；`_unref` 在计数归零时释放，调用者不得在 unref 后访问对象。

## 6. 内部引用计数 vs 公开所有权

- 公开对象（DB、迭代器、WriteBatch、Cache、Env 文件）采用**显式所有权**：谁 create 谁 destroy，接口不隐藏释放。
- 内部对象（MemTable、Version、Table）采用**refcount**：多个读者可同时持引用；最后一个 unref 释放。
- refcount 不是公开契约：用户拿不到 ref/unref，也不依赖计数；公开 API 只暴露借用句柄或显式 destroy。
- 快照是两者之间的桥梁：用户拿到借用句柄，DB 内部通过 Version 引用保证数据存活，`release_snapshot` 负责减引用。
- 内部 ref/unref 的锁/原子语义写在相关函数声明或字段注释中；跨线程 unref 需要原子或全局锁，按任务设计选择。

## 7. 典型生命周期

### 7.1 Get

```text
调用者 key(Slice, 借用)
  -> cldb_get
     -> 内部查找，构造 value 副本（由库分配）
  <- *value（由调用者用 cldb_free 释放）
```

调用期间 key 必须有效；返回后 key 可释放。value 与 DB 生命周期无关，DB 关闭后仍可由调用者释放。

### 7.2 迭代器

- `cldb_new_iterator` 返回调用者拥有的迭代器。
- `key()/value()` 借用，下一次 `next/prev/seek` 或 destroy 后失效。
- 迭代器持有必要的内部引用，保证遍历期间表和版本不被释放。
- 必须先 destroy 迭代器再 close DB。

### 7.3 Slice 与 Buffer

- Slice 是“指针 + 长度”的只读视图，不拥有数据。
- `cldb_buf_slice()` 返回借用视图；任何 append/reserve/free 都可能使指针失效。
- 自追加时源 Slice 可能指向同一 buffer，realloc 后必须重新计算或先复制。
- 数据库落盘前必须复制借用数据；不能把用户 Slice 指针存进 MemTable/WAL。

### 7.4 回调与自定义对象

- Comparator/FilterPolicy/Cache/Env 对象由调用者拥有，必须比 DB 活得久。
- 回调参数只在调用期间有效；需要保存时复制。
- 回调返回的 Slice 若由回调内部缓冲提供，必须保证到下一次回调前有效，具体窗口在接口注释中规定。

## 8. 常见错误

| 错误 | 后果 | 正确做法 |
|---|---|---|
| 释放仅引用的 Slice | 重复释放/堆破坏 | 只释放文档标注由调用者释放的返回值 |
| 返回局部数组指针 | 悬垂 | 分配并转移所有权，或让调用者提供缓冲 |
| buffer 扩容后继续用旧 Slice | 悬垂/读到旧数据 | 重新取 slice |
| 把用户 Slice 直接存入 MemTable/WAL | 调用返回后悬垂 | 复制到内部 buffer |
| 关闭 DB 后使用迭代器/快照 | UAF | 先 destroy/release |
| 重复 destroy | 双重释放 | 每次 create 只 destroy 一次 |
| 自定义 Comparator 是栈对象 | DB 运行期悬垂 | 静态/堆分配并保证生命周期 |
| `cldb_free` 释放非库内存 | 分配器不匹配 | 只用于文档标注的返回值 |
| 内部 refcount 未配对 | 泄漏或提前释放 | 每个 ref 恰好一次 unref |

## 9. 审查清单

- [ ] 每个函数注释写明输入/输出由谁释放、只引用时的有效范围。
- [ ] 每个指针字段按模板标注所有权和保护锁。
- [ ] 创建失败时输出参数为 NULL，部分构造可安全销毁。
- [ ] 借用返回值写清有效窗口；调用者不得保存超过窗口的指针。
- [ ] 公开 API 的堆返回值统一 `cldb_free`。
- [ ] 自定义对象生命周期覆盖 DB；回调参数不被保存。
- [ ] 内部 ref/unref 配对，锁/原子语义明确。
- [ ] 迭代器、快照、DB 的销毁顺序正确。
