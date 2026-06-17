#pragma once
#include <vector>
#include <cstdint>
#include <stdexcept>
#include <unordered_map>
#include <algorithm>
#include "bit_io.hpp"

// ═══════════════════════════════════════════════════════════════
//  LZ77 со скользящим окном
//
//  Формат сжатых данных:
//   [8 байт] исходный размер (big-endian uint64)
//   [4 байт] размер окна (big-endian uint32)
//   [2 байт] максимальная длина совпадения
//   затем токены:
//     бит 0  → литерал: 8 бит символ
//     бит 1  → совпадение: WBits бит смещение + LBits бит длина
//   [1 байт] в конце: число бит-заполнителей
// ═══════════════════════════════════════════════════════════════
class LZ77Coder {
    uint32_t win_size_  = 32768;   // размер окна (1–65536)
    uint16_t max_match_ = 258;     // максимальная длина совпадения
    uint16_t min_match_ = 3;       // минимальная длина (иначе литерал выгоднее)

    int win_bits() const {
        int b = 0; uint32_t w = win_size_ - 1;
        while (w) { ++b; w >>= 1; }
        return b ? b : 1;
    }

    int len_bits() const {
        int b = 0; uint16_t m = max_match_ - min_match_;
        while (m) { ++b; m >>= 1; }
        return b ? b : 1;
    }

    // ── Хэш-цепочки для быстрого поиска ─────────────────────
    using HashMap = std::unordered_map<uint32_t, std::vector<uint32_t>>;

    static uint32_t hash3(const uint8_t* p) {
        return (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2];
    }

    // Ищет наилучшее совпадение; возвращает (distance, length)
    std::pair<uint32_t,uint32_t> find_match(
        const std::vector<uint8_t>& data,
        uint32_t pos,
        HashMap& chains) const
    {
        uint32_t best_len = 0, best_dist = 0;
        uint32_t look_ahead = std::min<uint32_t>(max_match_, data.size() - pos);

        if (look_ahead < min_match_) return {0, 0};

        uint32_t h = hash3(data.data() + pos);
        auto it = chains.find(h);
        if (it == chains.end()) return {0, 0};

        for (auto jt = it->second.rbegin(); jt != it->second.rend(); ++jt) {
            uint32_t cand = *jt;
            if (pos - cand > win_size_) break;   // вышли за окно

            uint32_t len = 0;
            while (len < look_ahead && data[pos + len] == data[cand + len]) ++len;

            if (len > best_len) {
                best_len = len; best_dist = pos - cand;
                if (best_len >= max_match_) break;
            }
        }
        return {best_dist, best_len};
    }

public:
    explicit LZ77Coder(uint32_t window_size = 32768,
                       uint16_t max_len     = 258)
        : win_size_(window_size), max_match_(max_len) {}

    void set_window(uint32_t ws) { win_size_ = ws; }
    uint32_t window_size() const { return win_size_; }

    // ── КОДИРОВАНИЕ ──────────────────────────────────────────
    std::vector<uint8_t> encode(const std::vector<uint8_t>& in) {
        std::vector<uint8_t> out;
        out.reserve(in.size() / 2 + 64);

        // Заголовок
        uint64_t orig = in.size();
        for (int i = 7; i >= 0; --i) out.push_back((orig >> (i*8)) & 0xFF);
        for (int i = 3; i >= 0; --i) out.push_back((win_size_ >> (i*8)) & 0xFF);
        out.push_back((max_match_ >> 8) & 0xFF);
        out.push_back(max_match_ & 0xFF);

        size_t pad_pos = out.size();
        out.push_back(0);   // место под pad

        const int WB = win_bits();
        const int LB = len_bits();

        HashMap chains;
        BitWriter bw(out);
        uint32_t pos = 0;

        while (pos < in.size()) {
            auto [dist, len] = find_match(in, pos, chains);

            if (len >= min_match_) {
                bw.write_bit(1);
                bw.write_bits(dist - 1, WB);
                bw.write_bits(len - min_match_, LB);

                for (uint32_t i = 0; i < len; ++i) {
                    if (pos + i + 2 < in.size()) {
                        uint32_t h = hash3(in.data() + pos + i);
                        chains[h].push_back(pos + i);
                        // Ограничиваем длину цепочки
                        if (chains[h].size() > 8) chains[h].erase(chains[h].begin());
                    }
                }
                pos += len;
            } else {
                bw.write_bit(0);
                bw.write_bits(in[pos], 8);

                if (pos + 2 < in.size()) {
                    uint32_t h = hash3(in.data() + pos);
                    chains[h].push_back(pos);
                    if (chains[h].size() > 8) chains[h].erase(chains[h].begin());
                }
                ++pos;
            }
        }
        int pad = bw.flush();
        out[pad_pos] = static_cast<uint8_t>(pad);
        return out;
    }

    // ── ДЕКОДИРОВАНИЕ ────────────────────────────────────────
    std::vector<uint8_t> decode(const std::vector<uint8_t>& in) {
        if (in.size() < 15)
            throw std::runtime_error("LZ77: слишком короткий заголовок");

        uint64_t orig = 0;
        for (int i = 0; i < 8; ++i) orig = (orig << 8) | in[i];

        uint32_t win = 0;
        for (int i = 0; i < 4; ++i) win = (win << 8) | in[8 + i];

        uint16_t maxm = static_cast<uint16_t>((in[12] << 8) | in[13]);
        uint8_t  pad  = in[14];

        int WB = 0; { uint32_t w = win - 1; while (w) { ++WB; w >>= 1; } if (!WB) WB=1; }
        int LB = 0; { uint16_t m = maxm - min_match_; while (m) { ++LB; m >>= 1; } if (!LB) LB=1; }

        std::vector<uint8_t> out;
        out.reserve(static_cast<size_t>(orig));

        const uint8_t* data   = in.data() + 15;
        size_t         nbytes = in.size() - 15;
        size_t total_bits = nbytes * 8;
        if (total_bits < pad)
            throw std::runtime_error("LZ77: некорректный заголовок");
        total_bits -= pad;

        size_t bit = 0;
        int cb = 7; size_t cb_pos = 0;

        auto read_bit = [&]() -> bool {
            if (bit >= total_bits || cb_pos >= nbytes)
                throw std::runtime_error("LZ77: неожиданный конец данных");
            bool b = (data[cb_pos] >> cb) & 1;
            if (--cb < 0) { cb = 7; ++cb_pos; }
            ++bit;
            return b;
        };
        auto read_bits = [&](int n) -> uint64_t {
            uint64_t v = 0;
            for (int i = 0; i < n; ++i) v = (v << 1) | read_bit();
            return v;
        };

        while (out.size() < orig && bit < total_bits) {
            bool is_match = read_bit();
            if (is_match) {
                uint32_t dist = static_cast<uint32_t>(read_bits(WB)) + 1;
                uint32_t len  = static_cast<uint32_t>(read_bits(LB)) + min_match_;

                if (dist > out.size())
                    throw std::runtime_error("LZ77: недопустимое расстояние");

                uint32_t start = static_cast<uint32_t>(out.size()) - dist;
                for (uint32_t i = 0; i < len && out.size() < orig; ++i)
                    out.push_back(out[start + i]);
            } else {
                uint8_t sym = static_cast<uint8_t>(read_bits(8));
                out.push_back(sym);
            }
        }

        if (out.size() != orig)
            throw std::runtime_error("LZ77: несоответствие размера");
        return out;
    }
};
