add_library(signal_ui STATIC
    src/text.cpp
)
add_library(SignalCore::ui ALIAS signal_ui)

target_include_directories(signal_ui
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(signal_ui
    PUBLIC
        glm::glm
    PRIVATE
        libglew_static
        Freetype::Freetype
        wx::wxcore
        wx::wxgl
)