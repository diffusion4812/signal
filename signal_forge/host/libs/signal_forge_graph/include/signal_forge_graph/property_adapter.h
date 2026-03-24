#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <typeindex>
#include <unordered_map>

namespace signal_forge {

// --- Property descriptor (extend as needed)
enum class PropType { Float, Int, Bool, String, UInt64 };

struct Property {
    uint64_t node_id;
    std::string name;
    PropType type;
    void* ptr;          // pointer to the member on the live object
    double min = 0.0;
    double max = 0.0;
    bool readOnly = false;
};

}