add_library(task_2 SHARED
    src/task.c
)

target_include_directories(task_2
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(task_2
    PRIVATE
        task_host_core
        m
)
