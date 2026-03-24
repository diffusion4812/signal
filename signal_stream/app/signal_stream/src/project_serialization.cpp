#include "project_serialization.h"

namespace signal_stream {

namespace json = boost::json;

inline std::optional<std::string> to_string(const boost::json::value& v)
{
    if (v.is_string())  return std::string(v.as_string());
    if (v.is_int64())   return std::to_string(v.as_int64());
    if (v.is_uint64())  return std::to_string(v.as_uint64());
    if (v.is_double())  return std::to_string(v.as_double());
    if (v.is_bool())    return v.as_bool() ? "true" : "false";
    return std::nullopt;
}

inline std::unordered_map<std::string, std::string>
parse_flat_metadata(const boost::json::object& obj)
{
    std::unordered_map<std::string, std::string> result;
    for (const auto& [k, v] : obj) {
        if (auto s = to_string(v))
            result.emplace(std::string(k), std::move(*s));
    }
    return result;
}

// ─────────────────────────────────────────────────────────────
// SignalData
// ─────────────────────────────────────────────────────────────

SignalData tag_invoke(boost::json::value_to_tag<SignalData>,
                      const boost::json::value& jv)
{
    const auto& obj = jv.as_object();
    SignalData sig;

    if (auto it = obj.find("name"); it != obj.end() && it->value().is_string())
        sig.name = std::string(it->value().as_string());

    if (auto it = obj.find("type"); it != obj.end() && it->value().is_string())
        sig.type = std::string(it->value().as_string());

    if (auto it = obj.find("unit"); it != obj.end() && it->value().is_string())
        sig.unit = std::string(it->value().as_string());

    if (auto it = obj.find("metadata"); it != obj.end() && it->value().is_object())
        sig.metadata = parse_flat_metadata(it->value().as_object());

    return sig;
}

void tag_invoke(boost::json::value_from_tag,
                boost::json::value& jv,
                const SignalData& sig)
{
    auto& obj = jv.emplace_object();
    obj["name"] = sig.name;
    obj["type"] = sig.type;
    obj["unit"] = sig.unit;

    if (!sig.metadata.empty()) {
        auto& meta = obj["metadata"].emplace_object();
        for (const auto& [k, v] : sig.metadata)
            meta[k] = v;
    }
}

// ─────────────────────────────────────────────────────────────
// SourceData
// ─────────────────────────────────────────────────────────────

SourceData tag_invoke(boost::json::value_to_tag<SourceData>,
                      const boost::json::value& jv)
{
    const auto& obj = jv.as_object();
    SourceData src;

    if (auto it = obj.find("name"); it != obj.end() && it->value().is_string())
        src.name = std::string(it->value().as_string());

    if (auto it = obj.find("type"); it != obj.end() && it->value().is_string())
        src.type = std::string(it->value().as_string());

    if (auto it = obj.find("metadata"); it != obj.end() && it->value().is_object())
        src.metadata = parse_flat_metadata(it->value().as_object());

    if (auto it = obj.find("signals"); it != obj.end() && it->value().is_array())
        src.signals = boost::json::value_to<std::vector<SignalData>>(it->value());

    return src;
}

void tag_invoke(boost::json::value_from_tag,
                boost::json::value& jv,
                const SourceData& src)
{
    auto& obj = jv.emplace_object();
    obj["name"] = src.name;
    obj["type"] = src.type;

    if (!src.metadata.empty()) {
        auto& meta = obj["metadata"].emplace_object();
        for (const auto& [k, v] : src.metadata)
            meta[k] = v;
    }

    obj["signals"] = boost::json::value_from(src.signals);
}

// ─────────────────────────────────────────────────────────────
// ProjectData
// ─────────────────────────────────────────────────────────────

ProjectData tag_invoke(boost::json::value_to_tag<ProjectData>,
                       const boost::json::value& jv)
{
    const auto& obj = jv.as_object();
    ProjectData project;

    if (auto it = obj.find("name"); it != obj.end() && it->value().is_string())
        project.name = std::string(it->value().as_string());

    if (auto it = obj.find("sources"); it != obj.end() && it->value().is_array())
        project.sources = boost::json::value_to<std::vector<SourceData>>(it->value());

    return project;
}

void tag_invoke(boost::json::value_from_tag,
                boost::json::value& jv,
                const ProjectData& project)
{
    auto& obj = jv.emplace_object();
    obj["name"]    = project.name;
    obj["sources"] = boost::json::value_from(project.sources);
}

json::value serialize_project(const ProjectData& project)
{
    return boost::json::value_from(project);
}

void deserialize_project(const json::value& jv, ProjectData& project)
{
    project = boost::json::value_to<ProjectData>(jv);
}

} // namespace signal_stream