cmake_minimum_required(VERSION 3.14)
project(wave_agent_cli)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(ENABLE_FSDB "Enable FSDB support via Verdi FsdbReader SDK" ON)
set(VERDI_HOME "/home/qsun/synopsys_apps/verdi/V-2023.12-SP2" CACHE PATH "Path to Verdi installation root")

include_directories(third_party)
include_directories(third_party/fst)
include_directories(src)

# Enable ZLIB (often required by FST)
find_package(ZLIB)
if(ZLIB_FOUND)
    include_directories(${ZLIB_INCLUDE_DIRS})
endif()

# FST Sources
set(FST_SOURCES
    third_party/fst/fstapi.c
    third_party/fst/fastlz.c
    third_party/fst/lz4.c
)

# Set compile flags for FST C files to avoid warnings or issues with C++ linkage
set_source_files_properties(${FST_SOURCES} PROPERTIES LANGUAGE C)

file(GLOB SOURCES "src/*.cpp")

add_executable(wave_agent_cli ${SOURCES} ${FST_SOURCES})

if(ZLIB_FOUND)
    target_link_libraries(wave_agent_cli ZLIB::ZLIB)
endif()
# Link with -lz explicitly just in case
target_link_libraries(wave_agent_cli z)

if(ENABLE_FSDB)
    set(FSDB_INCLUDE_DIR "${VERDI_HOME}/share/FsdbReader")
    set(FSDB_LIB_DIR "${VERDI_HOME}/share/FsdbReader/linux64")

    if(EXISTS "${FSDB_INCLUDE_DIR}/ffrAPI.h" AND EXISTS "${FSDB_LIB_DIR}/libnffr.so" AND EXISTS "${FSDB_LIB_DIR}/libnsys.so")
        message(STATUS "FSDB support enabled from ${VERDI_HOME}")
        target_compile_definitions(wave_agent_cli PRIVATE WAVE_HAS_FSDB=1)
        target_include_directories(wave_agent_cli PRIVATE "${FSDB_INCLUDE_DIR}")
        target_link_directories(wave_agent_cli PRIVATE "${FSDB_LIB_DIR}")
        target_link_libraries(wave_agent_cli nffr nsys dl pthread)
        target_link_options(wave_agent_cli PRIVATE "-Wl,-rpath,${FSDB_LIB_DIR}")
    else()
        message(WARNING "FSDB SDK not found at ${VERDI_HOME}; building without FSDB support.")
    endif()
endif()
