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
set(SLANG_USE_MIMALLOC ON CACHE BOOL "")
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

find_package(Threads REQUIRED)
add_executable(parallel_topk_test tests/parallel_topk_test.cc)
target_include_directories(parallel_topk_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(parallel_topk_test PRIVATE Threads::Threads ${CMAKE_DL_LIBS})

enable_testing()
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(RTL_TRACE_PARENT_BIN "/tmp/rtl_trace_item5_round3_main" CACHE FILEPATH
    "Frozen main binary for legacy reader and incremental-cache regression tests")
set(RTL_TRACE_PARENT_ARGS)
if(EXISTS "${RTL_TRACE_PARENT_BIN}")
  list(APPEND RTL_TRACE_PARENT_ARGS --parent-bin "${RTL_TRACE_PARENT_BIN}")
else()
  message(STATUS "SKIP: frozen main binary unavailable: ${RTL_TRACE_PARENT_BIN}")
endif()
set(RTL_TRACE_REFERENCE_BIN "/tmp/rtl_trace_item5_round3_main" CACHE FILEPATH "Frozen reference rtl_trace for compatibility tests")
set(RTL_TRACE_REFERENCE_ARGS)
if(RTL_TRACE_REFERENCE_BIN)
  if(NOT EXISTS "${RTL_TRACE_REFERENCE_BIN}")
    message(FATAL_ERROR "RTL_TRACE_REFERENCE_BIN does not exist")
  endif()
  list(APPEND RTL_TRACE_REFERENCE_ARGS --reference-bin "${RTL_TRACE_REFERENCE_BIN}")
endif()

add_test(
  NAME rtl_trace_semantic_regression
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/tests/semantic_regression.py
          --rtl-trace $<TARGET_FILE:rtl_trace>
          --source-dir ${CMAKE_CURRENT_SOURCE_DIR}
)
set_tests_properties(
  rtl_trace_semantic_regression
  PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..
)

add_test(NAME assignment_utils_test COMMAND assignment_utils_test)
add_test(NAME parallel_topk_test COMMAND parallel_topk_test)

add_executable(canonical_path_test tests/canonical_path_test.cc)
target_include_directories(canonical_path_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME canonical_path_test COMMAND canonical_path_test)

add_executable(endpoint_dedup_test tests/endpoint_dedup_test.cc)
target_include_directories(endpoint_dedup_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(endpoint_dedup_test PRIVATE slang::slang)
add_test(NAME endpoint_dedup_test COMMAND endpoint_dedup_test)

add_test(
  NAME endpoint_dedup_regression
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/tests/endpoint_dedup_regression.py
          --rtl-trace $<TARGET_FILE:rtl_trace>
          --source-dir ${CMAKE_CURRENT_SOURCE_DIR}
)

add_test(
  NAME multidim_axes_regression
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/tests/multidim_axes_regression.py
          --rtl-trace $<TARGET_FILE:rtl_trace>
          --source-dir ${CMAKE_CURRENT_SOURCE_DIR}
          ${RTL_TRACE_PARENT_ARGS}
)

add_test(
  NAME merged_range_regression
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/tests/merged_range_regression.py
          --rtl-trace $<TARGET_FILE:rtl_trace>
          --source-dir ${CMAKE_CURRENT_SOURCE_DIR}
)

add_test(
  NAME coordinate_diagnostics_regression
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/tests/coordinate_diagnostics_regression.py
          --rtl-trace $<TARGET_FILE:rtl_trace>
          --source-dir ${CMAKE_CURRENT_SOURCE_DIR}
          ${RTL_TRACE_PARENT_ARGS}
)

add_test(
  NAME member_loads_regression
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/tests/member_loads_regression.py
          --rtl-trace $<TARGET_FILE:rtl_trace>
          --source-dir ${CMAKE_CURRENT_SOURCE_DIR}
          ${RTL_TRACE_PARENT_ARGS}
)
set(RTL_TRACE_MAPPING_REFERENCE_BIN "/tmp/rtl_trace_item5_round3_phase1" CACHE FILEPATH
    "Frozen visible v6 binary for mmap compatibility tests")
set(RTL_TRACE_MAPPING_REFERENCE_ARGS)
if(EXISTS "${RTL_TRACE_MAPPING_REFERENCE_BIN}")
  list(APPEND RTL_TRACE_MAPPING_REFERENCE_ARGS --reference-bin "${RTL_TRACE_MAPPING_REFERENCE_BIN}")
endif()

add_test(
  NAME graph_db_mapping_regression
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/tests/graph_db_mapping_regression.py
          --rtl-trace $<TARGET_FILE:rtl_trace>
          ${RTL_TRACE_MAPPING_REFERENCE_ARGS}
)

add_executable(graph_pod_view_test tests/graph_pod_view_test.cc)
target_include_directories(graph_pod_view_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(graph_pod_view_test PRIVATE slang::slang)
add_test(NAME graph_pod_view_test COMMAND graph_pod_view_test)
add_test(
  NAME graph_publication_regression
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/tests/graph_publication_regression.py
          --rtl-trace $<TARGET_FILE:rtl_trace>
          ${RTL_TRACE_REFERENCE_ARGS}
)

set(RTL_TRACE_GENERATE_PARENT_BIN "" CACHE FILEPATH
    "Optional pre-epoch-11 binary for inactive-generate incremental rebuild regression")
set(RTL_TRACE_GENERATE_PARENT_ARGS)
if(EXISTS "${RTL_TRACE_GENERATE_PARENT_BIN}")
  list(APPEND RTL_TRACE_GENERATE_PARENT_ARGS --parent-bin "${RTL_TRACE_GENERATE_PARENT_BIN}")
endif()
add_test(
  NAME uninstantiated_generate_regression
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/tests/uninstantiated_generate_regression.py
          --rtl-trace $<TARGET_FILE:rtl_trace>
          --source-dir ${CMAKE_CURRENT_SOURCE_DIR}
          ${RTL_TRACE_GENERATE_PARENT_ARGS}
)
add_test(
  NAME port_parent_traversal_regression
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/tests/port_parent_traversal_regression.py
          --rtl-trace $<TARGET_FILE:rtl_trace>
          --source-dir ${CMAKE_CURRENT_SOURCE_DIR}
)
add_test(
  NAME global_port_compaction_regression
  COMMAND ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/tests/global_port_compaction_regression.py
          --rtl-trace $<TARGET_FILE:rtl_trace>
          --source-dir ${CMAKE_CURRENT_SOURCE_DIR}
)
