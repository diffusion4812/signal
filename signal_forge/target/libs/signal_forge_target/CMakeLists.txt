add_library(signal_forge_target STATIC
    src/framework_shm.c
)

target_include_directories(signal_forge_target
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(signal_forge_target
    PRIVATE
        task_host_core
)