#include "signal_ui/text.h"

namespace signal_ui {
// ===========================================================================
// FreeType initialization
// ===========================================================================
FontAtlas CreateFontAtlas(FT_Library& lib, FT_Face& face, int fontSize) {
    FontAtlas atlas;
    atlas.width = 1024;  // SDFs often need more space due to padding
    atlas.height = 1024;
    atlas.base_font_size = fontSize;
    
    // 1. Set a large pixel size for better SDF precision
    FT_Set_Pixel_Sizes(face, 0, fontSize);
    atlas.fontAscender   = (float)(face->size->metrics.ascender >> 6);
    atlas.fontDescender  = (float)(face->size->metrics.descender >> 6);
    atlas.fontLineHeight = (float)(face->size->metrics.height >> 6);
    
    // 2. Configure the SDF spread (distance from edge in pixels)
    // This defines how "blurry" the field can be. 8 is standard.
    FT_Int spread = 8;
    FT_Property_Set(lib, "sdf", "spread", &spread);

    std::vector<unsigned char> bitmapBuffer(atlas.width * atlas.height, 0);

    int currX = spread; 
    int currY = spread;
    int maxHeight = 0;

    for (unsigned char i = 32; i < 128; i++) {
        // FT_LOAD_DEFAULT loads the outline
        if (FT_Load_Char(face, i, FT_LOAD_DEFAULT)) continue;

        // Render using the SDF module
        if (FT_Render_Glyph(face->glyph, FT_RENDER_MODE_SDF)) continue;

        FT_GlyphSlot g = face->glyph;

        // Check for atlas overflow (shelf packing)
        if (currX + g->bitmap.width + spread >= atlas.width) {
            currX = spread;
            currY += maxHeight + (spread * 2);
            maxHeight = 0;
        }

        // Copy the SDF bitmap into our atlas
        for (unsigned int row = 0; row < g->bitmap.rows; ++row) {
            for (unsigned int col = 0; col < g->bitmap.width; ++col) {
                int x = currX + col;
                int y = currY + row;
                bitmapBuffer[y * atlas.width + x] = g->bitmap.buffer[row * g->bitmap.pitch + col];
            }
        }

        // Save character metrics
        // IMPORTANT: The SDF bitmap is larger than the original glyph because of the spread.
        // We store the UVs and the size including the spread.
        Character ch = {
            (float)(g->advance.x >> 6), (float)(g->advance.y >> 6),
            (float)g->bitmap.width,      (float)g->bitmap.rows,
            (float)g->bitmap_left,       (float)g->bitmap_top,
            (float)currX / atlas.width,  (float)currY / atlas.height
        };
        atlas.chars[i] = ch;

        currX += g->bitmap.width + (spread * 2);
        maxHeight = std::max(maxHeight, (int)g->bitmap.rows);
    }

    // OpenGL Texture setup
    glGenTextures(1, &atlas.textureID);
    glBindTexture(GL_TEXTURE_2D, atlas.textureID);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, atlas.width, atlas.height, 0, GL_RED, GL_UNSIGNED_BYTE, bitmapBuffer.data());

    // CRITICAL: SDF requires Linear filtering to interpolate distances
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    return atlas;
}

GLuint CompileShader(const char *vert_src, const char *frag_src)
{
    auto compile = [](GLenum type, const char *src) -> GLuint {
        GLuint shader = glCreateShader(type);
        glShaderSource(shader, 1, &src, nullptr);
        glCompileShader(shader);

        GLint ok = GL_FALSE;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[1024];
            glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
            // In production, surface this through a proper logging system.
            (void)log;
        }
        return shader;
    };

    GLuint vs   = compile(GL_VERTEX_SHADER,   vert_src);
    GLuint fs   = compile(GL_FRAGMENT_SHADER, frag_src);
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);

    GLint ok = GL_FALSE;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        (void)log;
    }

    glDeleteShader(vs);
    glDeleteShader(fs);
    return prog;
}

void InitializeFreeTypeLib(FT_Library& lib) {
    FT_Init_FreeType(&lib);
}

void InitializeFontAtlas(FT_Library& lib, FontAtlas& atlas, int size, std::string path) {
    FT_Face face;
    FT_New_Face(lib, path.c_str(), 0, &face);
    atlas = CreateFontAtlas(lib, face, size);
    FT_Done_Face(face);
}

void CleanupFreeTypeFace(FT_Face& face)
{
    if (face) {
        FT_Done_Face(face);
        face = nullptr;
    }
}
void CleanupFreeTypeLib(FT_Library& lib)
{
    if (lib) {
        FT_Done_FreeType(lib);
        lib = nullptr;
    }
}

