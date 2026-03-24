add_library(adapter_udp STATIC
    src/udp_adapter.cpp
)
add_library(SignalStream::AdapterUDP ALIAS adapter_udp)

target_include_directories(adapter_udp
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(adapter_udp
    PUBLIC
        SignalStream::Core
)

target_compile_options(adapter_udp PRIVATE /utf-8)