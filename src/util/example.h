#ifndef CLDB_SRC_UTIL_EXAMPLE_H_
#define CLDB_SRC_UTIL_EXAMPLE_H_

#include <stdint.h>

#include "cleveldb/status.h"

/* 不透明句柄；布局在 .c 中私有，调用者只能经下面的函数访问。
 * 上游同样把 DBImpl 隐藏在 leveldb::DB 指针之后。 */
typedef struct cldb_example cldb_example_t;

/* 创建对象；成功时 *out 拥有对象（对应上游 DB::Open 返回、调用者负责
 * delete），失败返回非 OK 且 *out = NULL。 */
cldb_status_t cldb_example_create(const char *name, cldb_example_t **out);

/* 销毁对象并释放其内部资源；对应上游 delete DB。
 * 接受 NULL；销毁后指针失效，不得重复销毁。 */
void cldb_example_destroy(cldb_example_t *example);

/* 借用内部 name，直到 set_name/destroy；对应上游只读属性访问。
 * 不加锁，调用者不得与 set_name 并发。 */
const char *cldb_example_name(cldb_example_t *example);

/* 复制入参后在锁内替换 name；对应上游用互斥量保护可变成员。
 * 失败时旧 name 保持不变。 */
cldb_status_t cldb_example_set_name(cldb_example_t *example,
                                    const char *name);

/* 在锁内完成读-改-写；对应上游 DBImpl::mutex_ 保护共享状态的模式。
 * 溢出返回 InvalidArgument，成功时可选写入 *out。 */
cldb_status_t cldb_example_add(cldb_example_t *example, int64_t delta,
                               int64_t *out);

/* 在锁内读取 value；对应上游受锁保护的元数据读取。out 必须非 NULL。 */
cldb_status_t cldb_example_get_value(cldb_example_t *example, int64_t *out);

#endif /* CLDB_SRC_UTIL_EXAMPLE_H_ */
