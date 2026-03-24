add_library(adapter_opcua STATIC
    src/opcua_adapter.cpp
)
add_library(SignalStream::AdapterOPCUA ALIAS adapter_opcua)

target_include_directories(adapter_opcua
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(adapter_opcua
    PUBLIC
        SignalStream::Core
    PRIVATE
        open62541pp::open62541pp
)

if(MSVC)
    target_compile_options(adapter_opcua PRIVATE /utf-8)
endif()