add_library(signal_stream_core STATIC
    src/adapter_registration.cpp
    src/source.cpp
    src/orchestrator.cpp
    src/source_registry.cpp
    src/storage_manager.cpp
)
add_library(SignalStream::Core ALIAS signal_stream_core)

target_include_directories(signal_stream_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(signal_stream_core
    PUBLIC
        avro-cpp::avrocpp_shared
    PRIVATE
        SignalStream::ApacheBridge
        Arrow::arrow_shared
        Parquet::parquet_shared
        Boost::json
        spdlog::spdlog
)

target_compile_options(signal_stream_core PRIVATE /utf-8 /bigobj)