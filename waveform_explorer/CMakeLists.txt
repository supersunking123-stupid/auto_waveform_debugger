cmake_minimum_required(VERSION 3.14)
project(wave_agent_cli)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(third_party)
include_directories(src)

file(GLOB SOURCES "src/*.cpp")

add_executable(wave_agent_cli ${SOURCES})