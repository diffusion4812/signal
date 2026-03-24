// node_canvas.cpp
#include "signal_forge_graph/node_canvas.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>

wxDEFINE_EVENT(EVT_NODE_SELECTED,         wxCommandEvent);
wxDEFINE_EVENT(EVT_NODE_PROPERTY_CHANGED, wxCommandEvent);

namespace signal_forge {

using namespace layout;

// ─────────────────────────────────────────────────────────────────────────────
// GL attributes — called once; the result is reused for every canvas.
// ─────────────────────────────────────────────────────────────────────────────

static const wxGLAttributes &GetGLAttribs()
{
    static wxGLAttributes attrs;
    attrs.PlatformDefaults()
         .RGBA()
         .DoubleBuffer()
         .Depth(16)
         .SampleBuffers(1).Samplers(4) // MSAA 4x
         .EndList();
    return attrs;
}

// ─────────────────────────────────────────────────────────────────────────────
// Constructor
// ─────────────────────────────────────────────────────────────────────────────

NodeCanvas::NodeCanvas(wxWindow *parent, Graph *graph)
    : wxGLCanvas(parent, GetGLAttribs())
    , m_graph(graph)
    , ID_NODE_DELETE(wxWindow::NewControlId())
    , ID_PROBE_ADD(wxWindow::NewControlId())
    , ID_INJECT_ADD(wxWindow::NewControlId())
    , ID_CANVAS_RESET(wxWindow::NewControlId())
{
    wxGLContextAttrs ctxAttrs;
    ctxAttrs.CoreProfile().OGLVersion(3, 3).EndList();
    m_context = new wxGLContext(this, nullptr, &ctxAttrs);

    // ── Paint ─────────────────────────────────────────────────────────────
    Bind(wxEVT_PAINT, &NodeCanvas::OnPaint, this);
    Bind(wxEVT_SIZE,  &NodeCanvas::OnSize,  this);

    // ── Mouse — left button: select / drag / link ──────────────────────────
    Bind(wxEVT_LEFT_DOWN,    &NodeCanvas::OnMouseLeftDown,       this);
    //Bind(wxEVT_LEFT_DCLICK,  &NodeCanvas::OnMouseDoubleLeftDown, this);
    Bind(wxEVT_RIGHT_DOWN,   &NodeCanvas::OnMouseRightDown,      this);
    Bind(wxEVT_MOTION,       &NodeCanvas::OnMouseMove,           this);
    Bind(wxEVT_LEFT_UP,      &NodeCanvas::OnMouseUp,             this);
    Bind(wxEVT_MOUSEWHEEL,   &NodeCanvas::OnMouseWheel,          this);

    // ── Mouse — middle button: pan ─────────────────────────────────────────
    Bind(wxEVT_MIDDLE_DOWN, [this](wxMouseEvent &e) {
        m_panning    = true;
        m_last_mouse = e.GetPosition();
        if (!HasCapture()) CaptureMouse();
    });
    Bind(wxEVT_MIDDLE_UP, [this](wxMouseEvent &) {
        m_panning = false;
        if (HasCapture()) ReleaseMouse();
    });

    // ── Keyboard ──────────────────────────────────────────────────────────
    Bind(wxEVT_KEY_DOWN, &NodeCanvas::OnKeyDown, this);

    // -- Node editing -----------------------------------------------
    //m_editor = new wxTextCtrl(this, wxID_ANY, "",
    //                          wxDefaultPosition, wxSize(1,1),
    //                          wxBORDER_NONE | wxTE_PROCESS_ENTER);
    //m_editor->Hide();
    //m_editor->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { CommitEdit(); });
    //m_editor->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent&)   { CommitEdit(); });

    SetFocus(); // Accept keyboard events immediately
}

// ─────────────────────────────────────────────────────────────────────────────
// InitGL — one-time OpenGL initialisation; called before the first paint.
// ─────────────────────────────────────────────────────────────────────────────

void NodeCanvas::InitGL()
{
    SetCurrent(*m_context);
    static bool s_initialized = false;
    if (!s_initialized) {
        glewExperimental = GL_TRUE;
        glewInit();

        m_renderer.Init(); // compile shaders, create VAOs/VBOs

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glEnable(GL_MULTISAMPLE);
        s_initialized = true;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// OnPaint
// ─────────────────────────────────────────────────────────────────────────────

void NodeCanvas::OnPaint(wxPaintEvent &)
{
    wxPaintDC dc(this); // Required even with GL
    InitGL();

    wxSize size = GetClientSize();
    glViewport(0, 0, size.x, size.y);
    glClearColor(0.18f, 0.18f, 0.20f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glm::mat4 proj = BuildProjection();

    // Grid
    m_renderer.DrawGrid(proj, m_zoom);

    // Links
    for (const auto &link : m_graph->Links())
        m_renderer.DrawBezierLink(proj, link, *m_graph);

    // Pending link being dragged
    if (m_linking_from_pin >= 0) {
        const Node &node = m_graph->GetNodeForPin(m_linking_from_pin);
        glm::vec2 from   = Renderer::GetPinWorldPos(node, m_linking_from_pin);
        glm::vec2 to     = ScreenToWorld(m_last_mouse);
        m_renderer.DrawBezierPending(proj, from, to);
    }

    // Nodes (draw on top of links)
    for (const auto &node : m_graph->Nodes()) {
        bool selected = (node->id == m_selected_node);
        m_renderer.DrawNode(proj, *node, selected);
    }

    SwapBuffers();
}

// ─────────────────────────────────────────────────────────────────────────────
// OnSize
// ─────────────────────────────────────────────────────────────────────────────

void NodeCanvas::OnSize(wxSizeEvent &evt)
{
    Refresh();
    evt.Skip();
}

// ─────────────────────────────────────────────────────────────────────────────
// OnMouseLeftDown — left click: start drag, linking, or deselect
// ─────────────────────────────────────────────────────────────────────────────

void NodeCanvas::OnMouseLeftDown(wxMouseEvent &e)
{
    SetFocus();
    glm::vec2 world = ScreenToWorld(e.GetPosition());
    m_last_mouse    = e.GetPosition();

    // Priority 1: did we click an output pin?  → begin linking.
    int hit_pin = HitTestOutputPin(world);
    if (hit_pin >= 0) {
        m_linking_from_pin = hit_pin;
        if (!HasCapture()) CaptureMouse();
        Refresh();
        return;
    }

    // Priority 2: did we click a node?  → select and begin drag.
    int hit_node = HitTestNode(world);
    if (hit_node >= 0) {
        m_selected_node = hit_node;
        m_dragging_node = hit_node;
        // Record cursor offset so the node doesn't "jump" to the mouse position.
        const Node *node = m_graph->FindNode(hit_node);
        if (node)
            m_drag_offset = world - glm::vec2(node->x, node->y);
            wxCommandEvent evt(EVT_NODE_SELECTED);
            evt.SetString(wxString::Format("%llu", static_cast<unsigned long long>(hit_node)));
            AddPendingEvent(evt);
        if (!HasCapture()) CaptureMouse();
        Refresh();
        return;
    }

    // Priority 3: clicked empty space → deselect.
    m_selected_node = -1;
    Refresh();
}

// ─────────────────────────────────────────────────────────────────────────────
// OnMouseDoubleLeftDown — left click: enable/disable value injection
// ─────────────────────────────────────────────────────────────────────────────

void NodeCanvas::OnMouseDoubleLeftDown(wxMouseEvent &e)
{
    SetFocus();
    glm::vec2 world = ScreenToWorld(e.GetPosition());
    m_last_mouse    = e.GetPosition();

    // Priority 1: did we click a node?  → select and begin drag.
    int hit_node = HitTestNode(world);
    if (hit_node >= 0) {
        // Record cursor offset so the node doesn't "jump" to the mouse position.
        Node *node = m_graph->FindNode(hit_node);
        if (node) {
            if (auto* inject = dynamic_cast<InjectNode*>(node)) {
                //inject->active = !inject->active;
                m_editing_node = hit_node;
                // Position and show editor (as in previous example)
                wxPoint screen   = ClientToScreen(e.GetPosition());
                wxPoint parentPt = GetParent()->ScreenToClient(screen);
                const int w = FromDIP(180, this), h = FromDIP(24, this);

                m_editor->SetSize(parentPt.x, parentPt.y, w, h);
                m_editor->SetValue(wxString::FromUTF8(std::to_string(inject->forced_value).c_str()));
                m_editor->Show();
                m_editor->Raise();
                m_editor->SetFocus();
                m_editor->SetSelection(-1, -1);
                return;
            }
        }
        if (!HasCapture()) CaptureMouse();
        Refresh();
        return;
    }

    // Priority 2: clicked empty space → deselect.
    m_selected_node = -1;
    Refresh();
}

// ─────────────────────────────────────────────────────────────────────────────
// OnMouseLeftDown — left click: start drag, linking, or deselect
// ─────────────────────────────────────────────────────────────────────────────

void NodeCanvas::OnMouseRightDown(wxMouseEvent &e)
{
    SetFocus();
    m_last_mouse = e.GetPosition();

    wxMenu menu;
    wxMenu* add_menu = new wxMenu;

    const auto& block_types = m_graph->AllBlockNames();

    // This vector holds the "Reservations". 
    // They stay reserved as long as this vector exists.
    struct DynamicItem {
        wxWindowIDRef id;
        std::string name;
    };
    auto dynamic_items = std::make_shared<std::vector<DynamicItem>>();

    for (const auto& name : block_types) {
        wxWindowIDRef new_id = wxWindow::NewControlId();
        add_menu->Append(new_id, name);
        dynamic_items->push_back({new_id, name});
    }

    menu.AppendSubMenu(add_menu,    "Add Node");
    menu.Append(ID_NODE_DELETE,     "Delete Node");
    menu.AppendSeparator();
    menu.Append(ID_PROBE_ADD,       "Add Probe");
    menu.Append(ID_INJECT_ADD,      "Add Inject");
    menu.AppendSeparator();
    menu.Append(ID_CANVAS_RESET,    "Reset View");

    // Capture 'dynamic_items' by value (shared_ptr) in the lambda.
    // This ensures the IDs remain "Reserved" until the lambda is destroyed.
    menu.Bind(wxEVT_MENU, [this, dynamic_items](wxCommandEvent& evt) {
        int clicked_id = evt.GetId();
        glm::vec2 world = ScreenToWorld(m_last_mouse);

        if (clicked_id == ID_PROBE_ADD) {
            m_graph->AddProbeNode(world.x, world.y);
        }
        else if (clicked_id == ID_INJECT_ADD) {
            m_graph->AddInjectNode(world.x, world.y);
        }
        else {
            // Check the dynamic list
            for (const auto& item : *dynamic_items) {
                if (item.id == clicked_id) {
                    m_graph->AddNode(item.name, world.x, world.y);
                    break;
                }
            }
        }
        Refresh();
    });

    PopupMenu(&menu, e.GetPosition());
}

// ─────────────────────────────────────────────────────────────────────────────
// OnMouseMove — handle panning, node dragging, and live link preview
// ─────────────────────────────────────────────────────────────────────────────

void NodeCanvas::OnMouseMove(wxMouseEvent &e)
{
    wxPoint cur   = e.GetPosition();
    wxPoint delta = cur - m_last_mouse;

    if (m_panning) {
        // Screen-space delta → world-space delta
        m_pan_x += static_cast<float>(delta.x) / m_zoom;
        m_pan_y += static_cast<float>(delta.y) / m_zoom;
        Refresh();
    }

    if (m_dragging_node >= 0) {
        glm::vec2 world = ScreenToWorld(cur);
        Node *node = m_graph->FindNode(m_dragging_node);
        if (node) {
            node->x = world.x - m_drag_offset.x;
            node->y = world.y - m_drag_offset.y;
        }
        Refresh();
    }

    if (m_linking_from_pin >= 0) {
        // Just need to update the cursor endpoint for the pending bezier.
        Refresh();
    }

    m_last_mouse = cur;
}

// ─────────────────────────────────────────────────────────────────────────────
// OnMouseUp — finish node drag or attempt to complete a link
// ─────────────────────────────────────────────────────────────────────────────

void NodeCanvas::OnMouseUp(wxMouseEvent &e)
{
    if (m_linking_from_pin >= 0) {
        glm::vec2 world = ScreenToWorld(e.GetPosition());
        int hit_pin = HitTestInputPin(world);
        if (hit_pin >= 0) {
            // Validate before adding — this also enforces IEC rules.
            auto result = m_graph->ValidateLink(m_linking_from_pin, hit_pin);
            if (result == Graph::LinkValidation::OK)
                m_graph->AddLink(m_linking_from_pin, hit_pin);
        }
        m_linking_from_pin = -1;
        Refresh();
    }

    m_dragging_node = -1;

    if (HasCapture())
        ReleaseMouse();
}

// ─────────────────────────────────────────────────────────────────────────────
// OnMouseWheel — zoom, keeping the point under the cursor fixed in world space
// ─────────────────────────────────────────────────────────────────────────────

void NodeCanvas::OnMouseWheel(wxMouseEvent &e)
{
    wxPoint mouse = e.GetPosition();

    // World position under the cursor before zoom change
    glm::vec2 world_before = ScreenToWorld(mouse);

    float factor = (e.GetWheelRotation() > 0) ? 1.1f : (1.0f / 1.1f);
    m_zoom *= factor;
    m_zoom  = std::max(0.05f, std::min(m_zoom, 20.0f));

    // World position under the same screen pixel after zoom
    glm::vec2 world_after = ScreenToWorld(mouse);

    // Shift pan so the cursor stays pinned to the same world point
    m_pan_x += world_after.x - world_before.x;
    m_pan_y += world_after.y - world_before.y;

    Refresh();
}

// ─────────────────────────────────────────────────────────────────────────────
// OnKeyDown — keyboard shortcuts
// ─────────────────────────────────────────────────────────────────────────────

void NodeCanvas::OnKeyDown(wxKeyEvent &e)
{
    switch (e.GetKeyCode()) {
    case WXK_DELETE:
    case WXK_BACK:
        // Delete the selected node and all its links.
        if (m_selected_node >= 0) {
            m_graph->RemoveNode(m_selected_node);
            m_selected_node = -1;
            m_dragging_node = -1;
            Refresh();
        }
        break;

    case WXK_ESCAPE:
        // Cancel an in-progress link.
        if (m_linking_from_pin >= 0) {
            m_linking_from_pin = -1;
            if (HasCapture()) ReleaseMouse();
            Refresh();
        }
        m_selected_node = -1;
        Refresh();
        break;

    default:
        e.Skip();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Commit edit
// ─────────────────────────────────────────────────────────────────────────────

void NodeCanvas::CommitEdit() {
    if (!m_editor || !m_editor->IsShown()) return;

    const wxString text = m_editor->GetValue();
    if (m_editing_node >= 0) {
        if (auto* node = m_graph->FindNode(m_editing_node)) {
            if (auto* inject = dynamic_cast<InjectNode*>(node)) {
                inject->forced_value = std::atof(text.ToStdString().c_str());
            }
        }
    }
    m_editor->Hide();
    m_editing_node = -1;
    SetFocus();
    Refresh(false);
}

// ─────────────────────────────────────────────────────────────────────────────
// Coordinate transforms
// ─────────────────────────────────────────────────────────────────────────────

// Projection convention (see BuildProjection):
//   world origin → screen centre
//   screen right  →  world +x
//   screen down   →  world +y   (y grows downward, matching screen space)
//   panning right (m_pan_x ↑) → content moves right on screen

glm::vec2 NodeCanvas::ScreenToWorld(wxPoint screen) const
{
    wxSize s = GetClientSize();
    float wx = static_cast<float>(screen.x - s.x / 2) / m_zoom - m_pan_x;
    float wy = static_cast<float>(screen.y - s.y / 2) / m_zoom - m_pan_y;
    return {wx, wy};
}

wxPoint NodeCanvas::WorldToScreen(glm::vec2 world) const
{
    wxSize s = GetClientSize();
    int sx = static_cast<int>((world.x + m_pan_x) * m_zoom + s.x * 0.5f);
    int sy = static_cast<int>((world.y + m_pan_y) * m_zoom + s.y * 0.5f);
    return {sx, sy};
}

// ─────────────────────────────────────────────────────────────────────────────
// BuildProjection
// ─────────────────────────────────────────────────────────────────────────────

glm::mat4 NodeCanvas::BuildProjection() const
{
    wxSize s  = GetClientSize();
    float hw  = (s.x * 0.5f) / m_zoom;  // half-width  in world units
    float hh  = (s.y * 0.5f) / m_zoom;  // half-height in world units
    float cx  = -m_pan_x;               // world x at screen centre
    float cy  = -m_pan_y;               // world y at screen centre
    // ortho(left, right, bottom, top): bottom > top flips y so +y goes downward.
    return glm::ortho(cx - hw, cx + hw,
                      cy + hh, cy - hh,
                      -1.0f,   1.0f);
}

// ─────────────────────────────────────────────────────────────────────────────
// Hit testing
// ─────────────────────────────────────────────────────────────────────────────

int NodeCanvas::HitTestNode(glm::vec2 world) const
{
    // Iterate in reverse so topmost-drawn node is tested first.
    const auto &nodes = m_graph->Nodes();
    for (auto it = nodes.rbegin(); it != nodes.rend(); ++it) {
        const Node &n = **it;
        if (world.x >= n.x && world.x <= n.x + NODE_WIDTH &&
            world.y >= n.y && world.y <= n.y + NODE_HEIGHT)
        {
            return n.id;
        }
    }
    return -1;
}

int NodeCanvas::HitTestOutputPin(glm::vec2 world) const
{
    for (const auto &node : m_graph->Nodes()) {
        float y = node->y + PIN_Y_START;
        for (const auto &pin : node->outputs) {
            glm::vec2 pin_pos(node->x + NODE_WIDTH, y);
            if (glm::length(world - pin_pos) <= PIN_HIT_RADIUS)
                return pin.id;
            y += PIN_SPACING;
        }
    }
    return -1;
}

int NodeCanvas::HitTestInputPin(glm::vec2 world) const
{
    for (const auto &node : m_graph->Nodes()) {
        float y = node->y + PIN_Y_START;
        for (const auto &pin : node->inputs) {
            glm::vec2 pin_pos(node->x, y);
            if (glm::length(world - pin_pos) <= PIN_HIT_RADIUS)
                return pin.id;
            y += PIN_SPACING;
        }
    }
    return -1;
}

} // namespace signal_forge