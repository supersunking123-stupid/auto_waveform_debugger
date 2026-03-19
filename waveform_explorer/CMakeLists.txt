cmake_minimum_required(VERSION 3.14)
project(wave_agent_cli)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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
