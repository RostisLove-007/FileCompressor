#pragma once
#include <vector>
#include <cstdint>
#include <stdexcept>
#include <algorithm>

// ═══════════════════════════════════════════════════════════════
//  LZSS — оптимизированный LZ77
//
//  Отличие от LZ77: литерал хранится «как есть» без флага в потоке,
//  а перед каждой группой из 8 токенов идёт байт флагов:
//    бит=0 → следующий байт — литерал
//    бит=1 → следующие 2 байта — ссылка (dist 12 бит, len 4 бита)
//
//  Ссылка (2 байта):
//    [15:4] — dist-1 (0..4094 → dist 1..4095)
//    [3:0]  — len-MIN_MATCH (0..15 → len 3..18)
//
//  Формат файла:
//    [4 байта BE] размер оригинала
//    [флаг-байт + данные токенов]*
// ═══════════════════════════════════════════════════════════════
class LZSSCoder {
    static constexpr uint32_t WIN       = 4096;
    static constexpr uint32_t MIN_MATCH = 3;
    static constexpr uint32_t MAX_MATCH = 18;

    struct Match { uint32_t dist = 0, len = 0; };

    static Match find_match(const uint8_t* buf, uint32_t pos,
                            uint32_t size) noexcept {
        Match best;
        uint32_t start = (pos >= WIN) ? pos - WIN : 0;
        uint32_t look  = std::min<uint32_t>(MAX_MATCH, size - pos);

        for (uint32_t i = start; i < pos; ++i) {
            uint32_t len = 0;
            while (len < look && buf[i + len] == buf[pos + len]) ++len;
            if (len >= MIN_MATCH && len > best.len)
                best = {pos - i, len};
        }
        return best;
    }

public:
    // ── КОДИРОВАНИЕ ──────────────────────────────────────────
    std::vector<uint8_t> encode(const std::vector<uint8_t>& in) {
        std::vector<uint8_t> out;
        out.reserve(in.size());

        uint32_t orig = static_cast<uint32_t>(in.size());
        out.push_back((orig >> 24) & 0xFF);
        out.push_back((orig >> 16) & 0xFF);
        out.push_back((orig >>  8) & 0xFF);
        out.push_back( orig        & 0xFF);

        uint32_t pos = 0;
        while (pos < orig) {
            uint8_t flags = 0;
            std::vector<uint8_t> group;

            for (int bit = 7; bit >= 0 && pos < orig; --bit) {
                Match m = find_match(in.data(), pos, orig);
                if (m.len >= MIN_MATCH) {
                    flags |= static_cast<uint8_t>(1u << bit);
                    uint16_t ref = static_cast<uint16_t>(
                        (((m.dist - 1) & 0xFFF) << 4) |
                        ((m.len - MIN_MATCH) & 0xF));
                    group.push_back(static_cast<uint8_t>(ref >> 8));
                    group.push_back(static_cast<uint8_t>(ref & 0xFF));
                    pos += m.len;
                } else {
                    group.push_back(in[pos++]);
                }
            }
            out.push_back(flags);
            out.insert(out.end(), group.begin(), group.end());
        }
        return out;
    }

    // ── ДЕКОДИРОВАНИЕ ────────────────────────────────────────
    std::vector<uint8_t> decode(const std::vector<uint8_t>& in) {
        if (in.size() < 4)
            throw std::runtime_error("LZSS: слишком короткие данные");

        uint32_t orig = ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16)
                      | ((uint32_t)in[2] <<  8) |  (uint32_t)in[3];

        std::vector<uint8_t> out;
        out.reserve(orig);

        size_t pos = 4;
        while (pos < in.size() && out.size() < orig) {
            uint8_t flags = in[pos++];

            for (int bit = 7; bit >= 0 && pos < in.size() && out.size() < orig; --bit) {
                if (flags & (1u << bit)) {
                    if (pos + 2 > in.size())
                        throw std::runtime_error("LZSS: усечённая ссылка");
                    uint16_t ref  = static_cast<uint16_t>((in[pos] << 8) | in[pos+1]);
                    pos += 2;
                    uint32_t dist = static_cast<uint32_t>(ref >> 4) + 1;
                    uint32_t len  = static_cast<uint32_t>(ref & 0xF) + MIN_MATCH;

                    if (dist > out.size())
                        throw std::runtime_error("LZSS: недопустимое расстояние");

                    size_t start = out.size() - dist;
                    for (uint32_t j = 0; j < len && out.size() < orig; ++j)
                        out.push_back(out[start + j]);
                } else {
                    out.push_back(in[pos++]);
                }
            }
        }

        if (out.size() != orig)
            throw std::runtime_error("LZSS: несоответствие размера");
        return out;
    }
};
