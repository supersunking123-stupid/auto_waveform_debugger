cmake_minimum_required(VERSION 3.20)
project(StandaloneTraceGraph LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Keep the project self-contained by pointing slang's fmt dependency at the
# vendored local copy instead of attempting a network fetch.
set(FETCHCONTENT_SOURCE_DIR_FMT ${CMAKE_CURRENT_SOURCE_DIR}/third_party/fmt CACHE PATH "")

set(SLANG_INCLUDE_TOOLS OFF CACHE BOOL "")
set(SLANG_INCLUDE_TESTS OFF CACHE BOOL "")
set(SLANG_INCLUDE_DOCS OFF CACHE BOOL "")
set(SLANG_INCLUDE_INSTALL OFF CACHE BOOL "")
set(SLANG_USE_MIMALLOC OFF CACHE BOOL "")
set(SLANG_USE_CPPTRACE OFF CACHE BOOL "")

add_subdirectory(third_party/slang)

add_executable(rtl_elab main.cc)
target_link_libraries(rtl_elab PRIVATE slang::slang)

enable_testing()
find_package(Python3 COMPONENTS Interpreter REQUIRED)

add_test(
  NAME rtl_elab_smoke
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/tests/smoke_test.py
          --rtl-elab $<TARGET_FILE:rtl_elab>
          --source ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/smoke.sv
)
