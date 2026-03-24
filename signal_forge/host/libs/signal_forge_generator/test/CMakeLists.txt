include(GoogleTest)
enable_testing()

add_executable(signal_forge_generator_test
    src/main.cpp
)

target_include_directories(signal_forge_generator_test
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(signal_forge_generator_test
    PRIVATE
        GTest::gtest_main
        SignalForge::Graph
        SignalForge::Generator
)

gtest_discover_tests(signal_forge_generator_test)