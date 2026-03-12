#pragma once

#include <cstddef>
#include <cstdint>

inline void esp_fill_random(void* buffer, size_t length) {
    auto* bytes = static_cast<std::uint8_t*>(buffer);
    static std::uint32_t state = 0x4F3C2D1Bu;
    for (size_t i = 0; i < length; ++i) {
        state = state * 1664525u + 1013904223u;
        bytes[i] = static_cast<std::uint8_t>((state >> 24) & 0xFF);
    }
}

