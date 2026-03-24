#pragma once

#include <vector>
#include <map>
#include <GL/glew.h>
#include <GL/gl.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MODULE_H
#include <wx/wx.h>
#include <wx/window.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace signal_ui {

static const char* textVertexShader = R"(
#version 330 core
layout (location = 0) in vec2 aPos;
layout (location = 1) in vec2 aTexCoord;

out vec2 TexCoord;

uniform mat4 projection;
uniform mat4 model;

void main() {
    gl_Position = projection * model * vec4(aPos, 0.0, 1.0);
    TexCoord = aTexCoord;
}
)";

static const char* textFragmentShader = R"(
#version 330 core
in vec2 TexCoord;
out vec4 FragColor;

uniform sampler2D text;
uniform vec4 textColor;

void main() {
    // 1. Sample the distance field (0.5 is the edge)
    float distance = texture(text, TexCoord).r;
    
    // 2. Calculate smoothing (anti-aliasing)
    // fwidth calculates how much the distance value changes between pixels on screen.
    // This keeps the edge sharp at any zoom level or resolution.
    float smoothing = fwidth(distance);
    
    // 3. Create the alpha mask
    // Values > 0.5 are inside the glyph, values < 0.5 are outside.
    float alpha = smoothstep(0.5 - smoothing, 0.5 + smoothing, distance);
    
    FragColor = vec4(textColor.rgb, textColor.a * alpha);
}
)";

struct RGB {
    float r, g, b;
};

struct Character {
    float ax, ay; // Advance X and Y
    float bw, bh; // Bitmap width and height
    float bl, bt; // Bitmap left and top (bearing)
    float tx, ty; // Texture coordinates (x, y) offset in atlas
};

struct FontAtlas {
    GLuint textureID;
    int width, height;  //
    int base_font_size; // fontSize
    float fontAscender;  // face->size->metrics.ascender
    float fontDescender; // face->size->metrics.descender
    float fontLineHeight;// face->size->metrics.height
    std::map<char, Character> chars;
};

enum class TextAlign {
    LEFT, CENTER, RIGHT,
    TOP, MIDDLE, BOTTOM, BASELINE
};

struct TextLayout {
    glm::vec2 offset; // How much to shift the 'pos' passed to DrawText
    glm::vec2 size;   // The total logical size of the text block
};

FontAtlas CreateFontAtlas(FT_Library& lib, FT_Face& face, int fontSize);
GLuint CompileShader(const char *vert_src, const char *frag_src);
void InitializeShader();
void InitializeFreeTypeLib(FT_Library& lib);
void InitializeFontAtlas(FT_Library& lib, FontAtlas& atlas, int size, std::string path = "C:\\Windows\\Fonts\\arial.ttf");
void CleanupFreeTypeFace(FT_Face& face);
void CleanupFreeTypeLib(FT_Library& lib);
TextLayout CalculateLayout(const std::string& text, const FontAtlas& atlas, 
                           float targetHeight, TextAlign hAlign, TextAlign vAlign);

// Full version
void RenderText(const std::string& text, float x, float y, float targetHeight,
                const FontAtlas& atlas, GLuint shader, RGB colour,
                const glm::mat4& projection);

// Convenience overload — delegates with identity matrix
void RenderText(const std::string& text, float x, float y, float targetHeight,
                const FontAtlas& atlas, GLuint shader, RGB colour);

glm::vec2 MeasureText(const std::string& text, const FontAtlas& atlas, float targetHeight);

} // namespace signal_ui