TextLayout CalculateLayout(const std::string& text, const FontAtlas& atlas, 
                           float targetHeight, TextAlign hAlign, TextAlign vAlign) 
{
    float scale = targetHeight / (float)atlas.base_font_size;
    
    // 1. Calculate Width (using advance)
    float width = 0;
    for (char c : text) {
        if (atlas.chars.count(c)) width += atlas.chars.at(c).ax * scale;
    }

    // 2. Use Font Globals for Vertical sizing (Important for visual consistency!)
    // These should be stored in your FontAtlas when you call FreeType
    float ascender = atlas.fontAscender * scale;
    float descender = atlas.fontDescender * scale; // Usually negative
    float lineHeight = ascender + fabs(descender);

    glm::vec2 offset(0.0f);

    // Horizontal Alignment
    if (hAlign == TextAlign::CENTER) offset.x = -width / 2.0f;
    else if (hAlign == TextAlign::RIGHT) offset.x = -width;
    else if (hAlign == TextAlign::LEFT)  offset.x = 0.0f;

    // Vertical Alignment
    if (vAlign == TextAlign::TOP)             offset.y = ascender;
    else if (vAlign == TextAlign::MIDDLE)     offset.y = (ascender + descender) / 2.0f;
    else if (vAlign == TextAlign::BOTTOM)     offset.y = descender;
    else if (vAlign == TextAlign::BASELINE)   offset.y = 0;

    return { offset, glm::vec2(width, lineHeight) };
}

void RenderText(const std::string& text, float x, float y, float targetHeight,
                const FontAtlas& atlas, const GLuint shader, RGB colour, 
                const glm::mat4& projection)
{
    if (text.empty()) return;

    float atlasBaseSize = (float)atlas.base_font_size; 
    float scale = targetHeight / atlasBaseSize;
    
    std::vector<float> vertices;
    float penX = x;
    float penY = y;
    
    for (size_t i = 0; i < text.length(); ++i) {
        char c = text[i];
        
        auto it = atlas.chars.find(c);
        if (it == atlas.chars.end()) continue;
        
        const Character& ch = it->second;

        // bl = bearing left, bt = bearing top, bw = width, bh = height
        float x0 = penX + (ch.bl * scale);
        float y0 = penY - (ch.bt * scale); 
        float x1 = x0 + (ch.bw * scale);
        float y1 = y0 + (ch.bh * scale);

        // UV coordinates remain the same (normalized 0.0 to 1.0)
        float s0 = ch.tx;
        float t0 = ch.ty;
        float s1 = ch.tx + (ch.bw / (float)atlas.width);
        float t1 = ch.ty + (ch.bh / (float)atlas.height);
        
        vertices.insert(vertices.end(), {
            x0, y0, s0, t0,
            x0, y1, s0, t1,
            x1, y1, s1, t1,
            
            x0, y0, s0, t0,
            x1, y1, s1, t1,
            x1, y0, s1, t0
        });
        
        // 3. Scale the advance
        penX += ch.ax * scale;
        penY += ch.ay * scale;
    }
    
    if (vertices.empty()) return;
    
    // VAO/VBO logic...
    unsigned int vao, vbo;
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(float), vertices.data(), GL_STREAM_DRAW);
    
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    
    glUseProgram(shader);

    GLint modelLoc = glGetUniformLocation(shader, "model");
    if (modelLoc != -1) {
        glm::mat4 model = glm::mat4(1.0f);
        glUniformMatrix4fv(modelLoc, 1, GL_FALSE, glm::value_ptr(model));
    }

    glUniformMatrix4fv(glGetUniformLocation(shader, "projection"), 1, GL_FALSE, glm::value_ptr(projection));
    glUniform4f(glGetUniformLocation(shader, "textColor"), colour.r, colour.g, colour.b, 1.0f);
    
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, atlas.textureID);
    glUniform1i(glGetUniformLocation(shader, "text"), 0);
    
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    
    // vertices.size() / 4 because each vertex has 4 floats (x, y, u, v)
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(vertices.size() / 4));
    
    // Cleanup
    glBindVertexArray(0);
    glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(1, &vbo);
}

void RenderText(const std::string& text, float x, float y, float targetHeight,
                const FontAtlas& atlas, GLuint shader, RGB colour) {
    RenderText(text, x, y, targetHeight, atlas, shader, colour, glm::mat4(1.0f));
}

glm::vec2 MeasureText(const std::string& text, const FontAtlas& atlas, float targetHeight)
{
    if (text.empty()) return glm::vec2(0.0f);

    // 1. Calculate the same scale factor used in DrawText
    float baseFontSize = (float)atlas.base_font_size; 
    float scale = targetHeight / baseFontSize;

    float totalWidth = 0.0f;
    
    for (char c : text) {
        auto it = atlas.chars.find(c);
        if (it != atlas.chars.end()) {
            // 2. Scale the horizontal advance
            totalWidth += it->second.ax * scale;
        }
    }
    
    // Width is the scaled accumulated advance.
    // Height is the targetHeight you requested.
    return glm::vec2(totalWidth, targetHeight);
}

} // namespace signal_ui