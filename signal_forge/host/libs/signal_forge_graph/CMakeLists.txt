add_library(signal_forge_graph STATIC
    src/graph.cpp
    src/node_canvas.cpp
    src/renderer.cpp
    src/property_adapter.cpp
)
add_library(SignalForge::Graph ALIAS signal_forge_graph)

target_include_directories(signal_forge_graph
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(signal_forge_graph
    PRIVATE
        libglew_static
        Freetype::Freetype
        wx::wxcore
        wx::wxaui
        wx::wxgl
        glm::glm
        SignalCore::ui
)
