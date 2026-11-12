#include "db/skiplist.h"

#include <stddef.h>
#include <stdint.h>

/* 跳表实现；对应上游 db/skiplist.h。
 * 写者先初始化新节点及其全部 next 链，再用 release store 发布；读者用
 * acquire load 读取 next，因而看到链接时节点内容已可见。读无锁。 */

/* 节点存放在 arena 中；next 是弹性数组，长度由分配时的层高决定。
 * 链接一经发布就不再修改（只改指向它的 next），key 因此可长期只读。 */
struct cldb_skiplist_node {
  const char *key;           /* 头节点为 NULL；普通节点的 key 由调用者保活 */
  cldb_atomic_ptr_t next[];  /* 长度等于节点高度的原子链接数组 */
};

static cldb_skiplist_node_t *node_next(const cldb_skiplist_node_t *node,
                                       int level, memory_order order) {
  return cldb_atomic_ptr_load(&node->next[(size_t)level], order);
}

static void node_set_next(cldb_skiplist_node_t *node, int level,
                          cldb_skiplist_node_t *next, memory_order order) {
  cldb_atomic_ptr_store(&node->next[(size_t)level], next, order);
}

/* 层高只允许 [1, kMaxHeight]；返回 NULL 表示参数无效或分配失败。
 * key 可为 NULL（仅头节点使用）。 */
static cldb_skiplist_node_t *new_node(cldb_arena_t *arena, const char *key,
                                      int height) {
  cldb_skiplist_node_t *node;
  size_t bytes;
  int level;

  if (arena == NULL || height < 1 || height > CLDB_SKIPLIST_MAX_HEIGHT) {
    return NULL;
  }
  bytes = sizeof(*node) + sizeof(node->next[0]) * (size_t)height;
  node = (cldb_skiplist_node_t *)cldb_arena_allocate_aligned(arena, bytes);
  if (node == NULL) {
    return NULL;
  }
  node->key = key;
  for (level = 0; level < height; ++level) {
    cldb_atomic_ptr_init(&node->next[(size_t)level], NULL);
  }
  return node;
}

/* 返回第一个 >= key 的节点；list 为空或全部更小时返回 NULL。
 * prev 非 NULL 时按层写入该层最后一个 < key 的节点。 */
static cldb_skiplist_node_t *find_greater_or_equal(
    const cldb_skiplist_t *list, const char *key,
    cldb_skiplist_node_t **prev) {
  cldb_skiplist_node_t *x = list->head;
  int level = (int)cldb_atomic_u32_load(&list->max_height,
                                        memory_order_relaxed) -
              1;

  while (true) {
    cldb_skiplist_node_t *next =
        node_next(x, level, memory_order_acquire);
    if (next != NULL &&
        list->compare(list->compare_arg, next->key, key) < 0) {
      x = next;
    } else {
      if (prev != NULL) {
        prev[level] = x;
      }
      if (level == 0) {
        return next;
      }
      --level;
    }
  }
}

/* 返回最后一个 < key 的节点；可能返回头节点（表示不存在）。 */
static cldb_skiplist_node_t *find_less_than(const cldb_skiplist_t *list,
                                            const char *key) {
  cldb_skiplist_node_t *x = list->head;
  int level = (int)cldb_atomic_u32_load(&list->max_height,
                                        memory_order_relaxed) -
              1;

  while (true) {
    cldb_skiplist_node_t *next =
        node_next(x, level, memory_order_acquire);
    if (next != NULL &&
        list->compare(list->compare_arg, next->key, key) < 0) {
      x = next;
    } else {
      if (level == 0) {
        return x;
      }
      --level;
    }
  }
}

/* 返回最后一个节点；空表返回头节点。 */
static cldb_skiplist_node_t *find_last(const cldb_skiplist_t *list) {
  cldb_skiplist_node_t *x = list->head;
  int level = (int)cldb_atomic_u32_load(&list->max_height,
                                        memory_order_relaxed) -
              1;

  while (true) {
    cldb_skiplist_node_t *next =
        node_next(x, level, memory_order_acquire);
    if (next != NULL) {
      x = next;
    } else {
      if (level == 0) {
        return x;
      }
      --level;
    }
  }
}

bool cldb_skiplist_init(cldb_skiplist_t *list, cldb_arena_t *arena,
                        cldb_skiplist_compare_fn_t compare,
                        void *compare_arg) {
  if (list == NULL || arena == NULL || compare == NULL) {
    return false;
  }
  list->compare = compare;
  list->compare_arg = compare_arg;
  list->arena = arena;
  cldb_atomic_u32_init(&list->max_height, 1);
  cldb_random_init(&list->rnd, UINT32_C(0xdeadbeef));
  list->head = new_node(arena, NULL, CLDB_SKIPLIST_MAX_HEIGHT);
  return list->head != NULL;
}

