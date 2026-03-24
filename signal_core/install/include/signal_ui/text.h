#include <vector>
#include <map>
#include <GL/glew.h>
#include <GL/gl.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <wx/wx.h>
#include <wx/window.h>

namespace signal_ui {

// Vertex shader for text rendering
static const char* textVertexShader = R"(
#version 330 core
layout (location = 0) in vec2 aPos;
layout (location = 1) in vec2 aTexCoord;

out vec2 TexCoord;

uniform mat4 projection;

void main() {
    gl_Position = projection * vec4(aPos, 0.0, 1.0);
    TexCoord = aTexCoord;
}
)";

// Fragment shader for text rendering
static const char* textFragmentShader = R"(
#version 330 core
in vec2 TexCoord;
out vec4 FragColor;

uniform sampler2D text;
uniform vec4 textColor;

void main() {
    float alpha = texture(text, TexCoord).r;
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
    int width, height;
    std::map<char, Character> chars;
};

GLuint textShader_;

FT_Library ftLibrary_;
FT_Face    labelFont_;
FT_Face    titleFont_;

FontAtlas  labelAtlas_;
FontAtlas  titleAtlas_;

FontAtlas CreateFontAtlas(FT_Face face, int fontSize);
GLuint CompileShader(const char *vert_src, const char *frag_src);
void InitializeShader();
void InitializeFreeType();
void CleanupFreeTypeFace(FT_Face& face);
void CleanupFreeTypeLib(FT_Library& lib);
void Cleanup();
void RenderText(const wxString& text, float x, float y,
                const FontAtlas& atlas, RGB colour);
float MeasureText(const wxString& text, const FontAtlas& atlas);

} // namespace signal_ui