#include "db/snapshot.h"

#include <stdlib.h>

/* 快照链表实现；对应上游 db/snapshot.h。哨兵 head 内嵌在链表对象中，
 * 因此初始化不需要分配。快照节点由链表创建并在 delete 时释放。 */

void cldb_snapshot_list_init(cldb_snapshot_list_t *list) {
  if (list == NULL) {
    return;
  }
  list->head.sequence = 0;
  list->head.prev = &list->head;
  list->head.next = &list->head;
#if CLDB_DEBUG
  list->head.list = list;
#endif
}

bool cldb_snapshot_list_empty(const cldb_snapshot_list_t *list) {
  return list == NULL || list->head.next == &list->head;
}

cldb_snapshot_t *cldb_snapshot_list_new(cldb_snapshot_list_t *list,
                                        cldb_sequence_number_t sequence) {
  cldb_snapshot_t *snapshot;

  if (list == NULL || sequence > CLDB_MAX_SEQUENCE_NUMBER) {
    return NULL;
  }
  /* 快照按 sequence 递增创建；违反此约定会破坏 oldest/newest 语义。 */
  CLDB_ASSERT(cldb_snapshot_list_empty(list) ||
              cldb_snapshot_sequence(cldb_snapshot_list_newest(list)) <=
                  sequence);

  snapshot = malloc(sizeof(*snapshot));
  if (snapshot == NULL) {
    return NULL;
  }
  snapshot->sequence = sequence;
  snapshot->prev = list->head.prev;
  snapshot->next = &list->head;
  snapshot->prev->next = snapshot;
  snapshot->next->prev = snapshot;
#if CLDB_DEBUG
  snapshot->list = list;
#endif
  return snapshot;
}

void cldb_snapshot_list_delete(cldb_snapshot_list_t *list,
                               const cldb_snapshot_t *snapshot) {
  cldb_snapshot_t *mutable_snapshot;

  if (list == NULL || snapshot == NULL) {
    return;
  }
  /* 快照必须来自本链表，否则摘链会破坏另一条链表；重复删除会访问已释放
   * 内存，调用者必须保证恰好删除一次。 */
  CLDB_ASSERT(snapshot != &list->head);
#if CLDB_DEBUG
  CLDB_ASSERT(snapshot->list == list);
#endif
  mutable_snapshot = (cldb_snapshot_t *)(void *)snapshot;
  mutable_snapshot->prev->next = mutable_snapshot->next;
  mutable_snapshot->next->prev = mutable_snapshot->prev;
  free(mutable_snapshot);
}

const cldb_snapshot_t *cldb_snapshot_list_oldest(
    const cldb_snapshot_list_t *list) {
  if (cldb_snapshot_list_empty(list)) {
    return NULL;
  }
  return list->head.next;
}

const cldb_snapshot_t *cldb_snapshot_list_newest(
    const cldb_snapshot_list_t *list) {
  if (cldb_snapshot_list_empty(list)) {
    return NULL;
  }
  return list->head.prev;
}

cldb_sequence_number_t cldb_snapshot_sequence(
    const cldb_snapshot_t *snapshot) {
  return snapshot != NULL ? snapshot->sequence : 0;
}
