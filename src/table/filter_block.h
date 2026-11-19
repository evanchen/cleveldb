#ifndef CLDB_SRC_TABLE_FILTER_BLOCK_H_
#define CLDB_SRC_TABLE_FILTER_BLOCK_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cleveldb/buf.h"
#include "cleveldb/filter_policy.h"
#include "cleveldb/slice.h"
#include "cleveldb/status.h"

/* SSTable 过滤器块；对应上游 table/filter_block.h。构建器按 2 KB 文件区间
 * 把 key 分组交给 FilterPolicy，最终生成一个连续块：过滤器数据、每个过滤器
 * 的 fixed32 偏移数组、array_offset(fixed32)、base_lg(1 字节)。读取器从块
 * 末尾反向定位偏移数组，按数据块偏移判断 key 是否可能命中。策略和块内容
 * 都由调用者持有，builder/reader 只借用，必须活得比它们久。 */

/* 过滤器覆盖区间为 2^11 = 2 KB；上游固定值，不能随意修改，否则与上游
 * 生成的过滤器块不兼容。 */
#define CLDB_FILTER_BLOCK_BASE_LG 11
#define CLDB_FILTER_BLOCK_BASE \
  (UINT64_C(1) << CLDB_FILTER_BLOCK_BASE_LG)

/* 过滤器块构建器；调用顺序必须为
 *   (StartBlock AddKey*)* Finish
 * 单线程使用；key 仅在 AddKey 调用期间借用。Finish 后不得再调用其他方法。 */
typedef struct cldb_filter_block_builder {
  const cldb_filter_policy_t *policy; /* 仅引用；负责生成过滤器 */
  cldb_buf_t keys;                    /* 当前过滤器的扁平 key 内容 */
  size_t *starts;                     /* 每个 key 在 keys 中的起始偏移 */
  size_t starts_count;                /* keys 数量；GenerateFilter 后清零 */
  size_t starts_capacity;             /* starts 数组容量 */
  cldb_buf_t result;                  /* 已生成过滤器及最终偏移信息 */
  cldb_slice_t *tmp_keys;             /* 传给策略的临时 Slice 数组 */
  size_t tmp_keys_capacity;           /* tmp_keys 数组容量 */
  uint32_t *filter_offsets;           /* 每个过滤器的起始偏移 */
  size_t filter_offsets_count;        /* 已生成过滤器数 */
  size_t filter_offsets_capacity;     /* filter_offsets 数组容量 */
  bool finished;                      /* Finish 成功后为 true */
} cldb_filter_block_builder_t;

/* 初始化构建器；对应上游 FilterBlockBuilder 构造函数。policy 必须非 NULL
 * 且其 create_filter/key_may_match 可用；builder 必须尚未初始化。成功返回
 * OK；参数无效返回 InvalidArgument，此时 builder 已清零，可直接 destroy。 */
cldb_status_t cldb_filter_block_builder_init(
    cldb_filter_block_builder_t *builder,
    const cldb_filter_policy_t *policy);

/* 释放内部存储；对应上游析构函数。接受 NULL，之后不得再使用 builder。 */
void cldb_filter_block_builder_destroy(cldb_filter_block_builder_t *builder);

/* 标记一个数据块开始，block_offset 是该块在文件中的起始偏移；对应上游
 * StartBlock。偏移对应的过滤器序号必须不小于已生成数，否则返回
 * InvalidArgument；跳过区间会生成空过滤器（不调用策略）。分配失败返回
 * IOError，状态不变。Finish 后调用返回 InvalidArgument。 */
cldb_status_t cldb_filter_block_builder_start_block(
    cldb_filter_block_builder_t *builder, uint64_t block_offset);

/* 把 key 加入当前过滤器；对应上游 AddKey。空 key 合法，key 仅在调用期间
 * 借用。分配失败返回 IOError，构建器状态不变。Finish 后调用返回
 * InvalidArgument。 */
cldb_status_t cldb_filter_block_builder_add_key(
    cldb_filter_block_builder_t *builder, cldb_slice_t key);

/* 生成最后一个过滤器并追加偏移数组、array_offset 和 base_lg；对应上游
 * Finish。只能成功调用一次；成功时 *block 借用 builder 的 result，在
 * Destroy 前有效。参数无效或重复调用返回 InvalidArgument；分配失败或
 * 过滤器总长超过 fixed32 偏移范围返回 IOError，builder 仍可继续使用。 */
cldb_status_t cldb_filter_block_builder_finish(
    cldb_filter_block_builder_t *builder, cldb_slice_t *block);

/* 过滤器块读取器；对应上游 FilterBlockReader。contents 借用并在初始化时
 * 解析；格式损坏时保持禁用，所有查询保守返回 true。 */
typedef struct cldb_filter_block_reader {
  const cldb_filter_policy_t *policy; /* 仅引用；查询时调用 */
  const char *data;                   /* 过滤器数据起点；禁用时为 NULL */
  const char *offsets;                /* fixed32 偏移数组起点 */
  size_t data_size;                   /* 过滤器数据字节数（array_offset） */
  size_t num_offsets;                 /* 偏移数组元素数 */
  uint8_t base_lg;                    /* 过滤区间指数，正常为 11 */
} cldb_filter_block_reader_t;

/* 初始化读取器；对应上游 FilterBlockReader 构造函数。contents 只借用，
 * 在 reader 使用期间必须存活。块过短、array_offset 越界或 policy 无效时
 * reader 进入禁用状态，不返回错误；此时 key_may_match 返回 true。 */
void cldb_filter_block_reader_init(cldb_filter_block_reader_t *reader,
                                   const cldb_filter_policy_t *policy,
                                   cldb_slice_t contents);

/* 判断 key 是否可能位于 block_offset 开始的数据块；对应上游 KeyMayMatch。
 * false 表示一定不存在，true 表示可能存在或元数据损坏。index 超出偏移
 * 数组、偏移区间非法、base_lg 非法或 reader 被禁用时都返回 true，以避免
 * 假阴性；数据范围内的空过滤器交给策略判断（Bloom 返回 false）。key 长度
 * 非法时也保守返回 true。 */
bool cldb_filter_block_reader_key_may_match(
    const cldb_filter_block_reader_t *reader, uint64_t block_offset,
    cldb_slice_t key);

#endif /* CLDB_SRC_TABLE_FILTER_BLOCK_H_ */
