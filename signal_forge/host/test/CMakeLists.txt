include(GoogleTest)
enable_testing()

add_executable(signal_forge_host_test
    src/main.cpp
)

target_include_directories(signal_forge_host_test
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(signal_forge_host_test
    PRIVATE
        GTest::gtest_main
        wx::net
        task_host_core
)

gtest_discover_tests(signal_forge_host_test)