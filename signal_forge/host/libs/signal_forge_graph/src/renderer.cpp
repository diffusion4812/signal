// renderer.cpp
#include "signal_forge_graph/renderer.h"
#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace signal_forge {

using namespace layout;

// ─────────────────────────────────────────────────────────────────────────────
// Internal helpers
// ─────────────────────────────────────────────────────────────────────────────

static void TesselateCubicBezier(std::vector<glm::vec2> &out,
                                  glm::vec2 p0, glm::vec2 p1,
                                  glm::vec2 p2, glm::vec2 p3,
                                  int segments = 64)
{
    out.resize(segments + 1);
    for (int i = 0; i <= segments; ++i) {
        float t = static_cast<float>(i) / segments;
        float u = 1.0f - t;
        out[i] = u*u*u*p0
               + 3.0f*u*u*t*p1
               + 3.0f*u*t*t*p2
               +     t*t*t*p3;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// GetPinWorldPos — must stay consistent with DrawNode pin layout
// ─────────────────────────────────────────────────────────────────────────────

glm::vec2 Renderer::GetPinWorldPos(const Node &node, int pin_id)
{
    float y = node.y + PIN_Y_START;
    for (const auto &pin : node.inputs) {
        if (pin.id == pin_id) return {node.x, y};
        y += PIN_SPACING;
    }
    y = node.y + PIN_Y_START;
    for (const auto &pin : node.outputs) {
        if (pin.id == pin_id) return {node.x + NODE_WIDTH, y};
        y += PIN_SPACING;
    }
    return {node.x, node.y}; // fallback — should not happen
}

// ─────────────────────────────────────────────────────────────────────────────
// SetFlat — bind shader and upload the three standard uniforms
// ─────────────────────────────────────────────────────────────────────────────

void Renderer::SetFlat(const glm::mat4 &proj, const glm::mat4 &model,
                       const glm::vec4 &color)
{
    glUseProgram(m_flat_shader);
    glUniformMatrix4fv(glGetUniformLocation(m_flat_shader, "uProj"),
                       1, GL_FALSE, glm::value_ptr(proj));
    glUniformMatrix4fv(glGetUniformLocation(m_flat_shader, "uModel"),
                       1, GL_FALSE, glm::value_ptr(model));
    glUniform4fv(glGetUniformLocation(m_flat_shader, "uColor"),
                 1, glm::value_ptr(color));
}

// ─────────────────────────────────────────────────────────────────────────────
// UploadAndDrawLineStrip — upload point data to the dynamic line VBO and draw
// ─────────────────────────────────────────────────────────────────────────────

void Renderer::UploadAndDrawLineStrip(const std::vector<glm::vec2> &pts)
{
    glBindVertexArray(m_line_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_line_vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(pts.size() * sizeof(glm::vec2)),
                 pts.data(), GL_DYNAMIC_DRAW);
    glDrawArrays(GL_LINE_STRIP, 0, static_cast<GLsizei>(pts.size()));
    glBindVertexArray(0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Init
// ─────────────────────────────────────────────────────────────────────────────

void Renderer::Init()
{
    const char *vert_src =
        "#version 330 core\n"
        "layout(location = 0) in vec2 aPos;\n"
        "uniform mat4 uProj;\n"
        "uniform mat4 uModel;\n"
        "void main() {\n"
        "    gl_Position = uProj * uModel * vec4(aPos, 0.0, 1.0);\n"
        "}\n";

    const char *frag_src =
        "#version 330 core\n"
        "out vec4 FragColor;\n"
        "uniform vec4 uColor;\n"
        "void main() {\n"
        "    FragColor = uColor;\n"
        "}\n";

    m_flat_shader = signal_ui::CompileShader(vert_src, frag_src);
    m_text_shader = signal_ui::CompileShader(signal_ui::textVertexShader, signal_ui::textFragmentShader);

    signal_ui::InitializeFreeTypeLib(m_ft_library);
    signal_ui::InitializeFontAtlas(m_ft_library, m_label_atlas, 64);

    InitQuad();
    InitLine();
    InitCircle();
}

// ─────────────────────────────────────────────────────────────────────────────
// InitQuad — unit square [0,1]×[0,1] for GL_TRIANGLE_STRIP (4 vertices)
// ─────────────────────────────────────────────────────────────────────────────

void Renderer::InitQuad()
{
    // Vertices laid out as a triangle strip:
    //   (0,0) → (1,0) → (0,1) → (1,1)
    // Pair with a model matrix to position / scale the quad in world space.
    static const float verts[] = {
        0.0f, 0.0f,
        1.0f, 0.0f,
        0.0f, 1.0f,
        1.0f, 1.0f,
    };

    glGenVertexArrays(1, &m_quad_vao);
    glGenBuffers(1, &m_quad_vbo);
    glBindVertexArray(m_quad_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_quad_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glBindVertexArray(0);
}

// ─────────────────────────────────────────────────────────────────────────────
// InitLine — dynamic VBO for bezier curves, pending links, grid lines, etc.
// ─────────────────────────────────────────────────────────────────────────────

void Renderer::InitLine()
{
    glGenVertexArrays(1, &m_line_vao);
    glGenBuffers(1, &m_line_vbo);
    glBindVertexArray(m_line_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_line_vbo);
    // Pre-allocate; data is re-uploaded every frame with GL_DYNAMIC_DRAW.
    glBufferData(GL_ARRAY_BUFFER, 8192 * sizeof(glm::vec2), nullptr,
                 GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glBindVertexArray(0);
}

// ─────────────────────────────────────────────────────────────────────────────
// InitCircle — unit circle centred at (0,0), radius 1, as a GL_TRIANGLE_FAN
// ─────────────────────────────────────────────────────────────────────────────

void Renderer::InitCircle()
{
    const int N = 32; // segments
    // Fan: vertex 0 = centre; vertices 1..N+1 go around the perimeter;
    //      vertex N+1 == vertex 1 to close the fan.
    m_circle_vertex_count = N + 2;
    std::vector<glm::vec2> verts(m_circle_vertex_count);
    verts[0] = {0.0f, 0.0f};
    for (int i = 0; i <= N; ++i) {
        float a = 2.0f * static_cast<float>(M_PI) * i / N;
        verts[i + 1] = {std::cos(a), std::sin(a)};
    }

    glGenVertexArrays(1, &m_circle_vao);
    glGenBuffers(1, &m_circle_vbo);
    glBindVertexArray(m_circle_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_circle_vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(verts.size() * sizeof(glm::vec2)),
                 verts.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glBindVertexArray(0);
}

// ─────────────────────────────────────────────────────────────────────────────
// DrawGrid
// ─────────────────────────────────────────────────────────────────────────────

void Renderer::DrawGrid(const glm::mat4 &proj, float zoom)
{
    // Extract visible world bounds from the ortho projection matrix.
    // For glm::ortho(l, r, b, t):
    //   proj[0][0] = 2/(r-l)   proj[3][0] = -(r+l)/(r-l)
    //   proj[1][1] = 2/(t-b)   proj[3][1] = -(t+b)/(t-b)
    // In our BuildProjection: bottom = cy+hh, top = cy-hh  (y flipped)
    //   => proj[1][1] is negative.
    float half_w  =  1.0f / proj[0][0];
    float half_h  = -1.0f / proj[1][1];  // positive (proj[1][1] < 0)
    float world_cx = -proj[3][0] / proj[0][0];
    float world_cy = -proj[3][1] / proj[1][1];

    float x0 = world_cx - half_w;
    float x1 = world_cx + half_w;
    float y0 = world_cy - half_h;
    float y1 = world_cy + half_h;

    // Pick a grid step that stays visually comfortable regardless of zoom.
    float step = 50.0f;
    if      (zoom < 0.2f) step = 500.0f;
    else if (zoom < 0.5f) step = 200.0f;
    else if (zoom < 1.0f) step = 100.0f;
    else if (zoom > 5.0f) step = 10.0f;

    std::vector<glm::vec2> lines;
    lines.reserve(512);

    // Vertical lines
    float xs = std::floor(x0 / step) * step;
    for (float x = xs; x <= x1 + step; x += step) {
        lines.push_back({x, y0});
        lines.push_back({x, y1});
    }

    // Horizontal lines
    float ys = std::floor(y0 / step) * step;
    for (float y = ys; y <= y1 + step; y += step) {
        lines.push_back({x0, y});
        lines.push_back({x1, y});
    }

    if (lines.empty()) return;

    glm::mat4 identity(1.0f);
    SetFlat(proj, identity, {0.72f, 0.72f, 0.72f, 1.0f});
    glLineWidth(1.0f);

    // Upload all lines at once; use GL_LINES (pairs of vertices).
    glBindVertexArray(m_line_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_line_vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(lines.size() * sizeof(glm::vec2)),
                 lines.data(), GL_DYNAMIC_DRAW);
    glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(lines.size()));
    glBindVertexArray(0);
}

// ─────────────────────────────────────────────────────────────────────────────
// DrawCircle
// ─────────────────────────────────────────────────────────────────────────────

void Renderer::DrawCircle(const glm::mat4 &proj, glm::vec2 center,
                          float radius, glm::vec4 color)
{
    glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(center, 0.0f));
    model = glm::scale(model, glm::vec3(radius, radius, 1.0f));

    SetFlat(proj, model, color);
    glBindVertexArray(m_circle_vao);
    glDrawArrays(GL_TRIANGLE_FAN, 0, m_circle_vertex_count);
    glBindVertexArray(0);
}

// ─────────────────────────────────────────────────────────────────────────────
// DrawNode
// ─────────────────────────────────────────────────────────────────────────────

void Renderer::DrawNode(const glm::mat4 &proj, const Node &node, bool selected) {
    if (const auto* probe = dynamic_cast<const ProbeNode*>(&node)) {
        DrawProbeNode(proj, *probe, selected);
    } else if (const auto* inject = dynamic_cast<const InjectNode*>(&node)) {
        DrawInjectNode(proj, *inject, selected);
    } else {
        DrawStandardNode(proj, node, selected, {0.45f, 0.18f, 0.18f, 1.0f});
    }
}

void Renderer::DrawStandardNode(const glm::mat4 &proj, const Node &node, bool selected, glm::vec4 title_color) {
    // ── Body ─────────────────────────────────────────────────────────────────
    {
        glm::mat4 model = glm::translate(glm::mat4(1.0f),
                              glm::vec3(node.x, node.y, 0.0f));
        model = glm::scale(model, glm::vec3(NODE_WIDTH, NODE_HEIGHT, 1.0f));

        SetFlat(proj, model, {0.22f, 0.22f, 0.25f, 1.0f});
        glBindVertexArray(m_quad_vao);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glBindVertexArray(0);
    }

    // ── Title bar ────────────────────────────────────────────────────────────
    {
        glm::mat4 model = glm::translate(glm::mat4(1.0f),
                              glm::vec3(node.x, node.y, 0.0f));
        model = glm::scale(model, glm::vec3(NODE_WIDTH, TITLE_HEIGHT, 1.0f));
        SetFlat(proj, model, title_color);
        glBindVertexArray(m_quad_vao);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glBindVertexArray(0);

        // ── Instance label in title bar ──────────────────────────────────────────
        DrawText(proj, node.instance,
                {node.x + NODE_WIDTH / 2.0f, node.y + TITLE_HEIGHT / 2.0f},
                20.0f, {1.0f, 1.0f, 1.0f, 1.0f},
                signal_ui::TextAlign::CENTER,
                signal_ui::TextAlign::MIDDLE);
    }

    // ── Outline (if selected) -──────────────────────────────────────────────-
    {
        if (selected) {
            glm::mat4 model = glm::translate(glm::mat4(1.0f),
                                glm::vec3(node.x, node.y, 0.0f));
            model = glm::scale(model, glm::vec3(NODE_WIDTH, NODE_HEIGHT, 1.0f));

            SetFlat(proj, model, {1.0f, 0.7f, 0.0f, 1.0f});
            glBindVertexArray(m_quad_vao);
            glLineWidth(3.0f);
            GLuint lineIndices[] = { 0, 1, 3, 2 };
            glDrawElements(GL_LINE_LOOP, 4, GL_UNSIGNED_INT, lineIndices);
            glPointSize(3.0f);
            glDrawElements(GL_POINTS, 4, GL_UNSIGNED_INT, lineIndices);
            glBindVertexArray(0);
        }
    }

    // ── Input pins ───────────────────────────────────────────────────────────
    {
        float y_off = PIN_Y_START;
        for (const auto &pin : node.inputs) {
            glm::vec2 pos(node.x, node.y + y_off);
            DrawCircle(proj, pos, PIN_RADIUS, {0.35f, 0.75f, 0.35f, 1.0f});
            DrawText(proj, pin.name,
                     {pos.x + PIN_RADIUS + 4.0f, pos.y},
                     16.0f, {0.9f, 0.9f, 0.9f, 1.0f},
                     signal_ui::TextAlign::LEFT,
                     signal_ui::TextAlign::MIDDLE);
            y_off += PIN_SPACING;
        }
    }

    // ── Output pins ──────────────────────────────────────────────────────────
    {
        float y_off = PIN_Y_START;
        for (const auto &pin : node.outputs) {
            glm::vec2 pos(node.x + NODE_WIDTH, node.y + y_off);
            DrawCircle(proj, pos, PIN_RADIUS, {0.35f, 0.35f, 0.75f, 1.0f});
            // Label drawn right-aligned (shift left by estimated text width)
            glm::vec2 size = signal_ui::MeasureText(pin.name, m_label_atlas, 16.0f);
            DrawText(proj, pin.name,
                     {pos.x - PIN_RADIUS - 4.0f, pos.y},
                     16.0f, {0.9f, 0.9f, 0.9f, 1.0f},
                     signal_ui::TextAlign::RIGHT,
                     signal_ui::TextAlign::MIDDLE);
            y_off += PIN_SPACING;
        }
    }
}

void Renderer::DrawProbeNode(const glm::mat4 &proj, const ProbeNode &node, bool selected) {
    DrawStandardNode(proj, node, selected, {0.45f, 0.18f, 0.18f, 1.0f});

    std::string label = std::to_string(node.value);
    DrawText(proj, label,
             {node.x + layout::NODE_WIDTH / 2.0f,
              node.y + layout::TITLE_HEIGHT + 12.0f},
             14.0f, {0.4f, 1.0f, 0.4f, 1.0f},
             signal_ui::TextAlign::CENTER,
             signal_ui::TextAlign::MIDDLE);
}

void Renderer::DrawInjectNode(const glm::mat4 &proj, const InjectNode &node, bool selected) {
    glm::vec4 title_color = {0.45f, 0.18f, 0.18f, 1.0f};
    if (node.forcing_active) {
        title_color = {0.7f, 0.4f, 0.1f, 1.0f};
    }
    DrawStandardNode(proj, node, selected, title_color);

    // Draw forced value
    std::string label = std::to_string(node.forced_value);
    DrawText(proj, label,
             {node.x + layout::NODE_WIDTH / 2.0f,
              node.y + layout::TITLE_HEIGHT + 12.0f},
             14.0f, {1.0f, 0.4f, 0.4f, 1.0f},
             signal_ui::TextAlign::CENTER,
             signal_ui::TextAlign::MIDDLE);
}

// ─────────────────────────────────────────────────────────────────────────────
// DrawBezierLink
// ─────────────────────────────────────────────────────────────────────────────

void Renderer::DrawBezierLink(const glm::mat4 &proj, const Link &link,
                               const Graph &graph)
{
    const Node &from_node = graph.GetNodeForPin(link.from_pin);
    const Node &to_node   = graph.GetNodeForPin(link.to_pin);
    glm::vec2 p0 = GetPinWorldPos(from_node, link.from_pin);
    glm::vec2 p3 = GetPinWorldPos(to_node,   link.to_pin);

    float dx = glm::abs(p3.x - p0.x) * 0.5f;
    glm::vec2 p1 = p0 + glm::vec2(dx, 0.0f);
    glm::vec2 p2 = p3 - glm::vec2(dx, 0.0f);

    std::vector<glm::vec2> pts;
    TesselateCubicBezier(pts, p0, p1, p2, p3);

    glm::mat4 identity(1.0f);
    SetFlat(proj, identity, {0.85f, 0.82f, 0.28f, 1.0f});
    glLineWidth(2.0f);
    UploadAndDrawLineStrip(pts);
}

// ─────────────────────────────────────────────────────────────────────────────
// DrawBezierPending — in-progress link while dragging from a pin
// ─────────────────────────────────────────────────────────────────────────────

void Renderer::DrawBezierPending(const glm::mat4 &proj,
                                  glm::vec2 from, glm::vec2 to)
{
    float dx = glm::abs(to.x - from.x) * 0.5f;
    glm::vec2 p1 = from + glm::vec2(dx, 0.0f);
    glm::vec2 p2 = to   - glm::vec2(dx, 0.0f);

    std::vector<glm::vec2> pts;
    TesselateCubicBezier(pts, from, p1, p2, to);

    glm::mat4 identity(1.0f);
    SetFlat(proj, identity, {0.85f, 0.55f, 0.2f, 0.85f});
    glLineWidth(2.0f);
    UploadAndDrawLineStrip(pts);
}

void Renderer::DrawText(const glm::mat4 &proj, const std::string &text,
                        glm::vec2 pos, float targetHeight, glm::vec4 color,
                        signal_ui::TextAlign hAlign,
                        signal_ui::TextAlign vAlign)
{
    if (text.empty()) return;

    // 1. Get the layout offset
    signal_ui::TextLayout layout = signal_ui::CalculateLayout(text, m_label_atlas, targetHeight, hAlign, vAlign);
    
    // 2. Apply the offset to the starting position
    glm::vec2 alignedPos = pos + layout.offset;

    // 3. Render
    signal_ui::RenderText(text, alignedPos.x, alignedPos.y, targetHeight, 
                          m_label_atlas, m_text_shader, signal_ui::RGB{color.r, color.g, color.b}, proj);
}

} // namespace signal_forge
