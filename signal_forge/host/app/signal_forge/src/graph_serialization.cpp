#include "graph_serialization.h"

namespace signal_forge {

namespace json = boost::json;

// ─── FieldType ────────────────────────────────────────

inline void tag_invoke(json::value_from_tag, json::value& jv, Field::FieldType t) {
    switch (t) {
        case Field::FieldType::REAL: jv = "REAL"; break;
    }
}

inline Field::FieldType tag_invoke(json::value_to_tag<Field::FieldType>, const json::value& jv) {
    auto const& s = jv.as_string();
    if (s == "REAL") return Field::FieldType::REAL;
    throw std::runtime_error("Unknown FieldType: " + std::string(s));
}

// ─── PinDirection ─────────────────────────────────────

inline void tag_invoke(json::value_from_tag, json::value& jv, PinDirection d) {
    jv = (d == PinDirection::INPUT) ? "INPUT" : "OUTPUT";
}

inline PinDirection tag_invoke(json::value_to_tag<PinDirection>, const json::value& jv) {
    return (jv.as_string() == "INPUT") ? PinDirection::INPUT
                                       : PinDirection::OUTPUT;
}

// ─── Field ────────────────────────────────────────────

inline void tag_invoke(json::value_from_tag, json::value& jv, const Field& f) {
    jv = json::object{
        {"id",          f.id},
        {"name",        f.name},
        {"type",        json::value_from(f.type)},
        {"description", f.description},
        {"units",       f.units}
    };
}

inline Field tag_invoke(json::value_to_tag<Field>, const json::value& jv) {
    auto const& obj = jv.as_object();
    Field f;
    f.id          = static_cast<int>(obj.at("id").as_int64());
    f.name        = std::string(obj.at("name").as_string());
    f.type        = json::value_to<Field::FieldType>(obj.at("type"));
    f.description = std::string(obj.at("description").as_string());
    f.units       = std::string(obj.at("units").as_string());
    return f;
}

// ─── Pin ──────────────────────────────────────────────

inline void tag_invoke(json::value_from_tag, json::value& jv, const Pin& p) {
    // Start from base
    tag_invoke(json::value_from_tag{}, jv, static_cast<const Field&>(p));
    auto& obj = jv.as_object();
    obj["direction"]     = json::value_from(p.direction);
    obj["default_value"] = p.default_value;
}

inline Pin tag_invoke(json::value_to_tag<Pin>, const json::value& jv) {
    auto const& obj = jv.as_object();
    Pin p;
    static_cast<Field&>(p) = json::value_to<Field>(jv);
    p.direction     = json::value_to<PinDirection>(obj.at("direction"));
    p.default_value = obj.at("default_value").to_number<float>();
    return p;
}

// ─── Static ───────────────────────────────────────────

inline void tag_invoke(json::value_from_tag, json::value& jv, const Static& s) {
    tag_invoke(json::value_from_tag{}, jv, static_cast<const Field&>(s));
    auto& obj = jv.as_object();
    obj["default_value"] = s.default_value;
    obj["host_readable"] = s.host_readable;
    obj["host_writable"] = s.host_writable;
}

inline Static tag_invoke(json::value_to_tag<Static>, const json::value& jv) {
    auto const& obj = jv.as_object();
    Static s;
    static_cast<Field&>(s) = json::value_to<Field>(jv);
    s.default_value = obj.at("default_value").to_number<float>();
    s.host_readable = obj.at("host_readable").as_bool();
    s.host_writable = obj.at("host_writable").as_bool();
    return s;
}

// ─── Node (base) ──────────────────────────────────────

inline void node_to_json(json::object& obj, const Node& n) {
    obj["id"]       = n.id;
    obj["exec_id"]  = n.exec_id;
    obj["type"]     = n.type;
    obj["instance"] = n.instance;
    obj["x"]        = n.x;
    obj["y"]        = n.y;
    obj["inputs"]   = json::value_from(n.inputs);
    obj["outputs"]  = json::value_from(n.outputs);
    obj["statics"]  = json::value_from(n.statics);
}

inline void node_from_json(const json::object& obj, Node& n) {
    n.id       = static_cast<uint64_t>(obj.at("id").as_int64());
    n.exec_id  = static_cast<int>(obj.at("exec_id").as_int64());
    n.type     = std::string(obj.at("type").as_string());
    n.instance = std::string(obj.at("instance").as_string());
    n.x = obj.at("x").to_number<float>();
    n.y = obj.at("y").to_number<float>();
    n.inputs   = json::value_to<std::vector<Pin>>(obj.at("inputs"));
    n.outputs  = json::value_to<std::vector<Pin>>(obj.at("outputs"));
    n.statics  = json::value_to<std::vector<Static>>(obj.at("statics"));
}

inline void tag_invoke(json::value_from_tag, json::value& jv, const Node& n) {
    json::object obj;
    node_to_json(obj, n);
    obj["node_class"] = "Node";
    jv = std::move(obj);
}

// ─── ProbeNode ────────────────────────────────────────

inline void tag_invoke(json::value_from_tag, json::value& jv, const ProbeNode& n) {
    json::object obj;
    node_to_json(obj, n);
    obj["node_class"] = "ProbeNode";
    obj["value"]      = n.value;
    jv = std::move(obj);
}

inline ProbeNode tag_invoke(json::value_to_tag<ProbeNode>, const json::value& jv) {
    auto const& obj = jv.as_object();
    ProbeNode n;
    node_from_json(obj, n);
    n.value = obj.at("value").to_number<float>();
    return n;
}

// ─── InjectNode ───────────────────────────────────────

inline void tag_invoke(json::value_from_tag, json::value& jv, const InjectNode& n) {
    json::object obj;
    node_to_json(obj, n);
    obj["node_class"]    = "InjectNode";
    obj["observed_value"] = n.observed_value;
    obj["forcing_active"] = n.forcing_active;
    obj["forced_value"]   = n.forced_value;
    jv = std::move(obj);
}

inline InjectNode tag_invoke(json::value_to_tag<InjectNode>, const json::value& jv) {
    auto const& obj = jv.as_object();
    InjectNode n;
    node_from_json(obj, n);
    n.observed_value = obj.at("observed_value").to_number<float>();
    n.forcing_active = obj.at("forcing_active").as_bool();
    n.forced_value   = obj.at("forced_value").to_number<float>();
    return n;
}

// ─── Link ─────────────────────────────────────────────

inline void tag_invoke(json::value_from_tag, json::value& jv, const Link& l) {
    jv = json::object{
        {"id",       l.id},
        {"from_pin", l.from_pin},
        {"to_pin",   l.to_pin}
    };
}

inline Link tag_invoke(json::value_to_tag<Link>, const json::value& jv) {
    auto const& obj = jv.as_object();
    Link l;
    l.id       = static_cast<int>(obj.at("id").as_int64());
    l.from_pin = static_cast<int>(obj.at("from_pin").as_int64());
    l.to_pin   = static_cast<int>(obj.at("to_pin").as_int64());
    return l;
}

json::value serialize_graph(const Graph& graph)
{
    json::array node_array;
    for (const auto& ptr : graph.Nodes()) {
        json::value nj;
        if (auto* p = dynamic_cast<const ProbeNode*>(ptr.get()))
            nj = json::value_from(*p);
        else if (auto* inj = dynamic_cast<const InjectNode*>(ptr.get()))
            nj = json::value_from(*inj);
        else
            nj = json::value_from(*ptr);
        node_array.push_back(std::move(nj));
    }

    return json::object{
        {"nodes", std::move(node_array)},
        {"links", json::value_from(graph.Links())},
        {"next_id", graph.NextId()}
    };
}

void deserialize_graph(const boost::json::value& jv, Graph& graph)
{
    graph.ClearAll();

    auto const& obj = jv.as_object();

    for (const auto& nj : obj.at("nodes").as_array()) {
        auto cls = std::string(nj.as_object().at("node_class").as_string());

        if (cls == "ProbeNode") {
            graph.AddNode(std::make_unique<ProbeNode>(
                json::value_to<ProbeNode>(nj)));
        } else if (cls == "InjectNode") {
            graph.AddNode(std::make_unique<InjectNode>(
                json::value_to<InjectNode>(nj)));
        } else {
            auto n = std::make_unique<Node>();
            node_from_json(nj.as_object(), *n);
            graph.AddNode(std::move(n));
        }
    }

    for (const auto& lj : obj.at("links").as_array()) {
        graph.AddLink(json::value_to<Link>(lj));
    }

    graph.SetNextId(static_cast<int>(obj.at("next_id").as_int64()));
}

} // namespace signal_forge