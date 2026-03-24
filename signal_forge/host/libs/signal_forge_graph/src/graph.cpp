#include "signal_forge_graph/graph.h"
#include "signal_forge_graph/renderer.h"

namespace signal_forge {

/* ═══════════════════════════════════════
   Node
   ═══════════════════════════════════════ */

const Pin* Node::find_pin(int pin_id) const {
    for (const auto &p : inputs) {
        if (p.id == pin_id) return &p;
    }
    for (const auto &p : outputs) {
        if (p.id == pin_id) return &p;
    }
    return nullptr;
}

Pin* Node::find_pin(int pin_id) {
    for (auto &p : inputs) {
        if (p.id == pin_id) return &p;
    }
    for (auto &p : outputs) {
        if (p.id == pin_id) return &p;
    }
    return nullptr;
}

const Pin* Node::find_pin_by_name(const std::string &name,
                                   PinDirection dir) const {
    const auto &pins = (dir == PinDirection::INPUT) ? inputs : outputs;
    for (const auto &p : pins) {
        if (p.name == name) return &p;
    }
    return nullptr;
}

/* ═══════════════════════════════════════
   Graph — Construction / Destruction
   ═══════════════════════════════════════ */

Graph::Graph() {
    // ─────────────────────────────────────────────────────────────────────────────
    // init_builtin_templates
    //
    // Defines sf_probe and sf_inject as first-class BlockTemplates so every other
    // emit helper can treat them identically to vendor blocks.
    //
    // sf_probe
    //   inputs : in        (the signal being observed)
    //   outputs: out       (passthrough — keeps the probe transparent in the graph)
    //   statics: value     (host_readable) — captured snapshot read by the host
    //
    // sf_inject
    //   inputs : in             (the normal signal path)
    //   outputs: out            (forced or passthrough)
    //   statics: forced_value   (host_writable) — value the host wants to inject
    //            force_enable   (host_writable) — 0 = passthrough, ≠0 = force
    // ─────────────────────────────────────────────────────────────────────────────

    // ── sf_probe ──────────────────────────────────────────────────────────────
    {
        BlockTemplate t;
        t.struct_name       = "sf_probe_t";
        t.type              = "sf_probe";
        t.description       = "Signal probe: makes a signal observable by the host";
        t.signature         = 0xB9072A4C1F3E8D00ull; // fixed; never changes layout
        t.is_function_block = true;
        t.is_builtin        = true;
        t.entry_func        = "sf_probe_step";

        Pin in_pin(PinDirection::INPUT, "in");
        in_pin.id          = 0;
        in_pin.description = "Signal to observe";
        t.inputs.push_back(in_pin);

        Static val;
        val.id            = 0;
        val.name          = "value";
        val.description   = "Last captured value (host read-only)";
        val.host_readable = true;
        val.host_writable = false;
        t.statics.push_back(val);

        RegisterBlockTemplate(std::move(t));
    }

    // ── sf_inject ─────────────────────────────────────────────────────────────
    {
        BlockTemplate t;
        t.struct_name       = "sf_inject_t";
        t.type              = "sf_inject";
        t.description       = "Signal injector: lets the host force-override a signal";
        t.signature         = 0xD4F81B6E2C097A00ull; // fixed; never changes layout
        t.is_function_block = true;
        t.is_builtin        = true;
        t.entry_func        = "sf_inject_step";

        Pin in_pin(PinDirection::INPUT, "in");
        in_pin.id          = 0;
        in_pin.description = "Normal signal input";
        t.inputs.push_back(in_pin);

        Pin out_pin(PinDirection::OUTPUT, "out");
        out_pin.id          = 0;
        out_pin.description = "Forced or passthrough output";
        t.outputs.push_back(out_pin);

        Static forced_val;
        forced_val.id            = 0;
        forced_val.name          = "forced_value";
        forced_val.description   = "Value injected when force_enable ≠ 0";
        forced_val.host_readable = true;
        forced_val.host_writable = true;
        t.statics.push_back(forced_val);

        Static force_en;
        force_en.id            = 1;
        force_en.name          = "force_enable";
        force_en.description   = "0 = passthrough; non-zero = inject forced_value";
        force_en.host_readable = true;
        force_en.host_writable = true;
        t.statics.push_back(force_en);

        RegisterBlockTemplate(std::move(t));
    }
}

Graph::~Graph() {}

/* ═══════════════════════════════════════
   ID Generation
   ═══════════════════════════════════════ */

int Graph::next_id() {
    return next_id_++;
}

/* ═══════════════════════════════════════
   Block Templates
   ═══════════════════════════════════════ */

void Graph::RegisterBlockTemplate(const BlockTemplate &tmpl) {
    m_templates[tmpl.type] = tmpl;
}

const BlockTemplate* Graph::FindTemplate(const std::string &type) const {
    auto it = m_templates.find(type);
    return (it != m_templates.end()) ? &it->second : nullptr;
}

std::vector<std::string> Graph::AllBlockNames() const {
    std::vector<std::string> types;
    types.reserve(m_templates.size());
    for (const auto &[k, v] : m_templates) {
        types.push_back(k);
    }
    return types;
}

const std::map<std::string, BlockTemplate>& Graph::AllTemplates() const {
    return m_templates;
}

/* ═══════════════════════════════════════
   Instance Name Generation
   ═══════════════════════════════════════ */

std::string Graph::GenerateInstanceName(const std::string &type) const {
    // Find the highest existing index for this type
    int max_index = 0;
    std::string prefix = type + "_";

    for (const auto &node : nodes_) {
        if (node->type == type) {
            // Extract number from instance name like "PID_03"
            if (node->instance.rfind(prefix, 0) == 0) {
                std::string num_str = node->instance.substr(prefix.size());
                try {
                    int idx = std::stoi(num_str);
                    if (idx > max_index) max_index = idx;
                } catch (...) {}
            }
        }
    }

    std::ostringstream oss;
    oss << type << "_";
    // Zero-padded two digit index
    int next = max_index + 1;
    if (next < 10) oss << "0";
    oss << next;
    return oss.str();
}

/* ═══════════════════════════════════════
   Pin ID Assignment
   ═══════════════════════════════════════ */

void Graph::AssignPinIds(Node &node) {
    for (auto &p : node.inputs) {
        p.id = next_id();
    }
    for (auto &p : node.outputs) {
        p.id = next_id();
    }
    for (auto &s : node.statics) {
        s.id = next_id();
    }
}

/* ═══════════════════════════════════════
   Node Management
   ═══════════════════════════════════════ */

int Graph::AddNode(const std::string &type, float x, float y) {
    const BlockTemplate *tmpl = FindTemplate(type);
    if (!tmpl) {
        throw std::runtime_error(
            "SignalForge: Unknown block type '" + type + "'");
    }

    auto node = std::make_unique<Node>();
    node->id       = next_id();
    node->type     = type;
    node->instance = GenerateInstanceName(type);
    node->x        = x;
    node->y        = y;

    // Copy pin templates
    node->inputs  = tmpl->inputs;
    node->outputs = tmpl->outputs;
    node->statics = tmpl->statics;

    // Assign unique IDs to all fields
    AssignPinIds(*node);

    int id = node->id;
    nodes_.push_back(std::move(node));
    return id;
}

void Graph::AddNode(std::unique_ptr<Node> node) {
    if (!node) return;
    nodes_.push_back(std::move(node));
}

int Graph::AddProbeNode(float x, float y) {
    const BlockTemplate *tmpl = FindTemplate("sf_probe");
    if (!tmpl) {
        throw std::runtime_error(
            "SignalForge: Unknown block type 'sf_probe'");
    }

    auto node = std::make_unique<ProbeNode>();
    node->id  = next_id();
    node->type = "sf_probe";
    node->instance = GenerateInstanceName("sf_probe");
    node->x        = x;
    node->y        = y;

    // Copy pin templates
    node->inputs  = tmpl->inputs;
    node->outputs = tmpl->outputs;
    node->statics = tmpl->statics;

    // Assign unique IDs to all fields
    AssignPinIds(*node);

    int id = node->id;
    nodes_.push_back(std::move(node));
    return id;
}

int Graph::AddInjectNode(float x, float y) {
    const BlockTemplate *tmpl = FindTemplate("sf_inject");
    if (!tmpl) {
        throw std::runtime_error(
            "SignalForge: Unknown block type 'sf_inject'");
    }

    auto node = std::make_unique<InjectNode>();
    node->id  = next_id();
    node->type = "sf_inject";
    node->instance = GenerateInstanceName("sf_inject");
    node->x        = x;
    node->y        = y;

    // Copy pin templates
    node->inputs  = tmpl->inputs;
    node->outputs = tmpl->outputs;
    node->statics = tmpl->statics;

    // Assign unique IDs to all fields
    AssignPinIds(*node);

    int id = node->id;
    nodes_.push_back(std::move(node));
    return id;
}

bool Graph::RemoveNode(int node_id) {
    auto it = std::find_if(nodes_.begin(), nodes_.end(),
        [node_id](const std::unique_ptr<Node>& n) { return n->id == node_id; });

    if (it == nodes_.end()) return false;

    RemoveLinksForNode(node_id);

    nodes_.erase(it);
    return true;
}


void Graph::ClearAll() {
    nodes_.clear();
    m_links.clear();
    next_id_ = 1;
}

/* ═══════════════════════════════════════
   Link Management
   ═══════════════════════════════════════ */

int Graph::AddLink(int from_pin_id, int to_pin_id) {
    LinkValidation validation = ValidateLink(from_pin_id, to_pin_id);
    if (validation != LinkValidation::OK) {
        return -1;
    }

    Link link;
    link.id       = next_id();
    link.from_pin = from_pin_id;
    link.to_pin   = to_pin_id;

    int id = link.id;
    m_links.push_back(std::move(link));
    return id;
}

void Graph::AddLink(Link link) {
    m_links.push_back(std::move(link));
}

bool Graph::RemoveLink(int link_id) {
    auto it = std::find_if(m_links.begin(), m_links.end(),
        [link_id](const Link &l) { return l.id == link_id; });

    if (it == m_links.end()) return false;

    m_links.erase(it);
    return true;
}

void Graph::RemoveLinksForNode(int node_id) {
    const Node *node = FindNode(node_id);
    if (!node) return;

    // Collect all pin IDs belonging to this node
    std::set<int> pin_ids;
    for (const auto &p : node->inputs)  pin_ids.insert(p.id);
    for (const auto &p : node->outputs) pin_ids.insert(p.id);

    // Remove any link that references these pins
    m_links.erase(
        std::remove_if(m_links.begin(), m_links.end(),
            [&pin_ids](const Link &l) {
                return pin_ids.count(l.from_pin) > 0 ||
                       pin_ids.count(l.to_pin) > 0;
            }),
        m_links.end()
    );
}

void Graph::RemoveLinksForPin(int pin_id) {
    m_links.erase(
        std::remove_if(m_links.begin(), m_links.end(),
            [pin_id](const Link &l) {
                return l.from_pin == pin_id || l.to_pin == pin_id;
            }),
        m_links.end()
    );
}

/* ═══════════════════════════════════════
   Queries
   ═══════════════════════════════════════ */

Node* Graph::FindNode(int node_id) {
    for (auto &n : nodes_) {
        if (n->id == node_id) return n.get();
    }
    return nullptr;
}

const Node* Graph::FindNode(int node_id) const {
    for (const auto &n : nodes_) {
        if (n->id == node_id) return n.get();
    }
    return nullptr;
}

Link* Graph::FindLink(int link_id) {
    for (auto &l : m_links) {
        if (l.id == link_id) return &l;
    }
    return nullptr;
}

const Link* Graph::FindLink(int link_id) const {
    for (const auto &l : m_links) {
        if (l.id == link_id) return &l;
    }
    return nullptr;
}

const Pin& Graph::GetPin(int pin_id) const {
    for (const auto &node : nodes_) {
        const Pin *p = node->find_pin(pin_id);
        if (p) return *p;
    }
    throw std::runtime_error(
        "SignalForge: Pin ID " + std::to_string(pin_id) + " not found");
}

Pin& Graph::GetPin(int pin_id) {
    for (auto &node : nodes_) {
        Pin *p = node->find_pin(pin_id);
        if (p) return *p;
    }
    throw std::runtime_error(
        "SignalForge: Pin ID " + std::to_string(pin_id) + " not found");
}

const Node& Graph::GetNodeForPin(int pin_id) const {
    for (const auto &node : nodes_) {
        if (node->find_pin(pin_id)) return *node;
    }
    throw std::runtime_error(
        "SignalForge: No node found for pin ID "
        + std::to_string(pin_id));
}

Node& Graph::GetNodeForPin(int pin_id) {
    for (auto &node : nodes_) {
        if (node->find_pin(pin_id)) return *node;
    }
    throw std::runtime_error(
        "SignalForge: No node found for pin ID "
        + std::to_string(pin_id));
}

int Graph::FindLinkToPin(int input_pin_id) const {
    for (const auto &link : m_links) {
        if (link.to_pin == input_pin_id) return link.id;
    }
    return -1;
}

std::vector<int> Graph::FindLinksFromPin(int output_pin_id) const {
    std::vector<int> result;
    for (const auto &link : m_links) {
        if (link.from_pin == output_pin_id) {
            result.push_back(link.id);
        }
    }
    return result;
}

bool Graph::IsPinConnected(int pin_id) const {
    for (const auto &link : m_links) {
        if (link.from_pin == pin_id || link.to_pin == pin_id) {
            return true;
        }
    }
    return false;
}

/* ═══════════════════════════════════════
   Signal Naming
   ═══════════════════════════════════════ */

std::string Graph::GetSignalName(int link_id) const {
    const Link *link = FindLink(link_id);
    if (!link) {
        throw std::runtime_error(
            "SignalForge: Link ID " + std::to_string(link_id)
            + " not found");
    }
    return GetSignalNameForOutputPin(link->from_pin);
}

std::string Graph::GetSignalNameForOutputPin(int output_pin_id) const {
    const Node &node = GetNodeForPin(output_pin_id);
    const Pin &pin   = GetPin(output_pin_id);
    return node.instance + "_" + pin.name;
}

/* ═══════════════════════════════════════
   Link Validation
   ═══════════════════════════════════════ */

Graph::LinkValidation Graph::ValidateLink(int from_pin_id,
                                           int to_pin_id) const {
    // Check pins exist
    const Pin *from_pin = nullptr;
    const Pin *to_pin   = nullptr;
    const Node *from_node = nullptr;
    const Node *to_node   = nullptr;

    for (const auto &node : nodes_) {
        const Pin *p = node->find_pin(from_pin_id);
        if (p) { from_pin = p; from_node = node.get(); }
        p = node->find_pin(to_pin_id);
        if (p) { to_pin = p; to_node = node.get(); }
    }

    if (!from_pin || !to_pin) {
        return LinkValidation::PIN_NOT_FOUND;
    }

    // Cannot link within same node
    if (from_node->id == to_node->id) {
        return LinkValidation::SAME_NODE;
    }

    // Must be output -> input
    if (from_pin->direction != PinDirection::OUTPUT ||
        to_pin->direction   != PinDirection::INPUT) {
        return LinkValidation::SAME_DIRECTION;
    }

    // Type compatibility
    if (from_pin->type != to_pin->type) {
        bool compatible = false;
        if (!compatible) {
            return LinkValidation::TYPE_MISMATCH;
        }
    }

    // IEC rule: each input pin can only have one source
    for (const auto &link : m_links) {
        if (link.to_pin == to_pin_id) {
            return LinkValidation::INPUT_ALREADY_CONNECTED;
        }
    }

    // No duplicate links
    for (const auto &link : m_links) {
        if (link.from_pin == from_pin_id &&
            link.to_pin == to_pin_id) {
            return LinkValidation::DUPLICATE_LINK;
        }
    }

    return LinkValidation::OK;
}

/* ═══════════════════════════════════════
   Accessors
   ═══════════════════════════════════════ */

const std::vector<std::unique_ptr<Node>>& Graph::Nodes() const { return nodes_; }
const std::vector<Link>& Graph::Links() const { return m_links; }
std::vector<std::unique_ptr<Node>>& Graph::Nodes() { return nodes_; }
std::vector<Link>& Graph::Links() { return m_links; }

int Graph::NodeCount() const { return static_cast<int>(nodes_.size()); }
int Graph::LinkCount() const { return static_cast<int>(m_links.size()); }

} // namespace signal_forge