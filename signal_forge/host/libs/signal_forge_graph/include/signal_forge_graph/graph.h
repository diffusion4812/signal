#pragma once

#include <string>
#include <vector>
#include <map>
#include <set>
#include <queue>
#include <memory>
#include <stdexcept>
#include <algorithm>
#include <sstream>
#include <cstdint>
#include <filesystem>

namespace signal_forge {

class Renderer;

enum class PinDirection {
    INPUT,
    OUTPUT
};

class Field {
    public:enum class FieldType {
        REAL       // IEC REAL  → float
    };

    int         id      = -1;              // logical id (optional)
    std::string name;                      // symbol name in C struct
    FieldType   type    = FieldType::REAL; // data type (currently REAL)
    std::string description;
    std::string units;
    Field() = default;
    Field(std::string_view nm, FieldType t = FieldType::REAL, int id_ = -1, std::string_view desc = {})
    : id(id_), name(nm), type(t), description(desc) {}
};

class Pin : public Field {
public:
    PinDirection direction = PinDirection::INPUT;
    float        default_value = 0.0f;
    Pin() = default;
    Pin(PinDirection dir, std::string_view nm, FieldType t = FieldType::REAL, float def = 0.0f)
        : Field(nm, t), direction(dir), default_value(def) {}
};

class Static : public Field {
public:
    float default_value  = 0.0f;
    bool  host_readable  = false;
    bool  host_writable  = false;
    Static() = default;
    Static(std::string_view nm,
           float       def      = 0.0f,
           bool        readable = false,
           bool        writable = false)
        : Field(nm), default_value(def),
          host_readable(readable), host_writable(writable) {}
};

/* ═══════════════════════════════════════
   Node
   ═══════════════════════════════════════ */

class Node {
public:
    uint64_t         id = -1;        // Unique ID in graph
    int         exec_id = -1;   // Execution order ID
    std::string type;           // "PID", "ADD", "CONSTANT", etc.
    std::string instance;       // Unique name: "PID_01", "ADD_03"

    float x = 0.0f;            // Canvas position
    float y = 0.0f;

    std::vector<Pin> inputs;
    std::vector<Pin> outputs;
    std::vector<Static> statics;

    // Find pin by ID within this node, returns nullptr if not found
    const Pin* find_pin(int pin_id) const;
    Pin*       find_pin(int pin_id);

    // Find pin by name and direction
    const Pin* find_pin_by_name(const std::string &name,
                                PinDirection dir) const;
    virtual ~Node() = default;
};

class ProbeNode : public Node {
public:
    float value = 0.0f;
};

class InjectNode : public Node {
public:
    float observed_value = 0.0f;
    bool  forcing_active = false;
    float forced_value   = 0.0f;
};

/* ═══════════════════════════════════════
   Link (wire between two pins)
   ═══════════════════════════════════════ */

struct Link {
    int id       = -1;
    int from_pin = -1;      // Source output pin ID
    int to_pin   = -1;      // Destination input pin ID
};

/* ═══════════════════════════════════════
   Block Template
   Used by the editor palette to create
   new node instances.
   ═══════════════════════════════════════ */

struct BlockTemplate {
    std::string             type;
    std::string             description;
    uint64_t                signature;   // 64-bit magic block signature
    std::string             struct_name; // C struct name for this block
    std::string             entry_func;  // C function name to call for execution
    std::filesystem::path   library;     // Implementation
    std::filesystem::path   header;      // Header
    bool                    is_function_block = false;
    bool                    is_builtin        = false;
    std::vector<Pin>        inputs;
    std::vector<Pin>        outputs;
    std::vector<Static>     statics;
};

/* ═══════════════════════════════════════
   Graph
   ═══════════════════════════════════════ */

class Graph {
public:
    Graph();
    ~Graph();

    // ── ID generation ─────────────────────
    int next_id();

    // ── Block templates ───────────────────
    void RegisterBlockTemplate(const BlockTemplate &tmpl);
    const BlockTemplate* FindTemplate(const std::string &type) const;
    std::vector<std::string> AllBlockNames() const;
    const std::map<std::string, BlockTemplate>& AllTemplates() const;

    // ── Node management ───────────────────
    int  AddNode(const std::string &type, float x, float y);
    void AddNode(std::unique_ptr<Node> node);
    int  AddProbeNode(float x, float y);
    int  AddInjectNode(float x, float y);
    bool RemoveNode(int node_id);
    void ClearAll();

    // ── Link management ───────────────────
    int  AddLink(int from_pin_id, int to_pin_id);
    void AddLink(Link link);
    bool RemoveLink(int link_id);
    void RemoveLinksForNode(int node_id);
    void RemoveLinksForPin(int pin_id);

    // ── Queries ───────────────────────────
    Node*       FindNode(int node_id);
    const Node* FindNode(int node_id) const;

    Link*       FindLink(int link_id);
    const Link* FindLink(int link_id) const;

    const Pin&  GetPin(int pin_id) const;
    Pin&        GetPin(int pin_id);

    const Node& GetNodeForPin(int pin_id) const;
    Node&       GetNodeForPin(int pin_id);

    // Find which link feeds a given input pin (-1 if none)
    int FindLinkToPin(int input_pin_id) const;

    // Find all links from a given output pin
    std::vector<int> FindLinksFromPin(int output_pin_id) const;

    // Check if an input pin is connected
    bool IsPinConnected(int pin_id) const;

    // ── Signal naming ─────────────────────
    std::string GetSignalName(int link_id) const;
    std::string GetSignalNameForOutputPin(int output_pin_id) const;

    // ── Validation ────────────────────────
    // Check if a proposed link is valid
    enum class LinkValidation {
        OK,
        SAME_NODE,
        SAME_DIRECTION,
        TYPE_MISMATCH,
        INPUT_ALREADY_CONNECTED,
        DUPLICATE_LINK,
        PIN_NOT_FOUND
    };
    LinkValidation ValidateLink(int from_pin_id, int to_pin_id) const;

    // ── Accessors ─────────────────────────
    const std::vector<std::unique_ptr<Node>>& Nodes() const;
    const std::vector<Link>& Links() const;
    std::vector<std::unique_ptr<Node>>& Nodes();
    std::vector<Link>& Links();

    int NodeCount() const;
    int LinkCount() const;

    // ── Instance name generation ──────────
    std::string GenerateInstanceName(const std::string &type) const;

    // -- Next ID accessors (graph state consistency)
    int NextId() const { return next_id_; }
    void SetNextId(int id) { next_id_ = id; }

private:
    int next_id_ = 1;

    std::vector<std::unique_ptr<Node>> nodes_;
    std::vector<Link> m_links;
    std::map<std::string, BlockTemplate> m_templates;

    // Instance name counters per type
    mutable std::map<std::string, int> m_instance_counters;

    // Internal helpers
    void AssignPinIds(Node &node);
};

} // namespace signal_forge