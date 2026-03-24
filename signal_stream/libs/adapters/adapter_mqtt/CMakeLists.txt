add_library(adapter_mqtt STATIC
    src/mqtt_adapter.cpp
)
add_library(SignalStream::AdapterMQTT ALIAS adapter_mqtt)

target_include_directories(adapter_mqtt
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(adapter_mqtt
    PUBLIC
        SignalStream::Core
    PRIVATE
        async_mqtt_iface::async_mqtt_iface
)

if(MSVC)
    target_compile_options(adapter_mqtt PRIVATE /bigobj)
    target_compile_options(adapter_mqtt PRIVATE /utf-8)
endif()