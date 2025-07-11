#ifndef CREDITS_ARRAY_HASH_HPP
#define CREDITS_ARRAY_HASH_HPP

#include <array>
#include <cstdint>
#include <functional>

// Only provide hash specialization if it doesn't already exist
#ifndef STD_ARRAY_UINT8_32_HASH_DEFINED
#define STD_ARRAY_UINT8_32_HASH_DEFINED

namespace std {
    template<>
    struct hash<std::array<uint8_t, 32>> {
        size_t operator()(const std::array<uint8_t, 32>& arr) const noexcept {
            size_t result = 0;
            for (size_t i = 0; i < arr.size(); ++i) {
                result ^= std::hash<uint8_t>{}(arr[i]) + 0x9e3779b9 + (result << 6) + (result >> 2);
            }
            return result;
        }
    };
}

#endif // STD_ARRAY_UINT8_32_HASH_DEFINED
#endif // CREDITS_ARRAY_HASH_HPP