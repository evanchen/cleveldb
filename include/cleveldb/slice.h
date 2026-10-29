#ifndef CLDB_INCLUDE_CLEVELDB_SLICE_H_
#define CLDB_INCLUDE_CLEVELDB_SLICE_H_

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* 只读字节视图；对应上游 Slice。按值传递、不负责释放 data，
 * 让 key/value 比较和传递不产生拷贝，生命周期由底层存储保证。 */
typedef struct cldb_slice {
  const char *data; /* 仅引用；对应上游 data_；size > 0 时必须非 NULL */
  size_t size;      /* 字节数；对应上游 size_；不要求 NUL 结尾 */
} cldb_slice_t;

/* 构造只读视图；对应上游 Slice(d, n)。data 为 NULL 时返回空切片，
 * 避免 C 调用方额外判空，也不改变二进制内容的语义。 */
static inline cldb_slice_t cldb_slice_make(const char *data, size_t size) {
  cldb_slice_t slice;

  if (data == NULL) {
    slice.data = "";
    slice.size = 0;
  } else {
    slice.data = data;
    slice.size = size;
  }
  return slice;
}

/* 空切片；对应上游 Slice() 默认构造，data 指向静态空串。 */
static inline cldb_slice_t cldb_slice_empty(void) {
  return cldb_slice_make(NULL, 0);
}

/* 返回内部指针；对应上游 data()，调用者不得写入或释放。 */
static inline const char *cldb_slice_data(cldb_slice_t slice) {
  return slice.data;
}

/* 返回字节数；对应上游 size()，不依赖 NUL 终止。 */
static inline size_t cldb_slice_size(cldb_slice_t slice) { return slice.size; }

/* 判断长度是否为零；对应上游 empty()。 */
static inline bool cldb_slice_is_empty(cldb_slice_t slice) {
  return slice.size == 0;
}

/* 三路比较；对应上游 Slice::compare()。只保证返回值符号，
 * 因为 memcmp 的具体数值依实现而定。 */
static inline int cldb_slice_compare(cldb_slice_t a, cldb_slice_t b) {
  size_t min_size = a.size < b.size ? a.size : b.size;
  int result = min_size == 0 ? 0 : memcmp(a.data, b.data, min_size);

  if (result == 0) {
    if (a.size < b.size) {
      result = -1;
    } else if (a.size > b.size) {
      result = 1;
    }
  }
  return result;
}

/* 内容与长度都相等；对应上游 operator== 的 Slice 比较。 */
static inline bool cldb_slice_equal(cldb_slice_t a, cldb_slice_t b) {
  return a.size == b.size &&
         (a.size == 0 || memcmp(a.data, b.data, a.size) == 0);
}

/* 判断 prefix 是否为 slice 的前缀；对应上游 starts_with()，空前缀恒为真。 */
static inline bool cldb_slice_starts_with(cldb_slice_t slice,
                                          cldb_slice_t prefix) {
  return prefix.size <= slice.size &&
         (prefix.size == 0 ||
          memcmp(slice.data, prefix.data, prefix.size) == 0);
}

/* 删除前 n 字节；对应上游 remove_prefix()。上游 debug 下断言 n <= size；
 * C 版 n 超过长度时置空，避免越界并保持可预测行为。 */
static inline void cldb_slice_remove_prefix(cldb_slice_t *slice, size_t n) {
  if (slice == NULL) {
    return;
  }
  if (n >= slice->size) {
    slice->data = "";
    slice->size = 0;
    return;
  }
  slice->data += n;
  slice->size -= n;
}

#endif /* CLDB_INCLUDE_CLEVELDB_SLICE_H_ */
