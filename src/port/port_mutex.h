#ifndef CLDB_SRC_PORT_PORT_MUTEX_H_
#define CLDB_SRC_PORT_PORT_MUTEX_H_

#include <pthread.h>
#include <stdbool.h>

#include "cleveldb/status.h"
#include "port/port.h"

/* pthread 互斥量与条件变量薄封装；对应上游 port::Mutex/port::CondVar。
 * 与上游 void 接口不同，这里把 pthread 错误映射为状态：调用参数或
 * 生命周期错误返回 InvalidArgument，系统资源错误返回 IOError。
 * 默认 mutex 非递归，同一线程重复加锁会死锁。
 * 调用者显式 lock/unlock；持锁期间每个返回路径必须先 unlock，多项
 * 资源统一 goto cleanup，对应上游 MutexLock 析构时的自动解锁。 */

typedef struct cldb_mutex {
  pthread_mutex_t mutex; /* owned；pthread 默认属性，非递归 */
  bool initialized;      /* init 成功且尚未 destroy；仅用于拒绝误用 */
} cldb_mutex_t;

/* 条件变量；对应上游 port::CondVar。mutex 为 borrowed，其生命周期
 * 必须覆盖条件变量；Wait 要求调用者已持有该 mutex。 */
typedef struct cldb_condvar {
  pthread_cond_t cond; /* owned；pthread 条件变量 */
  cldb_mutex_t *mutex; /* borrowed；绑定的互斥量 */
  bool initialized;    /* init 成功且尚未 destroy；仅用于拒绝误用 */
} cldb_condvar_t;

/* 初始化互斥量；mutex 必须指向未初始化存储，成功后可 lock/unlock。
 * 失败返回非 OK，对象保持未初始化状态。 */
cldb_status_t cldb_mutex_init(cldb_mutex_t *mutex);

/* 销毁互斥量；接受 NULL。要求没有线程持有该锁，成功后不可再使用，
 * 可重新 init。销毁已销毁的对象返回 InvalidArgument。 */
cldb_status_t cldb_mutex_destroy(cldb_mutex_t *mutex);

/* 加锁；未初始化对象返回 InvalidArgument。默认非递归，同一线程
 * 重复加锁会死锁，而不是返回错误。 */
cldb_status_t cldb_mutex_lock(cldb_mutex_t *mutex);

/* 解锁；必须由持有锁的线程调用。未初始化对象返回 InvalidArgument。 */
cldb_status_t cldb_mutex_unlock(cldb_mutex_t *mutex);

/* 初始化条件变量并绑定已初始化的 mutex；condvar 必须指向未初始化
 * 存储，mutex 必须已 init 且生命周期覆盖 condvar。 */
cldb_status_t cldb_condvar_init(cldb_condvar_t *condvar,
                                cldb_mutex_t *mutex);

/* 销毁条件变量；接受 NULL。要求没有线程在等待，成功后不可再使用。 */
cldb_status_t cldb_condvar_destroy(cldb_condvar_t *condvar);

/* 释放锁并等待唤醒，返回时重新持有该锁；对应上游 CondVar::Wait。
 * REQUIRES: 调用者已持有 condvar 绑定的 mutex。
 * 必须在 while (!predicate) 循环中调用，以处理虚假唤醒。 */
cldb_status_t cldb_condvar_wait(cldb_condvar_t *condvar);

/* 唤醒至少一个等待者；对应上游 CondVar::Signal。 */
cldb_status_t cldb_condvar_signal(cldb_condvar_t *condvar);

/* 唤醒全部等待者；对应上游 CondVar::SignalAll。 */
cldb_status_t cldb_condvar_broadcast(cldb_condvar_t *condvar);

#endif /* CLDB_SRC_PORT_PORT_MUTEX_H_ */
