#include "signal_stream_app/source_config_propgrid_registry.h"
#include <fstream>
#include <algorithm>

// ═══════════════════════════════════════════════════════
//  Construction
// ═══════════════════════════════════════════════════════

PropertyTypeRegistry::PropertyTypeRegistry()
{
    LoadDefaults();
}

// ═══════════════════════════════════════════════════════
//  Hardcoded defaults (temporary, replace with JSON)
// ═══════════════════════════════════════════════════════

void PropertyTypeRegistry::LoadDefaults()
{
    // ─── SignalForge: source metadata ─────────────────
    {
        PropertyHint h;

        h = { PropertyHint::Type::String };
        h.tooltip = "Remote host address";
        hints_["SignalForge"]["host"] = h;

        h = { PropertyHint::Type::Int };
        h.min_val = 1; h.max_val = 65535;
        h.tooltip = "TCP port for data stream";
        hints_["SignalForge"]["data_port"] = h;

        h = { PropertyHint::Type::Int };
        h.min_val = 1; h.max_val = 65535;
        h.tooltip = "TCP port for configuration";
        hints_["SignalForge"]["config_port"] = h;

        h = { PropertyHint::Type::Int };
        h.min_val = 1; h.max_val = 60000; h.step = 100;
        h.tooltip = "Poll interval in milliseconds";
        hints_["SignalForge"]["interval"] = h;

        h = { PropertyHint::Type::Int };
        h.min_val = 0; h.max_val = 1000;
        h.tooltip = "Subscription identifier";
        hints_["SignalForge"]["sub_id"] = h;
    }

    // ─── SignalForge: signal metadata ─────────────────
    {
        PropertyHint h;

        h = { PropertyHint::Type::Int };
        h.min_val = 0; h.max_val = 9999;
        signal_hints_["SignalForge"]["field_id"] = h;

        h = { PropertyHint::Type::Enum };
        h.choices = { "f32", "f64", "i16", "i32", "u16", "u32", "bool" };
        signal_hints_["SignalForge"]["field_type"] = h;

        h = { PropertyHint::Type::Enum };
        h.choices = { "RO", "RW", "WO" };
        signal_hints_["SignalForge"]["access"] = h;
    }

    // ─── OpcUa: source metadata ──────────────────────
    {
        PropertyHint h;

        h = { PropertyHint::Type::String };
        h.tooltip = "OPC-UA server endpoint";
        hints_["OpcUa"]["endpoint_url"] = h;

        h = { PropertyHint::Type::Enum };
        h.choices = { "None", "Sign", "SignAndEncrypt" };
        hints_["OpcUa"]["security_mode"] = h;

        h = { PropertyHint::Type::Float };
        h.min_val = 100.0; h.max_val = 60000.0; h.step = 100.0;
        h.tooltip = "Publish interval in milliseconds";
        hints_["OpcUa"]["publish_interval"] = h;

        h = { PropertyHint::Type::Bool };
        h.tooltip = "Enable TLS encryption";
        hints_["OpcUa"]["use_tls"] = h;
    }

    // ─── OpcUa: signal metadata ──────────────────────
    {
        PropertyHint h;

        h = { PropertyHint::Type::String };
        h.tooltip = "OPC-UA node identifier";
        signal_hints_["OpcUa"]["node_id"] = h;

        h = { PropertyHint::Type::Int };
        h.min_val = 0; h.max_val = 100;
        signal_hints_["OpcUa"]["namespace"] = h;

        h = { PropertyHint::Type::Float };
        h.min_val = 0.0; h.max_val = 1000.0; h.step = 0.1;
        signal_hints_["OpcUa"]["deadband"] = h;
    }

    // ─── MQTT: source metadata ────────────────────────
    {
        PropertyHint h;

        h = { PropertyHint::Type::String };
        h.tooltip = "MQTT broker address";
        hints_["MQTT"]["broker"] = h;

        h = { PropertyHint::Type::Int };
        h.min_val = 1; h.max_val = 65535;
        hints_["MQTT"]["port"] = h;

        h = { PropertyHint::Type::String };
        hints_["MQTT"]["topic"] = h;

        h = { PropertyHint::Type::Enum };
        h.choices = { "0", "1", "2" };
        h.tooltip = "Quality of Service level";
        hints_["MQTT"]["qos"] = h;

        h = { PropertyHint::Type::Bool };
        hints_["MQTT"]["retain"] = h;

        h = { PropertyHint::Type::Int };
        h.min_val = 1; h.max_val = 3600; h.step = 10;
        h.tooltip = "Keep-alive interval in seconds";
        hints_["MQTT"]["keepalive"] = h;
    }

    // ─── MQTT: signal metadata ────────────────────────
    {
        PropertyHint h;

        h = { PropertyHint::Type::String };
        h.tooltip = "JSONPath to extract value";
        signal_hints_["MQTT"]["json_path"] = h;

        h = { PropertyHint::Type::Enum };
        h.choices = { "f32", "f64", "i32", "string", "bool" };
        signal_hints_["MQTT"]["data_type"] = h;
    }
}

// ═══════════════════════════════════════════════════════
//  Registration
// ═══════════════════════════════════════════════════════

void PropertyTypeRegistry::RegisterHint(const std::string& sourceType,
                                         const std::string& key,
                                         PropertyHint hint)
{
    hints_[sourceType][key] = std::move(hint);
}

void PropertyTypeRegistry::RegisterSignalHint(const std::string& sourceType,
                                               const std::string& key,
                                               PropertyHint hint)
{
    signal_hints_[sourceType][key] = std::move(hint);
}

// ═══════════════════════════════════════════════════════
//  Lookup
// ═══════════════════════════════════════════════════════

const PropertyHint* PropertyTypeRegistry::GetHint(
    const std::string& sourceType,
    const std::string& key) const
{
    auto typeIt = hints_.find(sourceType);
    if (typeIt == hints_.end()) return nullptr;
    auto keyIt = typeIt->second.find(key);
    return keyIt != typeIt->second.end() ? &keyIt->second : nullptr;
}

const PropertyHint* PropertyTypeRegistry::GetSignalHint(
    const std::string& sourceType,
    const std::string& key) const
{
    auto typeIt = signal_hints_.find(sourceType);
    if (typeIt == signal_hints_.end()) return nullptr;
    auto keyIt = typeIt->second.find(key);
    return keyIt != typeIt->second.end() ? &keyIt->second : nullptr;
}

// ═══════════════════════════════════════════════════════
//  Query
// ═══════════════════════════════════════════════════════

bool PropertyTypeRegistry::HasType(const std::string& sourceType) const
{
    return hints_.count(sourceType) || signal_hints_.count(sourceType);
}

std::vector<std::string> PropertyTypeRegistry::GetRegisteredTypes() const
{
    std::unordered_set<std::string> types;
    for (const auto& [key, _] : hints_)        types.insert(key);
    for (const auto& [key, _] : signal_hints_) types.insert(key);
    return { types.begin(), types.end() };
}

// ═══════════════════════════════════════════════════════
//  Clear
// ═══════════════════════════════════════════════════════

void PropertyTypeRegistry::Clear()
{
    hints_.clear();
    signal_hints_.clear();
}

// ═══════════════════════════════════════════════════════
//  File loading (stubs for now)
// ═══════════════════════════════════════════════════════

void PropertyTypeRegistry::LoadSourceFile(const std::filesystem::path& /*path*/)
{
    // TODO: implement JSON loading
}

void PropertyTypeRegistry::LoadDirectory(const std::filesystem::path& /*dir*/)
{
    // TODO: implement directory scanning
}