int cldb_skiplist_random_height(cldb_random_t *rnd) {
  int height = 1;

  while (height < CLDB_SKIPLIST_MAX_HEIGHT &&
         cldb_random_one_in(rnd, 4)) {
    ++height;
  }
  return height;
}

bool cldb_skiplist_insert(cldb_skiplist_t *list, const char *key) {
  cldb_skiplist_node_t *prev[CLDB_SKIPLIST_MAX_HEIGHT];
  cldb_skiplist_node_t *next;
  cldb_skiplist_node_t *node;
  uint32_t current_height;
  int height;
  int level;

  if (list == NULL || list->compare == NULL || list->head == NULL ||
      key == NULL) {
    return false;
  }
  next = find_greater_or_equal(list, key, prev);
  if (next != NULL &&
      list->compare(list->compare_arg, next->key, key) == 0) {
    return false;
  }

  height = cldb_skiplist_random_height(&list->rnd);
  node = new_node(list->arena, key, height);
  if (node == NULL) {
    return false;
  }

  current_height =
      cldb_atomic_u32_load(&list->max_height, memory_order_relaxed);
  if ((uint32_t)height > current_height) {
    for (level = (int)current_height; level < height; ++level) {
      prev[level] = list->head;
    }
    /* 升高层高不发布节点数据，relaxed 足够；各层链接随后 release 发布。 */
    cldb_atomic_u32_store(&list->max_height, (uint32_t)height,
                          memory_order_relaxed);
  }

  for (level = 0; level < height; ++level) {
    /* 本线程刚初始化新节点，读取旧链接无需 acquire。 */
    node_set_next(node, level,
                  node_next(prev[level], level, memory_order_relaxed),
                  memory_order_relaxed);
    /* 只有此层预链接完成后，其他线程才能通过 prev[level] 看到 node。 */
    node_set_next(prev[level], level, node, memory_order_release);
  }
  return true;
}

bool cldb_skiplist_contains(const cldb_skiplist_t *list, const char *key) {
  cldb_skiplist_node_t *node;

  if (list == NULL || list->compare == NULL || list->head == NULL ||
      key == NULL) {
    return false;
  }
  node = find_greater_or_equal(list, key, NULL);
  return node != NULL &&
         list->compare(list->compare_arg, node->key, key) == 0;
}

int cldb_skiplist_max_height(const cldb_skiplist_t *list) {
  if (list == NULL) {
    return 0;
  }
  return (int)cldb_atomic_u32_load(&list->max_height, memory_order_relaxed);
}

void cldb_skiplist_iterator_init(cldb_skiplist_iterator_t *iter,
                                 const cldb_skiplist_t *list) {
  if (iter == NULL) {
    return;
  }
  iter->list = list;
  iter->node = NULL;
}

bool cldb_skiplist_iterator_valid(const cldb_skiplist_iterator_t *iter) {
  return iter != NULL && iter->node != NULL;
}

const char *cldb_skiplist_iterator_key(
    const cldb_skiplist_iterator_t *iter) {
  if (!cldb_skiplist_iterator_valid(iter)) {
    return NULL;
  }
  return iter->node->key;
}

void cldb_skiplist_iterator_next(cldb_skiplist_iterator_t *iter) {
  if (!cldb_skiplist_iterator_valid(iter)) {
    return;
  }
  iter->node = node_next(iter->node, 0, memory_order_acquire);
}

void cldb_skiplist_iterator_prev(cldb_skiplist_iterator_t *iter) {
  cldb_skiplist_node_t *node;

  if (!cldb_skiplist_iterator_valid(iter) || iter->list == NULL ||
      iter->node->key == NULL) {
    return;
  }
  node = find_less_than(iter->list, iter->node->key);
  iter->node = node == iter->list->head ? NULL : node;
}

void cldb_skiplist_iterator_seek(cldb_skiplist_iterator_t *iter,
                                 const char *target) {
  if (iter == NULL) {
    return;
  }
  if (iter->list == NULL || target == NULL) {
    iter->node = NULL;
    return;
  }
  iter->node = find_greater_or_equal(iter->list, target, NULL);
}

void cldb_skiplist_iterator_seek_to_first(cldb_skiplist_iterator_t *iter) {
  if (iter == NULL) {
    return;
  }
  if (iter->list == NULL) {
    iter->node = NULL;
    return;
  }
  iter->node = node_next(iter->list->head, 0, memory_order_acquire);
}

void cldb_skiplist_iterator_seek_to_last(cldb_skiplist_iterator_t *iter) {
  cldb_skiplist_node_t *node;

  if (iter == NULL) {
    return;
  }
  if (iter->list == NULL) {
    iter->node = NULL;
    return;
  }
  node = find_last(iter->list);
  iter->node = node == iter->list->head ? NULL : node;
}
