#include "harness.h"

#include <errno.h>
#include <inttypes.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  const char *name;
  cldb_test_fn_t fn;
} cldb_test_case_t;

static cldb_test_case_t *g_tests;
static size_t g_test_count;
static size_t g_test_capacity;
static bool g_registration_failed;

static size_t g_current_failures;
static size_t g_total_failures;
static bool g_in_test;
static jmp_buf g_abort_jmp;

static void registration_error(const char *message, const char *name) {
  fprintf(stderr, "test registration failed: %s: %s\n", message,
          name != NULL ? name : "(null)");
  g_registration_failed = true;
}

void cldb_test_register(const char *name, cldb_test_fn_t fn) {
  size_t i;

  if (name == NULL || fn == NULL) {
    registration_error("null name or function", name);
    return;
  }

  for (i = 0; i < g_test_count; ++i) {
    if (strcmp(g_tests[i].name, name) == 0) {
      registration_error("duplicate name", name);
      return;
    }
  }

  if (g_test_count == g_test_capacity) {
    size_t new_capacity = g_test_capacity == 0 ? 16 : g_test_capacity * 2;
    cldb_test_case_t *tests;

    if (new_capacity > SIZE_MAX / sizeof(*tests)) {
      registration_error("registry too large", name);
      return;
    }
    tests = realloc(g_tests, new_capacity * sizeof(*tests));
    if (tests == NULL) {
      registration_error("out of memory", name);
      return;
    }
    g_tests = tests;
    g_test_capacity = new_capacity;
  }

  g_tests[g_test_count].name = name;
  g_tests[g_test_count].fn = fn;
  ++g_test_count;
}

static void record_failure(const char *file, int line, const char *expr,
                           bool fatal) {
  ++g_current_failures;
  ++g_total_failures;
  printf("%s:%d: %s failed: %s\n", file, line, fatal ? "ASSERT" : "EXPECT",
         expr);
}

static void abort_if_fatal(bool fatal) {
  if (!fatal) {
    return;
  }
  if (!g_in_test) {
    fprintf(stderr, "fatal assertion outside a running test\n");
    abort();
  }
  longjmp(g_abort_jmp, 1);
}

void cldb_test_check_bool(const char *file, int line, const char *expr,
                          bool expected, bool actual, bool fatal) {
  if (actual == expected) {
    return;
  }
  record_failure(file, line, expr, fatal);
  printf("  expected: %s\n  actual:   %s\n", expected ? "true" : "false",
         actual ? "true" : "false");
  abort_if_fatal(fatal);
}

void cldb_test_check_int(const char *file, int line, const char *expr,
                         int64_t expected, int64_t actual, bool fatal) {
  if (expected == actual) {
    return;
  }
  record_failure(file, line, expr, fatal);
  printf("  expected: %" PRId64 "\n  actual:   %" PRId64 "\n", expected,
         actual);
  abort_if_fatal(fatal);
}

void cldb_test_check_uint(const char *file, int line, const char *expr,
                          uint64_t expected, uint64_t actual, bool fatal) {
  if (expected == actual) {
    return;
  }
  record_failure(file, line, expr, fatal);
  printf("  expected: %" PRIu64 "\n  actual:   %" PRIu64 "\n", expected,
         actual);
  abort_if_fatal(fatal);
}

void cldb_test_check_ptr(const char *file, int line, const char *expr,
                         const void *expected, const void *actual, bool fatal) {
  if (expected == actual) {
    return;
  }
  record_failure(file, line, expr, fatal);
  printf("  expected: %p\n  actual:   %p\n", (void *)expected,
         (void *)actual);
  abort_if_fatal(fatal);
}

static bool strings_equal(const char *a, const char *b) {
  if (a == NULL || b == NULL) {
    return a == b;
  }
  return strcmp(a, b) == 0;
}

static const char *string_or_null(const char *s) {
  return s != NULL ? s : "(null)";
}

void cldb_test_check_str(const char *file, int line, const char *expr,
                         const char *expected, const char *actual, bool fatal) {
  if (strings_equal(expected, actual)) {
    return;
  }
  record_failure(file, line, expr, fatal);
  printf("  expected: \"%s\"\n  actual:   \"%s\"\n",
         string_or_null(expected), string_or_null(actual));
  abort_if_fatal(fatal);
}

static bool run_one(const cldb_test_case_t *test) {
  g_current_failures = 0;
  g_in_test = true;

  printf("[ RUN      ] %s\n", test->name);
  fflush(stdout);

  if (setjmp(g_abort_jmp) == 0) {
    test->fn();
  }
  g_in_test = false;

  if (g_current_failures == 0) {
    printf("[       OK ] %s\n", test->name);
    return true;
  }
  printf("[  FAILED  ] %s (%zu failure%s)\n", test->name, g_current_failures,
         g_current_failures == 1 ? "" : "s");
  return false;
}

static bool parse_repeat(const char *value, size_t *repeat) {
  char *end = NULL;
  unsigned long long parsed;

  if (value[0] < '0' || value[0] > '9') {
    return false;
  }
  errno = 0;
  parsed = strtoull(value, &end, 10);
  if (errno != 0 || end == value || *end != '\0' || parsed == 0 ||
      parsed > (unsigned long long)SIZE_MAX) {
    return false;
  }
  *repeat = (size_t)parsed;
  return true;
}

static bool matches(const char *name, const char *filter) {
  return filter == NULL || strstr(name, filter) != NULL;
}

static int usage(const char *program) {
  fprintf(stderr,
          "usage: %s [--filter=<substr>] [--list] [--repeat=<N>]\n", program);
  return 2;
}

int cldb_test_run(int argc, char **argv) {
  const char *filter = NULL;
  bool list = false;
  size_t repeat = 1;
  size_t ran = 0;
  size_t passed = 0;
  size_t failed = 0;
  size_t i;
  size_t r;

  /* 行缓冲：测试崩溃时已输出的诊断不丢。 */
  setvbuf(stdout, NULL, _IOLBF, BUFSIZ);

  for (i = 1; i < (size_t)argc; ++i) {
    const char *arg = argv[i];

    if (strcmp(arg, "--list") == 0) {
      list = true;
    } else if (strncmp(arg, "--filter=", 9) == 0) {
      filter = arg + 9;
    } else if (strncmp(arg, "--repeat=", 9) == 0) {
      if (!parse_repeat(arg + 9, &repeat)) {
        fprintf(stderr, "invalid --repeat: %s\n", arg + 9);
        return usage(argv[0]);
      }
    } else {
      return usage(argv[0]);
    }
  }

  if (g_registration_failed) {
    return 1;
  }

  if (list) {
    for (i = 0; i < g_test_count; ++i) {
      if (matches(g_tests[i].name, filter)) {
        printf("%s\n", g_tests[i].name);
      }
    }
    return 0;
  }

  g_total_failures = 0;
  for (r = 0; r < repeat; ++r) {
    for (i = 0; i < g_test_count; ++i) {
      if (!matches(g_tests[i].name, filter)) {
        continue;
      }
      ++ran;
      if (run_one(&g_tests[i])) {
        ++passed;
      } else {
        ++failed;
      }
    }
  }

  printf("[==========] %zu test%s ran.\n", ran, ran == 1 ? "" : "s");
  printf("[  PASSED  ] %zu test%s.\n", passed, passed == 1 ? "" : "s");
  printf("[  FAILED  ] %zu test%s, %zu assertion failure%s.\n", failed,
         failed == 1 ? "" : "s", g_total_failures,
         g_total_failures == 1 ? "" : "s");
  return failed == 0 ? 0 : 1;
}
