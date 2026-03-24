#pragma once

#include <boost/json.hpp>
#include "signal_forge_graph/graph.h"
#include <memory>
#include <vector>

namespace signal_forge {

namespace json = boost::json;

// ─── FieldType ────────────────────────────────────────

void      tag_invoke(json::value_from_tag, json::value& jv, Field::FieldType t);
Field::FieldType tag_invoke(json::value_to_tag<Field::FieldType>, const json::value& jv);

// ─── PinDirection ─────────────────────────────────────

void         tag_invoke(json::value_from_tag, json::value& jv, PinDirection d);
PinDirection tag_invoke(json::value_to_tag<PinDirection>, const json::value& jv);

// ─── Field ────────────────────────────────────────────

void  tag_invoke(json::value_from_tag, json::value& jv, const Field& f);
Field tag_invoke(json::value_to_tag<Field>, const json::value& jv);

// ─── Pin ──────────────────────────────────────────────

void tag_invoke(json::value_from_tag, json::value& jv, const Pin& p);
Pin  tag_invoke(json::value_to_tag<Pin>, const json::value& jv);

// ─── Static ───────────────────────────────────────────

void   tag_invoke(json::value_from_tag, json::value& jv, const Static& s);
Static tag_invoke(json::value_to_tag<Static>, const json::value& jv);

// ─── Node helpers ─────────────────────────────────────

void node_to_json(json::object& obj, const Node& n);
void node_from_json(const json::object& obj, Node& n);

// ─── Node ─────────────────────────────────────────────

void tag_invoke(json::value_from_tag, json::value& jv, const Node& n);

// ─── ProbeNode ────────────────────────────────────────

void      tag_invoke(json::value_from_tag, json::value& jv, const ProbeNode& n);
ProbeNode tag_invoke(json::value_to_tag<ProbeNode>, const json::value& jv);

// ─── InjectNode ───────────────────────────────────────

void       tag_invoke(json::value_from_tag, json::value& jv, const InjectNode& n);
InjectNode tag_invoke(json::value_to_tag<InjectNode>, const json::value& jv);

// ─── Link ─────────────────────────────────────────────

void tag_invoke(json::value_from_tag, json::value& jv, const Link& l);
Link tag_invoke(json::value_to_tag<Link>, const json::value& jv);

// ─── Graph-level serialization ────────────────────────

json::value serialize_graph(const Graph& graph);
void deserialize_graph(const boost::json::value& jv, Graph& graph);

} // namespace signal_forge