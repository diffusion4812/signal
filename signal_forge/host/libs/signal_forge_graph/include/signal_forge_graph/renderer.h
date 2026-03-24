// renderer.h
#pragma once
#include <GL/glew.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <string>
#include <vector>
#include "signal_forge_graph/graph.h"
#include "signal_ui/text.h"

namespace signal_forge {

// ─── Layout constants shared between Renderer and NodeCanvas ─────────────────
// Keep these in one place so hit-testing and drawing stay in sync.
namespace layout {
    constexpr float NODE_WIDTH    = 160.0f;
    constexpr float NODE_HEIGHT   = 90.0f;
    constexpr float TITLE_HEIGHT  = 28.0f;
    constexpr float PIN_Y_START   = 40.0f;
    constexpr float PIN_SPACING   = 22.0f;
    constexpr float PIN_RADIUS    = 5.0f;
    constexpr float PIN_HIT_RADIUS = 10.0f; // world-space pick radius for pins
}

class Renderer {
public:
    void Init();
    void DrawGrid(const glm::mat4 &proj, float zoom);

    // Main dispatch — calls Accept which calls the correct overload below
    void DrawNode(const glm::mat4 &proj, const Node &node, bool selected);

    // Per-type drawing — called via Accept, also usable directly
    void DrawStandardNode(const glm::mat4 &proj, const Node &node, bool selected, glm::vec4 title_color);
    void DrawProbeNode   (const glm::mat4 &proj, const ProbeNode &node, bool selected);
    void DrawInjectNode  (const glm::mat4 &proj, const InjectNode &node, bool selected);
    
    void DrawBezierLink(const glm::mat4 &proj, const Link &link,
                        const Graph &graph);
    void DrawBezierPending(const glm::mat4 &proj, glm::vec2 from,
                           glm::vec2 to);
    void DrawCircle(const glm::mat4 &proj, glm::vec2 center,
                    float radius, glm::vec4 color);
    void DrawText(const glm::mat4 &proj, const std::string &text,
                        glm::vec2 pos, float targetHeight, glm::vec4 color,
                        signal_ui::TextAlign hAlign,
                        signal_ui::TextAlign vAlign);

    // Compute the world-space centre of a pin; must stay consistent with DrawNode.
    static glm::vec2 GetPinWorldPos(const Node &node, int pin_id);

private:
    GLuint m_quad_vao   = 0, m_quad_vbo   = 0;
    GLuint m_line_vao   = 0, m_line_vbo   = 0;
    GLuint m_circle_vao = 0, m_circle_vbo = 0;
    int    m_circle_vertex_count = 0;
    GLuint m_flat_shader = 0;
    GLuint m_text_shader = 0;

    FT_Library m_ft_library = 0;
    signal_ui::FontAtlas m_label_atlas;

    void InitQuad();
    void InitLine();
    void InitCircle();

    // Convenience: bind flat shader and set all three uniforms in one call.
    void SetFlat(const glm::mat4 &proj, const glm::mat4 &model,
                 const glm::vec4 &color);

    // Upload arbitrary line-strip data and draw it (does NOT set shader/uniforms).
    void UploadAndDrawLineStrip(const std::vector<glm::vec2> &pts);
};

} // namespace signal_forge