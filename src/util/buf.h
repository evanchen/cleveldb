#ifndef CLDB_SRC_UTIL_BUF_H_
#define CLDB_SRC_UTIL_BUF_H_

#include <stdbool.h>
#include <stddef.h>

#include "cleveldb/slice.h"

/* 可增长字节缓冲；拥有 data，data[size] 恒为 '\0'。 */
typedef struct cldb_buf {
  char *data;      /* owned；data[size] == '\0'（data 非 NULL 时） */
  size_t size;     /* 已用字节，不含 NUL */
  size_t capacity; /* 已分配字节，含 NUL */
} cldb_buf_t;

/* 初始化为空；接受 NULL。 */
void cldb_buf_init(cldb_buf_t *buf);

/* 释放并置空；接受 NULL。 */
void cldb_buf_free(cldb_buf_t *buf);

/* 清空内容但保留容量；接受 NULL。 */
void cldb_buf_clear(cldb_buf_t *buf);

/* 保证 capacity >= requested（含 NUL）；失败时原内容不变。 */
bool cldb_buf_reserve(cldb_buf_t *buf, size_t requested);

/* 设置 size，新增字节清零；失败时原内容不变。 */
bool cldb_buf_resize(cldb_buf_t *buf, size_t size);

/* 追加任意字节；data 可指向本缓冲。 */
bool cldb_buf_append(cldb_buf_t *buf, const void *data, size_t size);

/* 追加单个字节。 */
bool cldb_buf_append_char(cldb_buf_t *buf, char ch);

/* 追加 Slice 内容。 */
bool cldb_buf_append_slice(cldb_buf_t *buf, cldb_slice_t slice);

/* 替换全部内容；source 可指向本缓冲。 */
bool cldb_buf_assign(cldb_buf_t *buf, const void *data, size_t size);

/* 交换两个缓冲；任一为 NULL 时不做任何事。 */
void cldb_buf_swap(cldb_buf_t *a, cldb_buf_t *b);

/* 借用视图，直到下次修改/释放；空缓冲返回空 Slice。 */
cldb_slice_t cldb_buf_slice(const cldb_buf_t *buf);

/* 借用 C 字符串，永不为 NULL；
 * 含 '\0' 时只适合路径等场景。 */
const char *cldb_buf_cstr(const cldb_buf_t *buf);

#endif /* CLDB_SRC_UTIL_BUF_H_ */
