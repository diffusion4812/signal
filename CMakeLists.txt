cmake_minimum_required(VERSION 3.26)
project(Signal VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(cmake/Dependencies.cmake)

add_subdirectory(signal_core)
add_subdirectory(signal_forge/host)
add_subdirectory(signal_forge/target/libs/task_host_core)
add_subdirectory(signal_forge/target/libs/dbg_pubsub)
add_subdirectory(signal_stream)