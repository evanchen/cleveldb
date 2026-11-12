#ifndef CLDB_SRC_DB_SKIPLIST_H_
#define CLDB_SRC_DB_SKIPLIST_H_

#include <stdbool.h>

#include "port/port_atomic.h"
#include "util/arena.h"
#include "util/random.h"

/* 无锁读、单写者跳表；对应上游 db/skiplist.h。读路径不加锁，但调用者
 * 必须保证读取期间 list 与 arena 不被销毁；写只能由单个线程执行，并由
 * 外部互斥量串行化。节点存入 arena 后永不删除，key 在链接后不可变。 */

enum { CLDB_SKIPLIST_MAX_HEIGHT = 12 };

typedef struct cldb_skiplist_node cldb_skiplist_node_t;

/* 比较 a、b，返回负/零/正；arg 为调用者上下文，仅引用。 */
typedef int (*cldb_skiplist_compare_fn_t)(void *arg, const char *a,
                                          const char *b);

typedef struct cldb_skiplist {
  cldb_skiplist_compare_fn_t compare; /* 必须非 NULL */
  void *compare_arg;                  /* 仅引用 */
  cldb_arena_t *arena;                /* 仅引用；必须比 list 活得久 */
  cldb_skiplist_node_t *head;         /* 最高层哨兵；其 key 不参与比较 */
  cldb_atomic_u32_t max_height;       /* 当前层高，写者更新、读者 relaxed 读取 */
  cldb_random_t rnd;                  /* 仅写者使用 */
} cldb_skiplist_t;

/* 初始化空表；只用 arena 分配头节点。参数无效或分配失败返回 false，
 * 失败后 list 不可用。不提供 destroy：所有节点由 arena 统一释放，
 * list 必须在 arena 销毁前停止使用。 */
bool cldb_skiplist_init(cldb_skiplist_t *list, cldb_arena_t *arena,
                        cldb_skiplist_compare_fn_t compare,
                        void *compare_arg);

/* 生成 [1, kMaxHeight] 的随机层高，每升一层概率约 1/4；对应上游
 * RandomHeight。会消耗 rnd 状态，rnd 为 NULL 时返回 1。 */
int cldb_skiplist_random_height(cldb_random_t *rnd);

/* 插入 key；调用者保证没有相等键。成功返回 true。相等键或 arena 分配
 * 失败返回 false，list 保持不变。key 内容由调用者保活，直到 arena 销毁；
 * key 不得在链接后修改。写操作必须由外部串行化。 */
bool cldb_skiplist_insert(cldb_skiplist_t *list, const char *key);

/* 是否存在比较相等的键；对应上游 Contains。可与插入并发调用。 */
bool cldb_skiplist_contains(const cldb_skiplist_t *list, const char *key);

/* 当前最大层高；无效对象返回 0。relaxed 读取，可能看到稍旧的值。 */
int cldb_skiplist_max_height(const cldb_skiplist_t *list);

/* 正向迭代器；按值复制后各副本独立。node 为 NULL 表示无效。 */
typedef struct cldb_skiplist_iterator {
  const cldb_skiplist_t *list; /* 仅引用 */
  cldb_skiplist_node_t *node;  /* 当前节点；NULL 表示越界 */
} cldb_skiplist_iterator_t;

/* 初始化为无效迭代器；list 为 NULL 时同样无效。 */
void cldb_skiplist_iterator_init(cldb_skiplist_iterator_t *iter,
                                 const cldb_skiplist_t *list);

/* 是否停在有效节点；对应上游 Valid。 */
bool cldb_skiplist_iterator_valid(const cldb_skiplist_iterator_t *iter);

/* 当前 key；仅引用，生命周期同 list/arena。无效迭代器返回 NULL。 */
const char *cldb_skiplist_iterator_key(
    const cldb_skiplist_iterator_t *iter);

/* 前进/后退一步；无效迭代器上不做事。Prev 不保存 prev 指针，而是从
 * 头节点重新搜索，代价 O(log n)。 */
void cldb_skiplist_iterator_next(cldb_skiplist_iterator_t *iter);
void cldb_skiplist_iterator_prev(cldb_skiplist_iterator_t *iter);

/* 定位到第一个 >= target 的节点；target 为 NULL 时迭代器变为无效。 */
void cldb_skiplist_iterator_seek(cldb_skiplist_iterator_t *iter,
                                 const char *target);

/* 定位到首/尾；空表时变为无效。 */
void cldb_skiplist_iterator_seek_to_first(cldb_skiplist_iterator_t *iter);
void cldb_skiplist_iterator_seek_to_last(cldb_skiplist_iterator_t *iter);

#endif /* CLDB_SRC_DB_SKIPLIST_H_ */
