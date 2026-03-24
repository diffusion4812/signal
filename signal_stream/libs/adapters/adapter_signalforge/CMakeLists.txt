add_library(adapter_signalforge STATIC
    src/signalforge_adapter.cpp
    src/signalforge_adapter_actions.cpp
)
add_library(SignalStream::AdapterSignalForge ALIAS adapter_signalforge)

target_include_directories(adapter_signalforge
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(adapter_signalforge
    PUBLIC
        SignalStream::App
        SignalStream::Core
        dbg_pubsub
)

if(MSVC)
    target_compile_options(adapter_signalforge PRIVATE /utf-8)
endif()