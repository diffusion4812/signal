#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <filesystem>

struct PropertyHint {
    enum class Type { String, Int, Float, Bool, Enum };

    Type type = Type::String;
    std::optional<double> min_val;
    std::optional<double> max_val;
    std::optional<double> step;
    std::vector<std::string> choices;
    bool read_only = false;
    std::string tooltip;
};

class PropertyTypeRegistry {
public:
    PropertyTypeRegistry();

    // ─── File loading (for later) ─────────────────────
    void LoadSourceFile(const std::filesystem::path& path);
    void LoadDirectory(const std::filesystem::path& dir);
    void Clear();

    // ─── Programmatic registration ────────────────────
    void RegisterHint(const std::string& sourceType,
                      const std::string& key,
                      PropertyHint hint);
    void RegisterSignalHint(const std::string& sourceType,
                            const std::string& key,
                            PropertyHint hint);

    // ─── Lookup ───────────────────────────────────────
    const PropertyHint* GetHint(const std::string& sourceType,
                                const std::string& key) const;
    const PropertyHint* GetSignalHint(const std::string& sourceType,
                                      const std::string& key) const;

    bool HasType(const std::string& sourceType) const;
    std::vector<std::string> GetRegisteredTypes() const;

private:
    void LoadDefaults();

    using HintMap = std::unordered_map<std::string, PropertyHint>;
    std::unordered_map<std::string, HintMap> hints_;
    std::unordered_map<std::string, HintMap> signal_hints_;
};