cmake_minimum_required(VERSION 3.16)
project(cleveldb VERSION 0.1.0 LANGUAGES C)

# 仅支持 Linux（POSIX + pthread）。
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR
    "cleveldb only supports Linux (CMAKE_SYSTEM_NAME=${CMAKE_SYSTEM_NAME}).")
endif()

if(NOT CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  message(FATAL_ERROR
    "cleveldb requires GCC or Clang (found ${CMAKE_C_COMPILER_ID}).")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

option(BUILD_SHARED_LIBS "Build shared libraries instead of static" OFF)
option(CLDB_BUILD_TESTS "Build the unit-test suite" ON)
option(CLDB_BUILD_TOOLS "Build cldb_dump / cldb_bench when their sources exist" OFF)
option(CLDB_WERROR "Treat compiler warnings as errors" OFF)

find_package(Threads REQUIRED)

# 自有 target 的统一编译设置；第三方代码不要使用。
function(cldb_configure_target target)
  target_compile_definitions(${target} PRIVATE
    _GNU_SOURCE
    CLDB_PLATFORM_LINUX=1)

  target_compile_options(${target} PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    $<$<CONFIG:Debug>:-O0>
    $<$<CONFIG:Debug>:-g3>
    $<$<CONFIG:Release>:-O2>)

  target_compile_definitions(${target} PRIVATE
    $<$<CONFIG:Debug>:CLDB_DEBUG=1>)

  if(CLDB_WERROR)
    target_compile_options(${target} PRIVATE -Werror)
  endif()
endfunction()

# 收集库源码；内部头留在 src/，只有 include/ 对外。
file(GLOB_RECURSE CLDB_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")

add_library(cleveldb ${CLDB_SOURCES})
add_library(cleveldb::cleveldb ALIAS cleveldb)

target_include_directories(cleveldb
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(cleveldb PUBLIC Threads::Threads)
cldb_configure_target(cleveldb)

# 只安装 include/ 下的公开头文件，src/ 内部头不安装。
include(GNUInstallDirs)
install(TARGETS cleveldb
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/cleveldb"
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  FILES_MATCHING PATTERN "*.h")

if(CLDB_BUILD_TESTS)
  enable_testing()

  file(GLOB_RECURSE CLDB_TEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.c")

  add_executable(cleveldb_tests ${CLDB_TEST_SOURCES})
  target_include_directories(cleveldb_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  target_link_libraries(cleveldb_tests PRIVATE cleveldb::cleveldb)
  cldb_configure_target(cleveldb_tests)

  # 全量、过滤、列表、重复、故意失败五条回归。
  add_test(NAME cleveldb_suite COMMAND cleveldb_tests)

  add_test(NAME cleveldb_filter COMMAND cleveldb_tests
    --filter=harness_filter_target)
  set_tests_properties(cleveldb_filter PROPERTIES
    PASS_REGULAR_EXPRESSION "1 test ran")

  add_test(NAME cleveldb_list COMMAND cleveldb_tests --list)
  set_tests_properties(cleveldb_list PROPERTIES
    PASS_REGULAR_EXPRESSION "harness_filter_target")

  add_test(NAME cleveldb_repeat COMMAND cleveldb_tests
    --filter=harness_filter_target --repeat=3)
  set_tests_properties(cleveldb_repeat PROPERTIES
    PASS_REGULAR_EXPRESSION "3 tests ran")

  add_test(NAME cleveldb_failure_path COMMAND cleveldb_tests
    --filter=harness_failure_example)
  set_tests_properties(cleveldb_failure_path PROPERTIES
    ENVIRONMENT "CLDB_TEST_ENABLE_FAILURE=1"
    WILL_FAIL TRUE)
endif()

if(CLDB_BUILD_TOOLS)
  foreach(tool IN ITEMS cldb_dump cldb_bench)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/${tool}.c")
      add_executable(${tool} "tools/${tool}.c")
      target_link_libraries(${tool} PRIVATE cleveldb::cleveldb)
      cldb_configure_target(${tool})
    else()
      message(STATUS
        "CLDB_BUILD_TOOLS=ON: tools/${tool}.c does not exist yet; skipping ${tool}")
    endif()
  endforeach()
endif()
