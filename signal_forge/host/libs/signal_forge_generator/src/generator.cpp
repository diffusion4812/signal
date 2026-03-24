// generator.cpp
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <sstream>
#include <set>
#include <iostream>
#include <queue>
#include <cctype>
#include <cstdint>

#include "signal_forge_generator/generator.h"

namespace signal_forge {

// ─────────────────────────────────────────────────────────────────────────────
// Node type helpers — use dynamic_cast; no category enum required
// ─────────────────────────────────────────────────────────────────────────────

static bool is_probe_node(const Node* n) {
    return dynamic_cast<const ProbeNode*>(n) != nullptr;
}
static bool is_inject_node(const Node* n) {
    return dynamic_cast<const InjectNode*>(n) != nullptr;
}
static bool is_standard_node(const Node* n) {
    return !is_probe_node(n) && !is_inject_node(n);
}

// ─────────────────────────────────────────────────────────────────────────────
// Constructor
// ─────────────────────────────────────────────────────────────────────────────

Generator::Generator(const Graph& g) : graph_(g) {
    init_block_library_map();
}


// ─────────────────────────────────────────────────────────────────────────────
// find_template — graph registry first, built-ins second
// ─────────────────────────────────────────────────────────────────────────────

const BlockTemplate* Generator::find_template(const std::string& type) const {
    if (const BlockTemplate* t = graph_.FindTemplate(type))
        return t;
    auto it = builtin_templates_.find(type);
    if (it != builtin_templates_.end())
        return &it->second;
    return nullptr;
}

std::string Generator::struct_name_for_node(const Node* n) const {
    if (const BlockTemplate* t = find_template(n->type))
        return t->struct_name;
    return "void";
}

// ─────────────────────────────────────────────────────────────────────────────
// generate
// ─────────────────────────────────────────────────────────────────────────────

Generator::Result Generator::generate() {
    collect_required_blocks();
    assign_slots();

    auto resolved = resolve_links();
    auto order    = topological_order_from_resolved(resolved);

    std::ostringstream out;

    emit_task_host_core_header(out);
    emit_includes             (out);
    emit_builtin_defs         (out);   // typedef struct + step functions for builtins
    emit_block_enum           (out);
    emit_sig_defines          (out);
    emit_pin_info             (out);
    emit_block_registry       (out);
    emit_inline_structs       (out);   // S_<instance>(ctx) accessors for all nodes
    //emit_debug_interface      (out);   // debug map + inject message handler
    emit_task_entry           (out, resolved, order);

    Result r;
    r.c_source = out.str();
    r.link_headers.assign(required_block_headers_.begin(), required_block_headers_.end());
    r.link_objects.assign(required_libs_.begin(),  required_libs_.end());
    return r;
}

// ─────────────────────────────────────────────────────────────────────────────
// resolve_links
//
// All links that touch probe or inject nodes are now included: probes need
// their input fed and their passthrough output consumed; inject nodes need
// their input assigned and their (possibly forced) output consumed.
// ─────────────────────────────────────────────────────────────────────────────

std::vector<Generator::ResolvedLink> Generator::resolve_links() const {
    std::vector<ResolvedLink> resolved;
    resolved.reserve(graph_.Links().size());

    for (const auto& link : graph_.Links()) {
        const Node& from_node = graph_.GetNodeForPin(link.from_pin);
        const Node& to_node   = graph_.GetNodeForPin(link.to_pin);

        const Pin* src_pin = from_node.find_pin(link.from_pin);
        const Pin* dst_pin = to_node.find_pin(link.to_pin);
        if (!src_pin || !dst_pin)
            continue;

        ResolvedLink rl;
        rl.id           = link.id;
        rl.src_node_id  = from_node.id;
        rl.src_instance = from_node.instance;
        rl.src_pin_name = src_pin->name;
        rl.dst_node_id  = to_node.id;
        rl.dst_instance = to_node.instance;
        rl.dst_pin_name = dst_pin->name;
        rl.from_pin_id  = link.from_pin;
        rl.to_pin_id    = link.to_pin;

        resolved.push_back(std::move(rl));
    }

    std::sort(resolved.begin(), resolved.end(),
              [](const auto& a, const auto& b){ return a.id < b.id; });
    return resolved;
}

// ─────────────────────────────────────────────────────────────────────────────
// init_block_library_map — standard (non-builtin) blocks only
// ─────────────────────────────────────────────────────────────────────────────

void Generator::init_block_library_map() {
    for (const auto& [typeName, blockTmpl] : graph_.AllTemplates()) {
        if (blockTmpl.is_builtin) continue;
        block_lib_map_.try_emplace(typeName,
                                   blockTmpl.header.string(),
                                   blockTmpl.library.string());
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// collect_required_blocks — gathers headers/libs for standard nodes;
//                           records which builtin types are actually used
// ─────────────────────────────────────────────────────────────────────────────

void Generator::collect_required_blocks() {
    for (const auto& n : graph_.Nodes()) {
        if (is_probe_node(n.get())) {
            used_builtin_types_.insert("sf_probe");
            continue;
        }
        if (is_inject_node(n.get())) {
            used_builtin_types_.insert("sf_inject");
            continue;
        }
        // Standard node
        auto it = block_lib_map_.find(n->type);
        if (it != block_lib_map_.end()) {
            required_block_headers_.insert(it->second.header);
            required_libs_.insert(it->second.object);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// assign_slots — ALL nodes get a slot (probe and inject included)
// ─────────────────────────────────────────────────────────────────────────────

void Generator::assign_slots() {
    int idx = 0;
    for (const auto& n : graph_.Nodes()) {
        slot_by_nodeid_[n->id]      = idx;
        slot_name_by_nodeid_[n->id] = sanitize_slot_name(n->instance);
        ++idx;
    }
}

std::string Generator::sanitize_slot_name(const std::string& inst) {
    std::string s = inst;
    for (auto& c : s)
        c = std::isalnum((unsigned char)c)
            ? (char)std::toupper((unsigned char)c)
            : '_';
    return std::string("SLOT_") + s;
}

// ─────────────────────────────────────────────────────────────────────────────
// topological_order_from_resolved
// ─────────────────────────────────────────────────────────────────────────────

std::vector<const Node*> Generator::topological_order_from_resolved(
    const std::vector<Generator::ResolvedLink>& resolved) const
{
    std::unordered_map<int, std::vector<int>> adj;
    std::unordered_map<int, int> indeg;

    for (const auto& n : graph_.Nodes()) indeg[n->id] = 0;

    for (const auto& rl : resolved) {
        adj[rl.src_node_id].push_back(rl.dst_node_id);
        indeg[rl.dst_node_id]++;
    }

    std::queue<int> q;
    for (const auto& [id, deg] : indeg)
        if (deg == 0) q.push(id);

    std::unordered_map<int, const Node*> id_to_ptr;
    for (const auto& n : graph_.Nodes())
        id_to_ptr[n->id] = n.get();

    std::vector<const Node*> order;
    order.reserve(graph_.Nodes().size());

    while (!q.empty()) {
        int u = q.front(); q.pop();
        if (auto it = id_to_ptr.find(u); it != id_to_ptr.end())
            order.push_back(it->second);
        if (auto adj_it = adj.find(u); adj_it != adj.end())
            for (int v : adj_it->second)
                if (--indeg[v] == 0) q.push(v);
    }

    // Cycle detected — fall back to declaration order
    if (order.size() != graph_.Nodes().size()) {
        order.clear();
        for (const auto& n : graph_.Nodes())
            order.push_back(n.get());
    }
    return order;
}

// ─────────────────────────────────────────────────────────────────────────────
// Emit helpers
// ─────────────────────────────────────────────────────────────────────────────

void Generator::emit_task_host_core_header(std::ostringstream& out) {
    out << "#include \"task_host_core/interface.h\"\n"
        << "#include \"task_host_core/manifest.h\"\n\n";
}

void Generator::emit_includes(std::ostringstream& out) {
    for (const auto& h : required_block_headers_)
        out << "#include \"" << h << "\"\n";
    if (!required_block_headers_.empty()) out << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// emit_builtin_defs
//
// Emits self-contained typedef struct + static inline step function for each
// builtin type that is actually used in the graph.  These definitions live
// entirely in the generated .c file — no separate header needed.
// ─────────────────────────────────────────────────────────────────────────────

void Generator::emit_builtin_defs(std::ostringstream& out) {
    if (used_builtin_types_.empty()) return;

    out << "/* Built-in block definitions */\n";

    if (used_builtin_types_.count("sf_probe")) {
        out <<
            "typedef struct {\n"
            "    float in;     /* input:  signal to observe           */\n"
            "    float value;  /* static: last captured value (host-readable) */\n"
            "} sf_probe_t;\n"
            "\n"
            "static inline void sf_probe_step(sf_probe_t *b) {\n"
            "    b->value = b->in;\n"
            "}\n\n";
    }

    if (used_builtin_types_.count("sf_inject")) {
        out <<
            "typedef struct {\n"
            "    float in;            /* input:  normal signal path            */\n"
            "    float out;           /* output: forced or passthrough          */\n"
            "    float forced_value;  /* static: value to inject (host-writable) */\n"
            "    float force_enable;  /* static: 0=passthrough, !=0=force (host-writable) */\n"
            "} sf_inject_t;\n"
            "\n"
            "static inline void sf_inject_step(sf_inject_t *b) {\n"
            "    b->out = (b->force_enable != 0.0f) ? b->forced_value : b->in;\n"
            "}\n\n";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// emit_block_enum — all nodes
// ─────────────────────────────────────────────────────────────────────────────

void Generator::emit_block_enum(std::ostringstream& out) {
    if (graph_.Nodes().empty()) return;

    out << "enum {\n";
    for (const auto& n : graph_.Nodes()) {
        out << "    " << slot_name_by_nodeid_.at(n->id)
            << " = " << slot_by_nodeid_.at(n->id) << ",\n";
    }
    out << "};\n\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// emit_sig_defines — signature #define for every node
// ─────────────────────────────────────────────────────────────────────────────

void Generator::emit_sig_defines(std::ostringstream& out) {
    for (const auto& n : graph_.Nodes()) {
        const BlockTemplate* tmpl = find_template(n->type);
        if (!tmpl) continue;
        out << "#define " << slot_name_by_nodeid_.at(n->id)
            << "_SIG " << tmpl->signature << "ull\n";
    }
    out << "\n";
}

static inline std::string manifest_type_from_sf_graph_type_str(Field::FieldType ft)
{
    switch (ft) {
    case Field::FieldType::REAL: return "FIELD_TYPE_REAL";
    //case FIELD_TYPE_BYTE:           return "BYTE";
    //case FIELD_TYPE_WORD:           return "WORD";
    //case FIELD_TYPE_DWORD:          return "DWORD";
    //case FIELD_TYPE_LWORD:          return "LWORD";
    //case FIELD_TYPE_USINT:          return "USINT";
    //case FIELD_TYPE_UINT:           return "UINT";
    //case FIELD_TYPE_UDINT:          return "UDINT";
    //case FIELD_TYPE_ULINT:          return "ULINT";
    //case FIELD_TYPE_SINT:           return "SINT";
    //case FIELD_TYPE_INT:            return "INT";
    //case FIELD_TYPE_DINT:           return "DINT";
    //case FIELD_TYPE_LINT:           return "LINT";
    //case FIELD_TYPE_REAL:           return "REAL";
    //case FIELD_TYPE_LREAL:          return "LREAL";
    //case FIELD_TYPE_TIME:           return "TIME";
    //case FIELD_TYPE_LTIME:          return "LTIME";
    //case FIELD_TYPE_DATE:           return "DATE";
    //case FIELD_TYPE_LDATE:          return "LDATE";
    //case FIELD_TYPE_TIME_OF_DAY:    return "TIME_OF_DAY";
    //case FIELD_TYPE_LTIME_OF_DAY:   return "LTIME_OF_DAY";
    //case FIELD_TYPE_DATE_AND_TIME:  return "DATE_AND_TIME";
    //case FIELD_TYPE_LDATE_AND_TIME: return "LDATE_AND_TIME";
    //case FIELD_TYPE_STRING:         return "STRING";
    //case FIELD_TYPE_WSTRING:        return "WSTRING";
    default:                        return "unknown";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// emit_pin_info — Pin + Static field tables for ALL nodes
// ─────────────────────────────────────────────────────────────────────────────
void Generator::emit_pin_info(std::ostringstream& out) {
    for (const auto& n : graph_.Nodes()) {
        const BlockTemplate* tmpl = find_template(n->type);
        if (!tmpl) continue;

        const std::string& slot = slot_name_by_nodeid_.at(n->id);
        out << "static const FieldInfo " << slot << "_FIELDS[] = {\n";

        for (const auto& f : tmpl->inputs) {
            int found_id = -1;
            for (const auto& node_input : n->inputs) {
                if (f.name == node_input.name) {
                    found_id = node_input.id;
                    break;
                }
            }

            out << "    FIELD_ENTRY(" << tmpl->struct_name << ", " << found_id << ", FIELD_DIR_INPUT, " << manifest_type_from_sf_graph_type_str(f.type) << ", " << n->instance << ", " << f.name << "),\n";
        }

        for (const auto& f : tmpl->outputs) {
            int found_id = -1;
            for (const auto& node_output : n->outputs) {
                if (f.name == node_output.name) {
                    found_id = node_output.id;
                    break;
                }
            }

            out << "    FIELD_ENTRY(" << tmpl->struct_name << ", " << found_id << ", FIELD_DIR_OUTPUT, " << manifest_type_from_sf_graph_type_str(f.type) << ", " << n->instance << ", " << f.name << "),\n";
        }
        for (const auto& s : tmpl->statics) {
            // Find the static id from the node's statics (mirror input/output logic)
            int found_id = -1;
            for (const auto& node_static : n->statics) {
                if (s.name == node_static.name) {
                    found_id = node_static.id;
                    break;
                }
            }

            // Derive the access-permission flag from the host_readable/writable
            // flags on the Static so the runtime knows which statics it may
            // read from or write to over the debug interface.
            const char* access = "FIELD_ACCESS_NONE";
            if (s.host_readable && s.host_writable) access = "FIELD_ACCESS_RW";
            else if (s.host_readable)               access = "FIELD_ACCESS_RD";
            else if (s.host_writable)               access = "FIELD_ACCESS_WR";

            out << "    STATIC_ENTRY(" << tmpl->struct_name << ", " << found_id << ", " << access << ", " << manifest_type_from_sf_graph_type_str(s.type) << ", " << n->instance << ", " << s.name << "),\n";
        }
        out << "};\n\n";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// emit_block_registry — all nodes
// ─────────────────────────────────────────────────────────────────────────────

void Generator::emit_block_registry(std::ostringstream& out) {
    const int total = (int)graph_.Nodes().size();

    out << "static const BlockRegistryEntry TASK_BLOCK_REGISTRY[]";
    if (total == 0) {
        out << ";\n\n";
    } else {
        out << " = {\n";
        for (const auto& n : graph_.Nodes()) {
            const BlockTemplate* tmpl = find_template(n->type);
            if (!tmpl) continue;

            const std::string& slot = slot_name_by_nodeid_.at(n->id);
            out << "    {\n"
                << "        .block_id   = " << n->id          << ",\n"
                << "        .block_name = \"" << n->type      << "\",\n"
                << "        .signature  = " << slot           << "_SIG,\n"
                << "        .block_size = sizeof("  << tmpl->struct_name << "),\n"
                << "        .field_count = (uint64_t)(sizeof(" << slot << "_FIELDS)"
                                          << " / sizeof(" << slot << "_FIELDS[0])),\n"
                << "        .fields     = " << slot << "_FIELDS\n"
                << "    },\n";
        }
        out << "};\n\n";
    }

    out << "const BlockRegistryEntry* task_registry(uint64_t *count) {\n"
        << "    if (count) {\n";
    if (total == 0)
        out << "        *count = 0;\n";
    else
        out << "        *count = (uint64_t)(sizeof(TASK_BLOCK_REGISTRY)"
            << " / sizeof(TASK_BLOCK_REGISTRY[0]));\n";
    out << "    }\n"
        << "    return TASK_BLOCK_REGISTRY;\n"
        << "}\n\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// emit_inline_structs — S_<instance>(ctx) accessor for ALL nodes
// ─────────────────────────────────────────────────────────────────────────────

void Generator::emit_inline_structs(std::ostringstream& out) {
    for (const auto& n : graph_.Nodes()) {
        const std::string sname = struct_name_for_node(n.get());
        const std::string& slot = slot_name_by_nodeid_.at(n->id);

        out << "static inline " << sname << "* S_" << n->instance
            << "(TaskContext *ctx) {\n"
            << "    return (" << sname << "*)ctx->slots[" << slot << "];\n"
            << "}\n\n";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// emit_debug_interface
//
// Emits three things:
//
//  1. sf_debug_map_t
//       A flat struct of float* pointers — one per host-accessible static
//       across all probe and inject nodes.  The host communication layer can
//       locate this symbol by name and dereference its members directly.
//
//  2. sf_debug_map_init(TaskContext*)
//       Must be called once at startup (after ctx->slots are allocated).
//       Fills every pointer in sf_debug_map from the live slot memory.
//
//  3. sf_inject_msg_t / sf_handle_inject_msg(TaskContext*, const sf_inject_msg_t*)
//       Tiny message handler called by the host communication layer whenever
//       the host wants to update an inject block's force_enable or forced_value.
//       The host sends {slot_id, field_id, value} where:
//           field_id 0  →  forced_value
//           field_id 1  →  force_enable
// ─────────────────────────────────────────────────────────────────────────────

void Generator::emit_debug_interface(std::ostringstream& out) {
    // Collect probe and inject nodes for easy iteration below
    std::vector<const Node*> probe_nodes;
    std::vector<const Node*> inject_nodes;
    for (const auto& n : graph_.Nodes()) {
        if (is_probe_node(n.get()))  probe_nodes.push_back(n.get());
        if (is_inject_node(n.get())) inject_nodes.push_back(n.get());
    }

    if (probe_nodes.empty() && inject_nodes.empty()) return;

    out << "/* Debug interface */\n\n";

    // ── 1. sf_debug_map_t ────────────────────────────────────────────────────
    out << "typedef struct {\n";
    for (const Node* n : probe_nodes)
        out << "    float *probe_" << n->instance << "_value;\n";
    for (const Node* n : inject_nodes) {
        out << "    float *inject_" << n->instance << "_forced_value;\n";
        out << "    float *inject_" << n->instance << "_force_enable;\n";
    }
    out << "} sf_debug_map_t;\n\n"
        << "static sf_debug_map_t sf_debug_map;\n\n";

    // ── 2. sf_debug_map_init ─────────────────────────────────────────────────
    out << "void sf_debug_map_init(TaskContext *ctx) {\n";
    for (const Node* n : probe_nodes) {
        const std::string& slot = slot_name_by_nodeid_.at(n->id);
        out << "    sf_debug_map.probe_" << n->instance << "_value"
            << " = &((sf_probe_t*)ctx->slots[" << slot << "])->value;\n";
    }
    for (const Node* n : inject_nodes) {
        const std::string& slot = slot_name_by_nodeid_.at(n->id);
        out << "    sf_debug_map.inject_" << n->instance << "_forced_value"
            << " = &((sf_inject_t*)ctx->slots[" << slot << "])->forced_value;\n";
        out << "    sf_debug_map.inject_" << n->instance << "_force_enable"
            << " = &((sf_inject_t*)ctx->slots[" << slot << "])->force_enable;\n";
    }
    out << "}\n\n";

    // ── 3. sf_inject_msg_t + sf_handle_inject_msg ────────────────────────────
    if (!inject_nodes.empty()) {
        out <<
            "/* Message sent from host to target to control an inject block.\n"
            "   slot_id  : matches the SLOT_* enum value for the inject node\n"
            "   field_id : 0 = forced_value, 1 = force_enable\n"
            "   value    : float value to write into the selected field        */\n"
            "typedef struct {\n"
            "    uint32_t slot_id;\n"
            "    uint32_t field_id;\n"
            "    float    value;\n"
            "} sf_inject_msg_t;\n\n"
            "void sf_handle_inject_msg(TaskContext *ctx, const sf_inject_msg_t *msg) {\n"
            "    switch (msg->slot_id) {\n";

        for (const Node* n : inject_nodes) {
            const std::string& slot = slot_name_by_nodeid_.at(n->id);
            out << "    case " << slot << ": {\n"
                << "        sf_inject_t *b = (sf_inject_t*)ctx->slots[" << slot << "];\n"
                << "        if (msg->field_id == 0) b->forced_value = msg->value;\n"
                << "        else                    b->force_enable  = msg->value;\n"
                << "        break;\n"
                << "    }\n";
        }

        out << "    default: break;\n"
            << "    }\n"
            << "}\n\n";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// emit_task_entry
//
//  Standard node  → S_<inst>(ctx) pointer declaration + entry_func() call
//  ProbeNode      → sf_probe_step() (captures value, passthrough out)
//  InjectNode     → sf_inject_step() (conditional forced output)
//  Wiring         → resolved link assignments for all node types
// ─────────────────────────────────────────────────────────────────────────────

void Generator::emit_task_entry(std::ostringstream& out,
                                 const std::vector<ResolvedLink>& resolved,
                                 const std::vector<const Node*>&  order)
{
    out << "void task_entry(TaskContext *ctx) {\n";

    // ── 1. Typed local pointers for every node ────────────────────────────────
    for (const Node* n : order) {
        const std::string sname = struct_name_for_node(n);
        out << "    " << sname << " *" << n->instance
            << " = S_" << n->instance << "(ctx);\n";
    }
    out << "\n";

    // ── 2. Execute each block in topological order ────────────────────────────
    for (const Node* n : order) {
        const BlockTemplate* tmpl = find_template(n->type);
        if (!tmpl) continue;

        out << "    " << tmpl->entry_func << "(" << n->instance << ");\n";
    }
    out << "\n";

    // ── 3. Wiring: propagate outputs to downstream inputs ─────────────────────
    for (const auto& rl : resolved) {
        out << "    " << rl.dst_instance << "->" << rl.dst_pin_name
            << " = "  << rl.src_instance << "->" << rl.src_pin_name << ";\n";
    }

    out << "}\n";
}

} // namespace signal_forge