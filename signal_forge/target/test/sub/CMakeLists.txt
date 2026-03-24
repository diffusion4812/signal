find_package(Threads REQUIRED)

add_executable(sub
    sub.c
)

target_link_libraries(sub
    dbg_pubsub
)