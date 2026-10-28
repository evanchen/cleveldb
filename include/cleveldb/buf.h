#ifndef CLDB_INCLUDE_CLEVELDB_BUF_H_
#define CLDB_INCLUDE_CLEVELDB_BUF_H_

#include <stdbool.h>
#include <stddef.h>

#include "cleveldb/slice.h"

/* 可增长字节缓冲；替代上游把 std::string 当字节数组用的场景
 * （如 WriteBatch::rep_、BlockBuilder 的输出、Env 的文件内容与测试目录）。
 * 拥有 data，并保证 data[size] == '\0'，可直接当 C 字符串读取。 */
typedef struct cldb_buf {
  char *data;      /* owned；capacity 为 0 时可为 NULL；对应 std::string 存储 */
  size_t size;     /* 已用字节，不含 NUL；对应 std::string::size() */
  size_t capacity; /* 已分配字节，含 NUL；std::string 内部维护，C 版显式保存 */
} cldb_buf_t;

/* 置为空缓冲；对应 std::string 默认构造。接受 NULL。 */
void cldb_buf_init(cldb_buf_t *buf);

/* 释放存储并重置；对应 std::string 析构。接受 NULL。 */
void cldb_buf_free(cldb_buf_t *buf);

/* 清空内容但保留容量；对应 std::string::clear()。接受 NULL。 */
void cldb_buf_clear(cldb_buf_t *buf);

/* 保证 capacity >= requested（含结尾 NUL）；对应 std::string::reserve，
 * 扩容后旧内容自动保留。失败时原内容不变。 */
bool cldb_buf_reserve(cldb_buf_t *buf, size_t requested);

/* 设置长度，新增字节补 '\0'；对应 std::string::resize()。
 * 失败时原内容不变。 */
bool cldb_buf_resize(cldb_buf_t *buf, size_t size);

/* 追加任意字节；对应 append(data, n)。data 可指向本缓冲，
 * 扩容后按偏移重新定位，避免悬垂。 */
bool cldb_buf_append(cldb_buf_t *buf, const void *data, size_t size);

/* 追加单字节；对应 std::string::push_back()。 */
bool cldb_buf_append_char(cldb_buf_t *buf, char ch);

/* 追加 Slice；对应上游 append(slice.data(), slice.size()) 的惯用法。 */
bool cldb_buf_append_slice(cldb_buf_t *buf, cldb_slice_t slice);

/* 替换全部内容；对应 std::string::assign()。source 可指向本缓冲。 */
bool cldb_buf_assign(cldb_buf_t *buf, const void *data, size_t size);

/* 交换两个缓冲；对应 std::string::swap()，O(1) 转移所有权。
 * 任一为 NULL 时不做任何事。 */
void cldb_buf_swap(cldb_buf_t *a, cldb_buf_t *b);

/* 借用视图供 Slice 接口使用；对应从 std::string 构造 Slice。
 * 视图在下次修改或释放前有效；空缓冲返回空 Slice。 */
cldb_slice_t cldb_buf_slice(const cldb_buf_t *buf);

/* NUL 结尾视图；对应 std::string::c_str()，上游日志、路径接口依赖它。
 * 永不为 NULL；含 '\0' 的二进制内容只适合当 C 字符串处理。 */
const char *cldb_buf_cstr(const cldb_buf_t *buf);

#endif /* CLDB_INCLUDE_CLEVELDB_BUF_H_ */
