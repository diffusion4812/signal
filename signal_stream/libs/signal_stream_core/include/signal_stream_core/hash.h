#pragma once

#include <cstdint>
#include <string_view>

namespace signal_stream {

    constexpr uint32_t fnv1a_32(std::string_view s) {
        uint32_t hash = 2166136261u;
        for (char c : s) {
            hash ^= static_cast<uint8_t>(c);
            hash *= 16777619u;
        }
        return hash;
    }

}
