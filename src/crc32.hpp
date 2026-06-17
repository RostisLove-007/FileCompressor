#pragma once
#include <cstdint>
#include <array>
#include <vector>

// ─────────────────────────────────────────────────────────────
//  CRC-32 (полином IEEE 802.3)
// ─────────────────────────────────────────────────────────────
namespace detail {
    constexpr std::array<uint32_t,256> make_crc_table() noexcept {
        std::array<uint32_t,256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int j = 0; j < 8; ++j)
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
        return t;
    }
    static constexpr auto CRC_TABLE = make_crc_table();
}

class CRC32 {
    static constexpr auto& TABLE = detail::CRC_TABLE;

    uint32_t crc_ = 0xFFFFFFFFu;

public:
    void update(const uint8_t* d, size_t n) noexcept {
        for (size_t i = 0; i < n; ++i)
            crc_ = TABLE[(crc_ ^ d[i]) & 0xFF] ^ (crc_ >> 8);
    }
    void update(const std::vector<uint8_t>& v) noexcept { update(v.data(), v.size()); }
    uint32_t value()  const noexcept { return crc_ ^ 0xFFFFFFFFu; }
    void     reset()        noexcept { crc_ = 0xFFFFFFFFu; }

    static uint32_t compute(const uint8_t* d, size_t n) noexcept {
        CRC32 c; c.update(d, n); return c.value();
    }
    static uint32_t compute(const std::vector<uint8_t>& v) noexcept {
        return compute(v.data(), v.size());
    }

    // Инкрементальный CRC: накапливать через update(), завершить через finalize()
    static uint32_t update(uint32_t accum, const uint8_t* d, size_t n) noexcept {
        for (size_t i = 0; i < n; ++i)
            accum = TABLE[(accum ^ d[i]) & 0xFF] ^ (accum >> 8);
        return accum;
    }
    static uint32_t finalize(uint32_t accum) noexcept {
        return accum ^ 0xFFFFFFFFu;
    }
};
