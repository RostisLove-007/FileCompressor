#pragma once
#include <vector>
#include <cstdint>
#include <array>

// ═══════════════════════════════════════════════════════════════
//  CRC32 — контрольная сумма (полином 0xEDB88320, IEEE 802.3)
// ═══════════════════════════════════════════════════════════════

// Таблица вынесена за пределы класса — это необходимо для
// корректной инициализации constexpr до первого использования
namespace detail {
    constexpr std::array<uint32_t,256> make_crc_table() {
        std::array<uint32_t,256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int j = 0; j < 8; ++j)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
        return t;
    }
    inline constexpr auto CRC_TABLE = make_crc_table();
}

class CRC32 {
public:
    static uint32_t compute(const uint8_t* d, size_t n) noexcept {
        uint32_t crc = 0xFFFFFFFFu;
        for (size_t i = 0; i < n; ++i)
            crc = detail::CRC_TABLE[(crc ^ d[i]) & 0xFF] ^ (crc >> 8);
        return crc ^ 0xFFFFFFFFu;
    }
    static uint32_t compute(const std::vector<uint8_t>& v) noexcept {
        return compute(v.data(), v.size());
    }
    static uint32_t update(uint32_t acc, const uint8_t* d, size_t n) noexcept {
        for (size_t i = 0; i < n; ++i)
            acc = detail::CRC_TABLE[(acc ^ d[i]) & 0xFF] ^ (acc >> 8);
        return acc;
    }
    static uint32_t finalize(uint32_t acc) noexcept { return acc ^ 0xFFFFFFFFu; }
};
