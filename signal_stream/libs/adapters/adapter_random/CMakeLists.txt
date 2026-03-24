add_library(adapter_random STATIC
    src/random_adapter.cpp
    src/random_adapter_actions.cpp
)
add_library(SignalStream::AdapterRandom ALIAS adapter_random)

target_include_directories(adapter_random
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(adapter_random
    PRIVATE
        SignalStream::Core
        SignalStream::App
        avro-cpp::avrocpp_shared
)

target_compile_options(adapter_random PRIVATE /utf-8)