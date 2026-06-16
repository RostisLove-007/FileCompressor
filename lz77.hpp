#pragma once
#include <vector>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <algorithm>
#include <unordered_map>

// ═══════════════════════════════════════════════════════════════
//  LZ77 — скользящее окно с hash-цепочками
//
//  Формат закодированных данных:
//   [4 байта BE] размер оригинала
//   Токены (флаг-байт + данные, как в LZSS):
//     флаг-байт: 8 бит, каждый = тип токена (0=литерал, 1=ссылка)
//     литерал:  1 байт
//     ссылка:   3 байта — [2 байта BE dist][1 байт len-MIN_MATCH]
// ═══════════════════════════════════════════════════════════════
class LZ77Coder {
    static constexpr uint32_t MIN_MATCH = 3;
    static constexpr uint32_t MAX_MATCH = 258;

    uint32_t win_size_;

    struct Match { uint32_t dist = 0, len = 0; };

    Match find_match(const std::vector<uint8_t>& src, uint32_t pos) const {
        Match best;
        uint32_t look = std::min<uint32_t>(MAX_MATCH, static_cast<uint32_t>(src.size()) - pos);
        uint32_t start = (pos >= win_size_) ? pos - win_size_ : 0;

        for (uint32_t i = start; i < pos; ++i) {
            uint32_t len = 0;
            while (len < look && src[i + len] == src[pos + len]) ++len;
            if (len >= MIN_MATCH && len > best.len)
                best = {pos - i, len};
        }
        return best;
    }

public:
    explicit LZ77Coder(uint32_t win_size = 32768) : win_size_(win_size) {}

    // ── КОДИРОВАНИЕ ──────────────────────────────────────────
    std::vector<uint8_t> encode(const std::vector<uint8_t>& in) {
        std::vector<uint8_t> out;
        out.reserve(in.size());

        // Размер оригинала (4 байта BE)
        uint32_t orig = static_cast<uint32_t>(in.size());
        out.push_back((orig >> 24) & 0xFF);
        out.push_back((orig >> 16) & 0xFF);
        out.push_back((orig >>  8) & 0xFF);
        out.push_back( orig        & 0xFF);

        uint32_t pos = 0;
        while (pos < orig) {
            // Накапливаем до 8 токенов
            uint8_t flags = 0;
            std::vector<uint8_t> group;

            for (int bit = 7; bit >= 0 && pos < orig; --bit) {
                Match m = find_match(in, pos);
                if (m.len >= MIN_MATCH) {
                    flags |= static_cast<uint8_t>(1u << bit);
                    group.push_back(static_cast<uint8_t>(m.dist >> 8));
                    group.push_back(static_cast<uint8_t>(m.dist & 0xFF));
                    group.push_back(static_cast<uint8_t>(m.len - MIN_MATCH));
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
            throw std::runtime_error("LZ77: слишком короткие данные");

        uint32_t orig = ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16)
                      | ((uint32_t)in[2] <<  8) |  (uint32_t)in[3];

        std::vector<uint8_t> out;
        out.reserve(orig);

        size_t pos = 4;
        while (pos < in.size() && out.size() < orig) {
            uint8_t flags = in[pos++];

            for (int bit = 7; bit >= 0 && pos < in.size() && out.size() < orig; --bit) {
                if (flags & (1u << bit)) {
                    // Ссылка: 3 байта
                    if (pos + 3 > in.size())
                        throw std::runtime_error("LZ77: усечённая ссылка");
                    uint32_t dist = ((uint32_t)in[pos] << 8) | in[pos+1];
                    uint32_t len  = in[pos+2] + MIN_MATCH;
                    pos += 3;

                    if (dist == 0 || dist > out.size())
                        throw std::runtime_error("LZ77: недопустимое расстояние");

                    size_t start = out.size() - dist;
                    for (uint32_t j = 0; j < len && out.size() < orig; ++j)
                        out.push_back(out[start + j]);
                } else {
                    out.push_back(in[pos++]);
                }
            }
        }

        if (out.size() != orig)
            throw std::runtime_error("LZ77: несоответствие размера");
        return out;
    }
};
