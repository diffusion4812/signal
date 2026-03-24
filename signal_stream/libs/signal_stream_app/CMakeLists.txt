add_library(signal_stream_app STATIC
    src/source_config_propgrid_registry.cpp
)
add_library(SignalStream::App ALIAS signal_stream_app)

target_include_directories(signal_stream_app
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(signal_stream_app
    PUBLIC
        wx::wxcore
        Arrow::arrow_shared
)

target_compile_options(signal_stream_app PRIVATE /utf-8 /bigobj)