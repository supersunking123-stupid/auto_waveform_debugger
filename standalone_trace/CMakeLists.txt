cmake_minimum_required(VERSION 3.20)
project(StandaloneTrace LANGUAGES CXX)

# Default to Release for -O3 -DNDEBUG unless explicitly overridden.
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Keep the standalone package self-contained: force slang's FetchContent(fmt)
# to use the vendored local source instead of downloading from the internet.
set(FETCHCONTENT_SOURCE_DIR_FMT ${CMAKE_CURRENT_SOURCE_DIR}/third_party/fmt CACHE PATH "")

# Build only what is needed for static RTL tracing.
set(SLANG_INCLUDE_TOOLS OFF CACHE BOOL "")
set(SLANG_INCLUDE_TESTS OFF CACHE BOOL "")
set(SLANG_INCLUDE_DOCS OFF CACHE BOOL "")
set(SLANG_INCLUDE_INSTALL OFF CACHE BOOL "")
set(SLANG_USE_MIMALLOC OFF CACHE BOOL "")
set(SLANG_USE_CPPTRACE OFF CACHE BOOL "")

add_subdirectory(third_party/slang)

add_executable(rtl_trace
  main.cc
  AssignmentUtils.cc
  db/GraphDb.cc
  compile/Compiler.cc
  query/TraceQuery.cc
  query/HierQuery.cc
  query/FindQuery.cc
  query/WhereIsQuery.cc
  serve/ServeLoop.cc
)
target_include_directories(rtl_trace PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(rtl_trace PRIVATE slang::slang)

add_executable(assignment_utils_test tests/assignment_utils_test.cc AssignmentUtils.cc)
target_include_directories(assignment_utils_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

enable_testing()
find_package(Python3 COMPONENTS Interpreter REQUIRED)

add_test(
  NAME rtl_trace_semantic_regression
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/tests/semantic_regression.py
          --rtl-trace $<TARGET_FILE:rtl_trace>
          --source-dir ${CMAKE_CURRENT_SOURCE_DIR}
)

add_test(NAME assignment_utils_test COMMAND assignment_utils_test)
