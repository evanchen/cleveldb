#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "db/dbformat.h"
#include "db/snapshot.h"
#include "harness.h"

/* 快照链表行为测试；对应上游 db/snapshot.h 的语义。链表结构为内部类型，
 * 测试直接检查 prev/next 以验证摘链后仍保持循环双向链表。 */

/* 核对链表从最老到最新的顺序，并检查双向/首尾链接；count 为 0 时验证
 * 空链哨兵自环。 */
static void expect_list_order(const cldb_snapshot_list_t *list,
                              const cldb_sequence_number_t *sequences,
                              size_t count) {
  const cldb_snapshot_t *node;
  size_t i;

  node = &list->head;
  for (i = 0; i < count; ++i) {
    node = node->next;
    CLDB_EXPECT_TRUE(node != &list->head);
    CLDB_EXPECT_EQ_PTR(node, node->next->prev);
    CLDB_EXPECT_EQ_UINT(sequences[i], cldb_snapshot_sequence(node));
  }
  CLDB_EXPECT_EQ_PTR(&list->head, node->next);
  CLDB_EXPECT_EQ_PTR(node, list->head.prev);

  node = &list->head;
  for (i = count; i > 0; --i) {
    node = node->prev;
    CLDB_EXPECT_TRUE(node != &list->head);
    CLDB_EXPECT_EQ_PTR(node, node->prev->next);
    CLDB_EXPECT_EQ_UINT(sequences[i - 1U], cldb_snapshot_sequence(node));
  }
  CLDB_EXPECT_EQ_PTR(&list->head, node->prev);
  CLDB_EXPECT_EQ_PTR(node, list->head.next);
}

CLDB_TEST(snapshot_list_new_oldest_and_delete) {
  cldb_snapshot_list_t list;
  cldb_snapshot_t *snapshots[3];
  const cldb_sequence_number_t all_sequences[] = {10, 20, 30};
  const cldb_sequence_number_t later_sequences[] = {20, 30};
  const cldb_sequence_number_t last_sequences[] = {20};

  cldb_snapshot_list_init(&list);
  CLDB_EXPECT_TRUE(cldb_snapshot_list_empty(&list));
  CLDB_EXPECT_EQ_PTR(NULL, cldb_snapshot_list_oldest(&list));
  CLDB_EXPECT_EQ_PTR(NULL, cldb_snapshot_list_newest(&list));
  expect_list_order(&list, NULL, 0);

  snapshots[0] = cldb_snapshot_list_new(&list, 10);
  snapshots[1] = cldb_snapshot_list_new(&list, 20);
  snapshots[2] = cldb_snapshot_list_new(&list, 30);
  CLDB_ASSERT_TRUE(snapshots[0] != NULL);
  CLDB_ASSERT_TRUE(snapshots[1] != NULL);
  CLDB_ASSERT_TRUE(snapshots[2] != NULL);
  CLDB_EXPECT_FALSE(cldb_snapshot_list_empty(&list));
  CLDB_EXPECT_EQ_PTR(snapshots[0], cldb_snapshot_list_oldest(&list));
  CLDB_EXPECT_EQ_PTR(snapshots[2], cldb_snapshot_list_newest(&list));
  CLDB_EXPECT_EQ_UINT(10, cldb_snapshot_sequence(snapshots[0]));
  CLDB_EXPECT_EQ_UINT(20, cldb_snapshot_sequence(snapshots[1]));
  CLDB_EXPECT_EQ_UINT(30, cldb_snapshot_sequence(snapshots[2]));
  expect_list_order(&list, all_sequences, 3);

  cldb_snapshot_list_delete(&list, snapshots[0]);
  snapshots[0] = NULL;
  CLDB_EXPECT_EQ_PTR(snapshots[1], cldb_snapshot_list_oldest(&list));
  CLDB_EXPECT_EQ_PTR(snapshots[2], cldb_snapshot_list_newest(&list));
  expect_list_order(&list, later_sequences, 2);

  cldb_snapshot_list_delete(&list, snapshots[2]);
  snapshots[2] = NULL;
  CLDB_EXPECT_EQ_PTR(snapshots[1], cldb_snapshot_list_oldest(&list));
  CLDB_EXPECT_EQ_PTR(snapshots[1], cldb_snapshot_list_newest(&list));
  expect_list_order(&list, last_sequences, 1);

  cldb_snapshot_list_delete(&list, snapshots[1]);
  snapshots[1] = NULL;
  CLDB_EXPECT_TRUE(cldb_snapshot_list_empty(&list));
  CLDB_EXPECT_EQ_PTR(NULL, cldb_snapshot_list_oldest(&list));
  CLDB_EXPECT_EQ_PTR(NULL, cldb_snapshot_list_newest(&list));
  expect_list_order(&list, NULL, 0);
}

