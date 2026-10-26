#ifndef CLDB_SRC_PORT_PORT_THREAD_H_
#define CLDB_SRC_PORT_PORT_THREAD_H_

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include "cleveldb/status.h"
#include "port/port.h"

/* POSIX 线程与睡眠；对应上游 std::thread、Env::StartThread 和
 * Env::SleepForMicroseconds。线程 start 成功后必须由 join 或 detach
 * 恰好消费一次。 */

/* 线程入口；对应 pthread start_routine。返回值由 join 交给调用者，
 * 不需要时可忽略。 */
typedef void *(*cldb_thread_fn_t)(void *arg);

typedef struct cldb_thread {
  pthread_t handle; /* owned；start 成功后有效 */
  bool started;     /* 已创建且尚未 join/detach；用于拒绝误用 */
} cldb_thread_t;

/* 清空线程对象；start 前必须调用，接受 NULL。对象正在运行时不得重置。 */
void cldb_thread_init(cldb_thread_t *thread);

/* 创建并启动线程；对应上游 StartThread 与 pthread_create。thread 必须
 * 已 init 且当前未启动；arg 为 borrowed，线程结束前必须保持有效。
 * 失败返回非 OK，对象保持未启动。成功后线程为 joinable，直到 join
 * 或 detach。 */
cldb_status_t cldb_thread_start(cldb_thread_t *thread, cldb_thread_fn_t fn,
                                void *arg);

/* 等待线程结束并消费其生命周期；对应 pthread_join。result 可以为 NULL，
 * 成功时写入线程返回值，其所有权由线程函数与调用者约定，库不释放。
 * 已 join/detach 或未启动的对象返回 InvalidArgument。成功后对象可再次
 * start。 */
cldb_status_t cldb_thread_join(cldb_thread_t *thread, void **result);

/* 分离线程；对应 pthread_detach。分离后资源在线程退出时自动回收，
 * 不能再 join；start 的 arg 仍需在线程结束前保持有效。已 join/detach
 * 或未启动的对象返回 InvalidArgument。 */
cldb_status_t cldb_thread_detach(cldb_thread_t *thread);

/* 睡眠指定毫秒数；对应上游 SleepForMicroseconds(millis * 1000)。
 * nanosleep 被信号中断时按剩余时间自动重试。时长超出 time_t 表示
 * 范围返回 InvalidArgument，其余系统错误返回 IOError。 */
cldb_status_t cldb_sleep_for_millis(uint64_t millis);

/* 睡眠指定微秒数；对应上游 Env::SleepForMicroseconds，语义同上。 */
cldb_status_t cldb_sleep_for_micros(uint64_t micros);

#endif /* CLDB_SRC_PORT_PORT_THREAD_H_ */
