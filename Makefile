# cleveldb 构建入口；仅支持 Linux 与 GNU make。
UNAME_S ?= $(shell uname -s)
ifeq ($(UNAME_S),Linux)
else
$(error cleveldb only supports Linux (uname -s = $(UNAME_S)))
endif

CC ?= cc
AR ?= ar
BUILD ?= debug
BUILD_DIR ?= build
OBJ_DIR := $(BUILD_DIR)/obj/$(BUILD)
LIB := $(BUILD_DIR)/libcleveldb.a
TEST_BIN := $(BUILD_DIR)/cleveldb_tests
PREFIX ?= /usr/local
DESTDIR ?=

DEFINES := -D_GNU_SOURCE -DCLDB_PLATFORM_LINUX=1
INCLUDES := -Iinclude -Isrc -Itests
CPPFLAGS += $(DEFINES) $(INCLUDES) $(EXTRA_CPPFLAGS)
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Wshadow
LDLIBS += -pthread

ifeq ($(BUILD),debug)
CFLAGS += -O0 -g3
CPPFLAGS += -DCLDB_DEBUG=1
else ifeq ($(BUILD),release)
CFLAGS += -O2
CPPFLAGS += -DNDEBUG
else
$(error BUILD 必须是 debug 或 release)
endif

ifeq ($(WERROR),1)
CFLAGS += -Werror
endif

ifeq ($(SANITIZE),1)
CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer -g
LDFLAGS += -fsanitize=address,undefined
endif

LIB_SRCS := $(shell find src -name '*.c' | sort)
TEST_SRCS := $(shell find tests -name '*.c' | sort)
TOOL_SRCS := $(wildcard tools/*.c)
LIB_OBJS := $(patsubst %.c,$(OBJ_DIR)/%.o,$(LIB_SRCS))
TEST_OBJS := $(patsubst %.c,$(OBJ_DIR)/%.o,$(TEST_SRCS))
TOOL_BINS := $(patsubst tools/%.c,$(BUILD_DIR)/%,$(TOOL_SRCS))
DEPFILES := $(LIB_OBJS:.o=.d) $(TEST_OBJS:.o=.d)

.PHONY: all lib tests tools test check asan clean clean-all install help
.DEFAULT_GOAL := all

all: $(LIB) $(TEST_BIN)

lib: $(LIB)

tests: $(TEST_BIN)

tools: $(TOOL_BINS)

$(OBJ_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(LIB): $(LIB_OBJS)
	@mkdir -p $(dir $@)
	rm -f $@
	$(AR) rcs $@ $^

$(TEST_BIN): $(TEST_OBJS) $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) $(TEST_OBJS) $(LIB) $(LDLIBS) -o $@

$(BUILD_DIR)/%: tools/%.c $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< $(LIB) $(LDLIBS) -o $@

test: $(TEST_BIN)
	$(TEST_BIN)

check: $(TEST_BIN)
	@set -e; \
	echo "==> full suite"; \
	$(TEST_BIN); \
	echo "==> filter"; \
	out=$$($(TEST_BIN) --filter=harness_filter_target); \
	printf '%s\n' "$$out"; \
	printf '%s\n' "$$out" | grep -q "1 test ran"; \
	echo "==> list"; \
	$(TEST_BIN) --list | grep -q "harness_filter_target"; \
	echo "==> repeat"; \
	out=$$($(TEST_BIN) --filter=harness_filter_target --repeat=3); \
	printf '%s\n' "$$out"; \
	printf '%s\n' "$$out" | grep -q "3 tests ran"; \
	echo "==> failure path"; \
	set +e; \
	CLDB_TEST_ENABLE_FAILURE=1 $(TEST_BIN) --filter=harness_failure_example; \
	status=$$?; \
	set -e; \
	if [ $$status -ne 1 ]; then \
	  echo "failure path exit $$status, expected 1"; \
	  exit 1; \
	fi; \
	echo "==> check passed"

asan:
	$(MAKE) BUILD_DIR=build-asan clean
	$(MAKE) BUILD=debug BUILD_DIR=build-asan SANITIZE=1 check

clean:
	@case "$(BUILD_DIR)" in \
	  "" | "/" | "." | "$(CURDIR)") \
	    echo "拒绝删除构建目录: $(BUILD_DIR)"; \
	    exit 1;; \
	esac
	rm -rf -- $(BUILD_DIR)

clean-all:
	rm -rf -- build build-asan build-release build-clang

install: $(LIB)
	install -d "$(DESTDIR)$(PREFIX)/lib" \
	  "$(DESTDIR)$(PREFIX)/include/cleveldb"
	install -m 644 $(LIB) "$(DESTDIR)$(PREFIX)/lib/"
	install -m 644 include/cleveldb/*.h \
	  "$(DESTDIR)$(PREFIX)/include/cleveldb/"

help:
	@printf '%s\n' \
	  "用法: make [目标] [变量=值]" \
	  "" \
	  "目标:" \
	  "  all       构建静态库和测试（默认）" \
	  "  lib       只构建 $(LIB)" \
	  "  tests     只构建 $(TEST_BIN)" \
	  "  tools     构建 tools/*.c" \
	  "  test      运行完整测试套件" \
	  "  check     运行全套回归（过滤/列表/重复/失败路径）" \
	  "  asan      在 build-asan 下用 ASan/UBSan 跑 check" \
	  "  clean     删除当前 BUILD_DIR" \
	  "  clean-all 删除 build/build-asan/build-release/build-clang" \
	  "  install   安装库和公开头文件" \
	  "" \
	  "变量:" \
	  "  BUILD=debug|release   默认 debug" \
	  "  BUILD_DIR=目录        默认 build" \
	  "  CC=编译器             默认 cc" \
	  "  WERROR=1              把警告当作错误" \
	  "  PREFIX=路径           默认 /usr/local"

-include $(DEPFILES)
