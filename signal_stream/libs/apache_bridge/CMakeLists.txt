add_library(apache_bridge STATIC
    src/apache_bridge.cpp
)
add_library(SignalStream::ApacheBridge ALIAS apache_bridge)

target_include_directories(apache_bridge
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(apache_bridge
    PRIVATE
        Arrow::arrow_shared
        avro-cpp::avrocpp_shared
        Boost::json
        fmt::fmt
)

target_compile_options(apache_bridge PRIVATE /utf-8 /bigobj)