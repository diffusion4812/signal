cmake_minimum_required(VERSION 3.26)
project(SignalForge VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_subdirectory(libs/signal_forge_graph)
add_subdirectory(libs/signal_forge_generator)
add_subdirectory(app/signal_forge)
add_subdirectory(test)