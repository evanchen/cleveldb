#include "cleveldb/filter_policy.h"

#include "port/port.h"

/* 对应上游 FilterPolicy 的虚析构；具体策略由 bloom.c 等实现 destroy。
 * 内置策略由具体 destroy 释放自身，调用者不应绕过本函数。 */
void cldb_filter_policy_destroy(cldb_filter_policy_t *policy) {
  if (policy == NULL) {
    return;
  }
  CLDB_ASSERT(policy->vtbl != NULL);
  CLDB_ASSERT(policy->vtbl->destroy != NULL);
  policy->vtbl->destroy(policy);
}
