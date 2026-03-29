cmake_minimum_required(VERSION 3.26)
project(SignalForgeTarget VERSION 1.0 LANGUAGES C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(cmake/Dependencies.cmake)

add_subdirectory(libs/dbg_pubsub)
add_subdirectory(libs/task_host_core)
add_subdirectory(libs/signal_forge_target)
add_subdirectory(app/signal_forge)
add_subdirectory(app/task_runner)
add_subdirectory(test/sub)