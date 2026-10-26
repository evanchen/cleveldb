#include "cleveldb/iterator.h"

#include <stdlib.h>

#include "port/port.h"

/* 对应上游 table/iterator.cc：Iterator 基类的 cleanup 链表、EmptyIterator，
 * 以及 NewEmptyIterator/NewErrorIterator。C 版用 vtable 替代虚函数，用
 * 结构体首成员嵌入实现继承。 */

struct cldb_iter_cleanup {
  cldb_iter_cleanup_fn_t function;
  void *arg1;
  void *arg2;
  cldb_iter_cleanup_t *next;
};

void cldb_iter_init(cldb_iterator_t *iterator,
                    const cldb_iterator_vtbl_t *vtbl) {
  if (iterator == NULL) {
    return;
  }
  CLDB_ASSERT(vtbl != NULL);
  iterator->vtbl = vtbl;
  iterator->cleanup_head = NULL;
  iterator->destroyed = false;
}

cldb_status_t cldb_iter_register_cleanup(cldb_iterator_t *iterator,
                                         cldb_iter_cleanup_fn_t function,
                                         void *arg1, void *arg2) {
  cldb_iter_cleanup_t *node;

  if (iterator == NULL) {
    return cldb_status_invalid_argument("cldb_iter_register_cleanup",
                                        "iterator is NULL");
  }
  if (function == NULL) {
    return cldb_status_invalid_argument("cldb_iter_register_cleanup",
                                        "function is NULL");
  }
  if (iterator->vtbl == NULL) {
    return cldb_status_invalid_argument("cldb_iter_register_cleanup",
                                        "iterator is not initialized");
  }
  CLDB_ASSERT(!iterator->destroyed);
  if (iterator->destroyed) {
    return cldb_status_invalid_argument("cldb_iter_register_cleanup",
                                        "iterator is destroyed");
  }

  node = malloc(sizeof(*node));
  if (node == NULL) {
    return cldb_status_io("cldb_iter_register_cleanup", "out of memory");
  }
  node->function = function;
  node->arg1 = arg1;
  node->arg2 = arg2;

  if (iterator->cleanup_head == NULL) {
    node->next = NULL;
    iterator->cleanup_head = node;
  } else {
    /* 上游把新节点插在头节点之后；因此首个回调后是注册逆序。 */
    node->next = iterator->cleanup_head->next;
    iterator->cleanup_head->next = node;
  }
  return cldb_status_ok();
}

void cldb_iter_run_cleanup(cldb_iterator_t *iterator) {
  cldb_iter_cleanup_t *head;
  cldb_iter_cleanup_t *node;
  cldb_iter_cleanup_t *next;

  if (iterator == NULL) {
    return;
  }
  head = iterator->cleanup_head;
  if (head == NULL) {
    return;
  }
  iterator->cleanup_head = NULL;

  /* 上游先执行内联头节点，再沿 next 遍历；节点在回调后可释放。 */
  head->function(head->arg1, head->arg2);
  node = head->next;
  free(head);
  while (node != NULL) {
    next = node->next;
    node->function(node->arg1, node->arg2);
    free(node);
    node = next;
  }
}

void cldb_iter_destroy(cldb_iterator_t *iterator) {
  if (iterator == NULL) {
    return;
  }
  CLDB_ASSERT(iterator->vtbl != NULL);
  CLDB_ASSERT(!iterator->destroyed);
  if (iterator->destroyed) {
    return;
  }
  iterator->destroyed = true;
  iterator->vtbl->destroy(iterator);
}

typedef struct cldb_empty_iterator {
  cldb_iterator_t base; /* 必须为首成员，回调可转回外层类型 */
  cldb_status_t status; /* 按值保存；错误迭代器始终返回该状态 */
} cldb_empty_iterator_t;

static bool empty_valid(const cldb_iterator_t *iterator) {
  (void)iterator;
  return false;
}

static void empty_seek_to_first(cldb_iterator_t *iterator) { (void)iterator; }

static void empty_seek_to_last(cldb_iterator_t *iterator) { (void)iterator; }

static void empty_seek(cldb_iterator_t *iterator, cldb_slice_t target) {
  (void)iterator;
  (void)target;
}

/* 上游在这两个方法中断言失败；release 下保持空操作。 */
static void empty_next(cldb_iterator_t *iterator) {
  (void)iterator;
  CLDB_ASSERT(false);
}

static void empty_prev(cldb_iterator_t *iterator) {
  (void)iterator;
  CLDB_ASSERT(false);
}

static cldb_slice_t empty_key(const cldb_iterator_t *iterator) {
  (void)iterator;
  CLDB_ASSERT(false);
  return cldb_slice_empty();
}

static cldb_slice_t empty_value(const cldb_iterator_t *iterator) {
  (void)iterator;
  CLDB_ASSERT(false);
  return cldb_slice_empty();
}

static cldb_status_t empty_status(const cldb_iterator_t *iterator) {
  const cldb_empty_iterator_t *self =
      (const cldb_empty_iterator_t *)iterator;

  return self->status;
}

/* 空迭代器没有具体资源；先执行基类 cleanup，再释放自身，对应上游派生类
 * 析构完成后执行 Iterator::~Iterator 的顺序。 */
static void empty_destroy(cldb_iterator_t *iterator) {
  cldb_empty_iterator_t *self = (cldb_empty_iterator_t *)iterator;

  cldb_iter_run_cleanup(iterator);
  free(self);
}

static const cldb_iterator_vtbl_t kEmptyVtbl = {
    .valid = empty_valid,
    .seek_to_first = empty_seek_to_first,
    .seek_to_last = empty_seek_to_last,
    .seek = empty_seek,
    .next = empty_next,
    .prev = empty_prev,
    .key = empty_key,
    .value = empty_value,
    .status = empty_status,
    .destroy = empty_destroy,
};

static cldb_iterator_t *new_empty_iterator(cldb_status_t status) {
  cldb_empty_iterator_t *iterator = malloc(sizeof(*iterator));

  if (iterator == NULL) {
    return NULL;
  }
  cldb_iter_init(&iterator->base, &kEmptyVtbl);
  iterator->status = status;
  return &iterator->base;
}

cldb_iterator_t *cldb_new_empty_iterator(void) {
  return new_empty_iterator(cldb_status_ok());
}

cldb_iterator_t *cldb_new_error_iterator(cldb_status_t status) {
  return new_empty_iterator(status);
}