CLDB_TEST(snapshot_list_out_of_order_delete) {
  enum { kSnapshotCount = 8 };
  static const size_t kDeleteOrder[kSnapshotCount] = {3, 7, 0, 5,
                                                      2, 6, 1, 4};
  cldb_snapshot_list_t list;
  cldb_snapshot_t *snapshots[kSnapshotCount];
  bool present[kSnapshotCount];
  cldb_sequence_number_t expected[kSnapshotCount];
  size_t step;
  size_t i;

  cldb_snapshot_list_init(&list);
  for (i = 0; i < (size_t)kSnapshotCount; ++i) {
    snapshots[i] =
        cldb_snapshot_list_new(&list, (cldb_sequence_number_t)i * 10U);
    CLDB_ASSERT_TRUE(snapshots[i] != NULL);
    present[i] = true;
  }
  for (i = 0; i < (size_t)kSnapshotCount; ++i) {
    expected[i] = (cldb_sequence_number_t)i * 10U;
  }
  expect_list_order(&list, expected, kSnapshotCount);

  for (step = 0; step < (size_t)kSnapshotCount; ++step) {
    size_t removed = kDeleteOrder[step];
    size_t first = 0;
    size_t last = 0;
    size_t remaining = 0;

    cldb_snapshot_list_delete(&list, snapshots[removed]);
    snapshots[removed] = NULL;
    present[removed] = false;

    for (i = 0; i < (size_t)kSnapshotCount; ++i) {
      if (present[i]) {
        expected[remaining++] = (cldb_sequence_number_t)i * 10U;
      }
    }
    expect_list_order(&list, expected, remaining);
    if (remaining == 0) {
      CLDB_EXPECT_TRUE(cldb_snapshot_list_empty(&list));
      CLDB_EXPECT_EQ_PTR(NULL, cldb_snapshot_list_oldest(&list));
      CLDB_EXPECT_EQ_PTR(NULL, cldb_snapshot_list_newest(&list));
      continue;
    }

    CLDB_EXPECT_FALSE(cldb_snapshot_list_empty(&list));
    while (!present[first]) {
      ++first;
    }
    last = (size_t)kSnapshotCount - 1U;
    while (!present[last]) {
      --last;
    }
    CLDB_EXPECT_EQ_PTR(snapshots[first], cldb_snapshot_list_oldest(&list));
    CLDB_EXPECT_EQ_PTR(snapshots[last], cldb_snapshot_list_newest(&list));
    CLDB_EXPECT_EQ_UINT(expected[0],
                        cldb_snapshot_sequence(
                            cldb_snapshot_list_oldest(&list)));
    CLDB_EXPECT_EQ_UINT(expected[remaining - 1U],
                        cldb_snapshot_sequence(
                            cldb_snapshot_list_newest(&list)));
  }
}

CLDB_TEST(snapshot_list_null_and_limits) {
  cldb_snapshot_list_t list;
  cldb_snapshot_t *first;
  cldb_snapshot_t *second;
  cldb_snapshot_t *latest;

  cldb_snapshot_list_init(NULL);
  CLDB_EXPECT_TRUE(cldb_snapshot_list_empty(NULL));
  CLDB_EXPECT_EQ_PTR(NULL, cldb_snapshot_list_oldest(NULL));
  CLDB_EXPECT_EQ_PTR(NULL, cldb_snapshot_list_newest(NULL));
  CLDB_EXPECT_EQ_UINT(0, cldb_snapshot_sequence(NULL));
  CLDB_EXPECT_EQ_PTR(NULL, cldb_snapshot_list_new(NULL, 10));

  cldb_snapshot_list_init(&list);
  CLDB_EXPECT_EQ_PTR(
      NULL, cldb_snapshot_list_new(&list, CLDB_MAX_SEQUENCE_NUMBER + 1U));
  /* 同一 sequence 可以创建多个快照；新快照仍追加在尾部。 */
  first = cldb_snapshot_list_new(&list, 0);
  second = cldb_snapshot_list_new(&list, 0);
  latest = cldb_snapshot_list_new(&list, CLDB_MAX_SEQUENCE_NUMBER);
  CLDB_ASSERT_TRUE(first != NULL);
  CLDB_ASSERT_TRUE(second != NULL);
  CLDB_ASSERT_TRUE(latest != NULL);
  CLDB_EXPECT_EQ_PTR(first, cldb_snapshot_list_oldest(&list));
  CLDB_EXPECT_EQ_PTR(latest, cldb_snapshot_list_newest(&list));
  CLDB_EXPECT_EQ_UINT(0, cldb_snapshot_sequence(first));
  CLDB_EXPECT_EQ_UINT(0, cldb_snapshot_sequence(second));
  CLDB_EXPECT_EQ_UINT(CLDB_MAX_SEQUENCE_NUMBER,
                      cldb_snapshot_sequence(latest));

  /* NULL 快照/链表不改变链表。 */
  cldb_snapshot_list_delete(NULL, first);
  CLDB_EXPECT_FALSE(cldb_snapshot_list_empty(&list));
  CLDB_EXPECT_EQ_PTR(first, cldb_snapshot_list_oldest(&list));
  cldb_snapshot_list_delete(&list, NULL);
  CLDB_EXPECT_FALSE(cldb_snapshot_list_empty(&list));
  CLDB_EXPECT_EQ_PTR(first, cldb_snapshot_list_oldest(&list));

  cldb_snapshot_list_delete(&list, second);
  cldb_snapshot_list_delete(&list, first);
  cldb_snapshot_list_delete(&list, latest);
  CLDB_EXPECT_TRUE(cldb_snapshot_list_empty(&list));
}

void cldb_register_snapshot_tests(void) {
  CLDB_RUN_TEST(snapshot_list_new_oldest_and_delete);
  CLDB_RUN_TEST(snapshot_list_out_of_order_delete);
  CLDB_RUN_TEST(snapshot_list_null_and_limits);
}
