add_library(signal_forge_generator STATIC
    src/generator.cpp
    src/header_parser.cpp
)
add_library(SignalForge::Generator ALIAS signal_forge_generator)

target_include_directories(signal_forge_generator
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(signal_forge_generator
    PRIVATE
        SignalForge::Graph
        task_host_core
)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/test)