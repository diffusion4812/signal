// MetadataHelpers.h
#pragma once

#include <string>
#include <unordered_map>
#include <optional>

namespace meta_util {

using MetaMap = std::unordered_map<std::string, std::string>;

inline std::string GetString(const MetaMap& meta,
                             const std::string& key,
                             const std::string& fallback = "")
{
    auto it = meta.find(key);
    return it != meta.end() ? it->second : fallback;
}

inline int GetInt(const MetaMap& meta,
                  const std::string& key,
                  int fallback = 0)
{
    auto it = meta.find(key);
    if (it == meta.end()) return fallback;
    try { return std::stoi(it->second); }
    catch (...) { return fallback; }
}

inline double GetDouble(const MetaMap& meta,
                        const std::string& key,
                        double fallback = 0.0)
{
    auto it = meta.find(key);
    if (it == meta.end()) return fallback;
    try { return std::stod(it->second); }
    catch (...) { return fallback; }
}

inline bool GetBool(const MetaMap& meta,
                    const std::string& key,
                    bool fallback = false)
{
    auto it = meta.find(key);
    if (it == meta.end()) return fallback;
    return it->second == "true" || it->second == "1";
}

inline void Set(MetaMap& meta,
                const std::string& key,
                const std::string& value)
{
    meta[key] = value;
}

inline void Set(MetaMap& meta,
                const std::string& key,
                int value)
{
    meta[key] = std::to_string(value);
}

inline void Set(MetaMap& meta,
                const std::string& key,
                double value)
{
    meta[key] = std::to_string(value);
}

inline void Set(MetaMap& meta,
                const std::string& key,
                bool value)
{
    meta[key] = value ? "true" : "false";
}

} // namespace meta_util
