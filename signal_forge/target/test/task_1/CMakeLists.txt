add_library(task_1 SHARED
    src/task.c
)

target_include_directories(task_1
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(task_1
    PRIVATE
        task_host_core
        m
)
