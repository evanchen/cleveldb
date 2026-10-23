#ifndef CLDB_INCLUDE_CLEVELDB_SLICE_H_
#define CLDB_INCLUDE_CLEVELDB_SLICE_H_

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* 只读字节视图；按值传递，不拥有 data。 */
typedef struct cldb_slice {
  const char *data; /* borrowed；size > 0 时必须非 NULL */
  size_t size;      /* 字节数 */
} cldb_slice_t;

/* 构造借用视图；data 为 NULL 时统一返回空切片。 */
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

/* 空切片。 */
static inline cldb_slice_t cldb_slice_empty(void) {
  return cldb_slice_make(NULL, 0);
}

/* 借用只读数据指针。 */
static inline const char *cldb_slice_data(cldb_slice_t slice) {
  return slice.data;
}

/* 字节数。 */
static inline size_t cldb_slice_size(cldb_slice_t slice) { return slice.size; }

/* 判断长度是否为零。 */
static inline bool cldb_slice_is_empty(cldb_slice_t slice) {
  return slice.size == 0;
}

/* 仅保证返回值符号：<0、==0、>0。 */
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

/* 内容与长度都相等。 */
static inline bool cldb_slice_equal(cldb_slice_t a, cldb_slice_t b) {
  return a.size == b.size &&
         (a.size == 0 || memcmp(a.data, b.data, a.size) == 0);
}

/* 判断 prefix 是否为 slice 的前缀；空前缀恒为真。 */
static inline bool cldb_slice_starts_with(cldb_slice_t slice,
                                          cldb_slice_t prefix) {
  return prefix.size <= slice.size &&
         (prefix.size == 0 ||
          memcmp(slice.data, prefix.data, prefix.size) == 0);
}

/* 删除前 n 字节；n 超过 size 时置空。
 * 上游在 debug 下断言。 */
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
