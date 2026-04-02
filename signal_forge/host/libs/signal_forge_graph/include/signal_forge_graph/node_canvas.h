// node_canvas.h
#pragma once
#include <GL/glew.h>
#include <wx/glcanvas.h>
#include <wx/dcclient.h>
#include <glm/glm.hpp>
#include "graph.h"
#include "renderer.h"
#include <ft2build.h>
#include FT_FREETYPE_H

wxDECLARE_EVENT(EVT_NODE_SELECTED, wxCommandEvent);
wxDECLARE_EVENT(EVT_NODE_PROPERTY_CHANGED, wxCommandEvent);

namespace signal_forge {

class NodeCanvas : public wxGLCanvas {
public:
    NodeCanvas(wxWindow *parent, Graph *graph);
    ~NodeCanvas() {}

private:
    wxGLContext *m_context = nullptr;
    Renderer     m_renderer;
    bool         m_glInitialized = false;
    Graph       *m_graph;

    // ── Camera ───────────────────────────────────────────────────────────────
    float m_pan_x = 0.0f;
    float m_pan_y = 0.0f;
    float m_zoom  = 1.0f;

    // ── Interaction state ────────────────────────────────────────────────────
    int     m_selected_node    = -1;
    int     m_dragging_node    = -1;
    int     m_editing_node     = -1;
    glm::vec2 m_drag_offset    = {0.0f, 0.0f}; // cursor offset within the node
    int     m_linking_from_pin = -1; // -1 means not linking
    bool    m_panning          = false;
    wxPoint m_last_mouse;
    wxTextCtrl* m_editor;

    // -- Events ---------------------------------------------------------
    void OnPaint(wxPaintEvent &evt);
    void OnSize(wxSizeEvent &evt);
    void OnMouseLeftDown(wxMouseEvent &evt);
    void OnMouseDoubleLeftDown(wxMouseEvent &evt);
    void OnMouseRightDown(wxMouseEvent &evt);
    void OnMouseMove(wxMouseEvent &evt);
    void OnMouseUp(wxMouseEvent &evt);
    void OnMouseWheel(wxMouseEvent &evt);
    void OnKeyDown(wxKeyEvent &evt);

    // -- Edit -----------------------------------------------------------
    void CommitEdit();

    // ── Coordinate transforms ────────────────────────────────────────────────
    glm::vec2 ScreenToWorld(wxPoint screen) const;
    wxPoint   WorldToScreen(glm::vec2 world) const;

    // ── Helpers ──────────────────────────────────────────────────────────────
    void InitGL();
    glm::mat4 BuildProjection() const;

    // Hit-test helpers; return node/pin id or -1 if nothing hit.
    int HitTestNode(glm::vec2 world) const;
    int HitTestOutputPin(glm::vec2 world) const;
    int HitTestInputPin(glm::vec2 world) const;

    wxWindowIDRef ID_NODE_DELETE;
    wxWindowIDRef ID_PROBE_ADD;
    wxWindowIDRef ID_INJECT_ADD;
    wxWindowIDRef ID_CANVAS_RESET;
};

} // namespace signal_forge