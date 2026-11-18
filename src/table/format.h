#ifndef CLDB_SRC_TABLE_FORMAT_H_
#define CLDB_SRC_TABLE_FORMAT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cleveldb/buf.h"
#include "cleveldb/env.h"
#include "cleveldb/options.h"
#include "cleveldb/slice.h"
#include "cleveldb/status.h"

/* SSTable 元数据与块读取；对应上游 table/format.h。文件尾部固定 48 字节
 * footer：两个 handle 顺序编码后共同补零到 40 字节（各最多 20），再写
 * 8 字节 magic，使变长 handle 也占定长空间。每个块末尾另有 5 字节
 * trailer：| type(1) | masked crc32c(4, 小端) |，CRC 覆盖块数据与 type。 */

#define CLDB_BLOCK_HANDLE_MAX_ENCODED_LENGTH 20
#define CLDB_TABLE_MAGIC_SIZE 8
#define CLDB_FOOTER_ENCODED_LENGTH \
  (2 * CLDB_BLOCK_HANDLE_MAX_ENCODED_LENGTH + CLDB_TABLE_MAGIC_SIZE)

/* 块 trailer 布局；type 表示编码方式，CRC 从 type 开始覆盖。 */
#define CLDB_BLOCK_TRAILER_TYPE_OFFSET 0
#define CLDB_BLOCK_TRAILER_TYPE_SIZE 1
#define CLDB_BLOCK_TRAILER_CRC_OFFSET \
  (CLDB_BLOCK_TRAILER_TYPE_OFFSET + CLDB_BLOCK_TRAILER_TYPE_SIZE)
#define CLDB_BLOCK_TRAILER_CRC_SIZE 4
#define CLDB_BLOCK_TRAILER_SIZE \
  (CLDB_BLOCK_TRAILER_TYPE_SIZE + CLDB_BLOCK_TRAILER_CRC_SIZE)

/* 表文件标识；上游对 `echo http://code.google.com/p/leveldb/ | sha1sum`
 * 的结果取前 64 位（0xdb4775248b80fb57），落盘为小端 fixed64。 */
#define CLDB_TABLE_MAGIC_NUMBER UINT64_C(0xdb4775248b80fb57)

/* 块位置；对应上游 BlockHandle。offset 从文件起点计，size 只含块内容，
 * 不含 5 字节 trailer。字段由调用者直接设置。 */
typedef struct cldb_block_handle {
  uint64_t offset; /* 块数据起始偏移 */
  uint64_t size;   /* 落盘块字节数；压缩块为压缩后大小 */
} cldb_block_handle_t;

/* 表尾元数据；对应上游 Footer。两个 handle 必须属于同一文件。 */
typedef struct cldb_footer {
  cldb_block_handle_t metaindex_handle; /* metaindex 块位置 */
  cldb_block_handle_t index_handle;     /* index 块位置 */
} cldb_footer_t;

/* 块字节及所有权提示；对应上游 BlockContents。读取成功后 cachable 仅作
 * 缓存提示；heap_allocated 为真时由 Block 或调用者 free。 */
typedef struct cldb_block_contents {
  cldb_slice_t data; /* 块内容，不含 trailer */
  bool cachable;     /* 数据可放入 block cache */
  bool heap_allocated; /* data 来自堆分配，需释放 */
} cldb_block_contents_t;

/* 把 handle 的两个 varint64 追加到 dst；返回 false 表示 dst 为 NULL 或
 * 追加失败。最大占 20 字节，与上游 BlockHandle::EncodeTo 字节一致。 */
bool cldb_block_handle_encode(const cldb_block_handle_t *handle,
                              cldb_buf_t *dst);

/* 从 input 头部解析 offset、size 并前进；对应上游 DecodeFrom。成功才
 * 提交 handle 和 input，失败返回 Corruption 且两者不变。 */
cldb_status_t cldb_block_handle_decode(cldb_slice_t *input,
                                      cldb_block_handle_t *handle);

/* 把 footer 追加到 dst，固定占 48 字节；对应上游 Footer::EncodeTo。
 * 返回 false 表示 dst 为 NULL 或追加失败。 */
bool cldb_footer_encode(const cldb_footer_t *footer, cldb_buf_t *dst);

/* 从 input 头部解析 48 字节 footer；对应上游 DecodeFrom。成功时写入
 * footer 并让 input 前进 48 字节，保留其后内容；失败时两者均不变。
 * 过短、magic 错误或 handle 损坏返回 Corruption，成功返回 OK。 */
cldb_status_t cldb_footer_decode(cldb_slice_t *input, cldb_footer_t *footer);

/* 读取 handle 指向的块及 trailer；对应上游 ReadBlock。verify_checksums
 * 为真时校验覆盖块数据与 type 的 CRC。成功时填充 result：文件实现通过
 * mmap 或自有缓冲返回数据时直接借用、不可缓存；来自 scratch 或解压缓冲
 * 时 heap_allocated 和 cachable 为真，由 Block/调用者负责释放。失败时
 * result 为空且不拥有数据。压缩类型在未启用对应后端时返回 Corruption；
 * 未知 type 亦然。借用结果必须保持 file 存活；options 不能为 NULL。 */
cldb_status_t cldb_read_block(cldb_random_access_file_t *file,
                              const cldb_read_options_t *options,
                              cldb_block_handle_t handle,
                              cldb_block_contents_t *result);

#endif /* CLDB_SRC_TABLE_FORMAT_H_ */
