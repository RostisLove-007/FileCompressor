#pragma once
#include <vector>
#include <cstdint>
#include <stdexcept>
#include <algorithm>
#include <unordered_map>

// ═══════════════════════════════════════════════════════════════
//  LZSS – флаговые биты сгруппированы в управляющие байты
//
//  Формат: [8 байт orig_size] затем группы:
//    [1 байт ctrl] – каждый бит (MSB first):
//        0 → литерал (1 байт следом)
//        1 → ссылка  (2 байта: смещение + длина упакованы)
//  Смещение 12 бит (окно 4 KB), длина 4 бита (3..18)
// ═══════════════════════════════════════════════════════════════
class LZSSCoder {
    static constexpr uint32_t WIN    = 4096;     // размер окна
    static constexpr uint32_t MAX_LN = 18;       // макс. длина совпадения
    static constexpr uint32_t MIN_LN = 3;        // мин. длина совпадения

    using HashMap = std::unordered_map<uint32_t, std::vector<uint32_t>>;

    static uint32_t hash3(const uint8_t* p) noexcept {
        return (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2];
    }

    static std::pair<uint32_t,uint32_t> find_match(
        const std::vector<uint8_t>& data,
        uint32_t pos,
        HashMap& chains) noexcept
    {
        uint32_t n = static_cast<uint32_t>(data.size());
        uint32_t look = std::min<uint32_t>(MAX_LN, n - pos);
        if (look < MIN_LN) return {0,0};

        uint32_t h = hash3(data.data() + pos);
        auto it = chains.find(h);
        if (it == chains.end()) return {0,0};

        uint32_t best_len = 0, best_dist = 0;
        for (auto jt = it->second.rbegin(); jt != it->second.rend(); ++jt) {
            uint32_t cand = *jt;
            if (pos - cand > WIN || pos - cand == 0) continue;
            uint32_t len = 0;
            while (len < look && data[pos+len] == data[cand+len]) ++len;
            if (len > best_len) { best_len = len; best_dist = pos - cand; }
            if (best_len >= MAX_LN) break;
        }
        if (best_len < MIN_LN) return {0,0};
        return {best_dist, best_len};
    }

public:
    // ── КОДИРОВАНИЕ ──────────────────────────────────────────
    std::vector<uint8_t> encode(const std::vector<uint8_t>& in) {
        std::vector<uint8_t> out;
        out.reserve(in.size());

        // Исходный размер
        uint64_t orig = in.size();
        for (int i = 7; i >= 0; --i) out.push_back((orig >> (i*8)) & 0xFF);

        HashMap chains;
        uint32_t pos = 0;
        uint32_t n   = static_cast<uint32_t>(in.size());

        while (pos < n) {
            // Собираем группу из 8 токенов
            uint8_t  ctrl  = 0;
            size_t   cpos  = out.size();    // позиция ctrl байта
            out.push_back(0);               // placeholder

            for (int bit = 7; bit >= 0 && pos < n; --bit) {
                auto [dist, len] = find_match(in, pos, chains);

                if (len >= MIN_LN) {
                    ctrl |= (1u << bit);
                    // 12 бит смещение, 4 бита (длина-MIN_LN)
                    uint16_t enc = static_cast<uint16_t>(
                        ((dist - 1) & 0xFFF) << 4 | ((len - MIN_LN) & 0xF));
                    out.push_back(enc >> 8);
                    out.push_back(enc & 0xFF);

                    for (uint32_t i = 0; i < len; ++i) {
                        if (pos + i + 2 < n) {
                            uint32_t h = hash3(in.data() + pos + i);
                            chains[h].push_back(pos + i);
                            if (chains[h].size() > 16)
                                chains[h].erase(chains[h].begin());
                        }
                    }
                    pos += len;
                } else {
                    // Литерал
                    out.push_back(in[pos]);
                    if (pos + 2 < n) {
                        uint32_t h = hash3(in.data() + pos);
                        chains[h].push_back(pos);
                        if (chains[h].size() > 16)
                            chains[h].erase(chains[h].begin());
                    }
                    ++pos;
                }
            }
            out[cpos] = ctrl;
        }
        return out;
    }

    // ── ДЕКОДИРОВАНИЕ ────────────────────────────────────────
    std::vector<uint8_t> decode(const std::vector<uint8_t>& in) {
        if (in.size() < 8)
            throw std::runtime_error("LZSS: слишком короткий заголовок");

        uint64_t orig = 0;
        for (int i = 0; i < 8; ++i) orig = (orig << 8) | in[i];

        std::vector<uint8_t> out;
        out.reserve(static_cast<size_t>(orig));

        size_t i = 8;
        while (i < in.size() && out.size() < orig) {
            uint8_t ctrl = in[i++];
            for (int bit = 7; bit >= 0 && out.size() < orig; --bit) {
                if ((ctrl >> bit) & 1) {
                    // Ссылка: 2 байта
                    if (i + 1 >= in.size())
                        throw std::runtime_error("LZSS: усечённая ссылка");
                    uint16_t enc  = (uint16_t(in[i]) << 8) | in[i+1];
                    i += 2;
                    uint32_t dist = ((enc >> 4) & 0xFFF) + 1;
                    uint32_t len  = (enc & 0xF) + MIN_LN;

                    if (dist > out.size())
                        throw std::runtime_error("LZSS: недопустимое расстояние");
                    uint32_t start = static_cast<uint32_t>(out.size()) - dist;
                    for (uint32_t j = 0; j < len && out.size() < orig; ++j)
                        out.push_back(out[start + j]);
                } else {
                    // Литерал: 1 байт
                    if (i >= in.size())
                        throw std::runtime_error("LZSS: усечённый литерал");
                    out.push_back(in[i++]);
                }
            }
        }

        if (out.size() != orig)
            throw std::runtime_error("LZSS: несоответствие размера");
        return out;
    }
};
