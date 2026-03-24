#ifndef SIGNAL_FORGE_GENERATOR_GENERATOR_H
#define SIGNAL_FORGE_GENERATOR_GENERATOR_H

#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <set>
#include <sstream>
#include <cstdint>

#include "signal_forge_graph/graph.h"
#include "task_host_core/manifest.h"

namespace signal_forge {

class Generator {
public:
    explicit Generator(const Graph& g);

    // ── Public generation result ─────────────────────────────────────────────
    struct Result {
        std::string              c_source;
        std::vector<std::string> link_headers;
        std::vector<std::string> link_objects;
    };

    Result generate();

private:
    // ── Resolved link (internal representation) ──────────────────────────────
    struct ResolvedLink {
        int         id;
        int         src_node_id;
        std::string src_instance;
        std::string src_pin_name;
        int         dst_node_id;
        std::string dst_instance;
        std::string dst_pin_name;
        int         from_pin_id;
        int         to_pin_id;
    };

    std::vector<ResolvedLink> resolve_links() const;

    // ── Initialisation helpers ───────────────────────────────────────────────
    void init_builtin_templates();      // defines sf_probe / sf_inject templates
    void init_block_library_map();
    void collect_required_blocks();
    void assign_slots();                // now covers ALL node types
    std::string sanitize_slot_name(const std::string& inst);
    uint32_t    magic_from_string(const std::string& s);

    // ── Template lookup (graph registry + built-ins) ─────────────────────────
    /// Returns pointer to BlockTemplate for any node type, checking the graph
    /// template registry first and the built-in table second.
    const BlockTemplate* find_template(const std::string& type) const;

    /// Convenience: look up the struct name for any node (probe → "sf_probe_t" etc.)
    std::string struct_name_for_node(const Node* n) const;

    // ── Code-emission helpers ────────────────────────────────────────────────
    void emit_task_host_core_header(std::ostringstream& out);
    void emit_includes             (std::ostringstream& out);

    /// Emits the self-contained typedef struct + static inline step function
    /// for every built-in block type that is actually used in the graph.
    void emit_builtin_defs         (std::ostringstream& out);

    void emit_block_enum           (std::ostringstream& out);
    void emit_sig_defines          (std::ostringstream& out);

    /// Emits PinInfo / FieldInfo tables for all nodes (pins + static fields).
    void emit_pin_info             (std::ostringstream& out);

    void emit_block_registry       (std::ostringstream& out);
    void emit_inline_structs       (std::ostringstream& out); // slot accessors

    /// Emits:
    ///   - sf_debug_map_t  struct with float* members for every probe/inject static
    ///   - sf_debug_map_init(TaskContext*)  — populates the pointers at startup
    ///   - sf_inject_msg_t / sf_handle_inject_msg()  — message handler called by
    ///     the host communication layer to force or release inject values
    void emit_debug_interface      (std::ostringstream& out);

    void emit_task_entry           (std::ostringstream& out,
                                    const std::vector<ResolvedLink>& resolved,
                                    const std::vector<const Node*>&  order);

    // ── Graph utilities ──────────────────────────────────────────────────────
    std::vector<const Node*> topological_order_from_resolved(
        const std::vector<ResolvedLink>& resolved) const;

    std::vector<const Link*> links_targeting_node(int nodeid) const;
    std::string              expr_from_pin_source(const Link& l) const;

    // ── Internal state ───────────────────────────────────────────────────────
    const Graph& graph_;

    /// Built-in block templates (sf_probe, sf_inject).
    /// Populated by init_builtin_templates() before any graph walk.
    std::map<std::string, BlockTemplate> builtin_templates_;

    /// Set of builtin type names actually present in the graph (used to decide
    /// which typedef / step-function bodies to emit).
    std::set<std::string> used_builtin_types_;

    struct BlockLibEntry { std::string header; std::string object; };
    std::map<std::string, BlockLibEntry> block_lib_map_;

    std::set<std::string> required_block_headers_;
    std::set<std::string> required_libs_;

    std::map<int, int>         slot_by_nodeid_;
    std::unordered_map<int, std::string> slot_name_by_nodeid_;
};

} // namespace signal_forge

#endif // SIGNAL_FORGE_GENERATOR_GENERATOR_H