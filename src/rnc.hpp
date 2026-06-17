#pragma once
#include <vector>
#include <cstdint>
#include <stdexcept>
#include <algorithm>
#include <array>

// ═══════════════════════════════════════════════════════════════
//  RNC (Rob Northen Compression) – упрощённая реализация
//
//  Алгоритм работает чанками по 4096 байт.
//  Каждый чанк: серии из 8 токенов, перед каждой серией — байт
//  флагов (бит=1 → ссылка, бит=0 → литерал, MSB=токен 0).
//  Ссылка: 2 байта — [15:4] (dist-1), [3:0] (len-2)
//  Диапазон dist: 1..4096, len: 2..17.
//
//  Формат файла:
//   "RNC\x01"           4 байта (магия)
//   orig_size           4 байта big-endian
//   comp_size           4 байта big-endian
//   crc16               2 байта
//   [чанки]
//   Каждый чанк: 2 байта размера, затем данные чанка
// ═══════════════════════════════════════════════════════════════
class RNCCoder {
    static constexpr uint32_t CHUNK     = 4096;
    static constexpr uint32_t WIN       = 4096;
    static constexpr uint32_t MIN_MATCH = 2;
    static constexpr uint32_t MAX_MATCH = 17;

    static uint16_t crc16(const uint8_t* d, size_t n) noexcept {
        uint16_t crc = 0;
        for (size_t i = 0; i < n; ++i) {
            crc ^= static_cast<uint16_t>(d[i] << 8);
            for (int j = 0; j < 8; ++j)
                crc = (crc & 0x8000) ? ((crc << 1) ^ 0x8005) : (crc << 1);
        }
        return crc;
    }

    struct Match { uint32_t dist = 0, len = 0; };

    static Match find_match(const uint8_t* buf, uint32_t pos, uint32_t size) noexcept {
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

    // Сжать один чанк.
    // Формат: флаг-байт (MSB=первый токен, 1=ссылка, 0=литерал) + данные.
    static std::vector<uint8_t> compress_chunk(
        const uint8_t* src, uint32_t src_len,
        const std::vector<uint8_t>& history)
    {
        // history + src в одном буфере, чтобы можно искать совпадения
        std::vector<uint8_t> buf;
        uint32_t hist_len = static_cast<uint32_t>(
            std::min(history.size(), static_cast<size_t>(WIN)));
        buf.insert(buf.end(), history.end() - hist_len, history.end());
        uint32_t offset = hist_len;
        buf.insert(buf.end(), src, src + src_len);

        std::vector<uint8_t> out;
        uint32_t pos = offset;

        while (pos < offset + src_len) {
            // Накапливаем до 8 токенов
            uint8_t flags = 0;
            std::vector<uint8_t> group;
            for (int bit = 7; bit >= 0 && pos < offset + src_len; --bit) {
                Match m = find_match(buf.data(), pos,
                                     static_cast<uint32_t>(buf.size()));
                if (m.len >= MIN_MATCH) {
                    flags |= static_cast<uint8_t>(1u << bit);
                    // [15:4] = dist-1, [3:0] = len-2
                    uint16_t ref = static_cast<uint16_t>(
                        (((m.dist - 1) & 0xFFF) << 4) |
                        ((m.len - MIN_MATCH) & 0xF));
                    group.push_back(static_cast<uint8_t>(ref >> 8));
                    group.push_back(static_cast<uint8_t>(ref & 0xFF));
                    pos += m.len;
                } else {
                    group.push_back(buf[pos++]);
                }
            }
            out.push_back(flags);
            out.insert(out.end(), group.begin(), group.end());
        }
        return out;
    }

public:
    // ── КОДИРОВАНИЕ ──────────────────────────────────────────
    std::vector<uint8_t> encode(const std::vector<uint8_t>& in) {
        std::vector<uint8_t> out;
        out.reserve(in.size() + 32);

        // Заголовок
        out.push_back('R'); out.push_back('N');
        out.push_back('C'); out.push_back(0x01);

        uint32_t orig = static_cast<uint32_t>(in.size());
        for (int i = 3; i >= 0; --i) out.push_back((orig >> (i*8)) & 0xFF);

        size_t comp_size_pos = out.size();
        out.push_back(0); out.push_back(0); out.push_back(0); out.push_back(0);

        uint16_t crc = crc16(in.data(), in.size());
        out.push_back(crc >> 8); out.push_back(crc & 0xFF);

        // Чанки
        std::vector<uint8_t> history;
        for (uint32_t off = 0; off < orig; off += CHUNK) {
            uint32_t len = std::min<uint32_t>(CHUNK, orig - off);
            auto chunk = compress_chunk(in.data() + off, len, history);

            // Заголовок чанка: 2 байта размера
            out.push_back(static_cast<uint8_t>(chunk.size() >> 8));
            out.push_back(static_cast<uint8_t>(chunk.size() & 0xFF));
            out.insert(out.end(), chunk.begin(), chunk.end());

            history.insert(history.end(),
                           in.begin() + off, in.begin() + off + len);
        }

        // Финальный сжатый размер
        uint32_t comp_size = static_cast<uint32_t>(out.size() - 14);
        for (int i = 3; i >= 0; --i)
            out[comp_size_pos + (3-i)] = (comp_size >> (i*8)) & 0xFF;

        return out;
    }

    // ── ДЕКОДИРОВАНИЕ ────────────────────────────────────────
    std::vector<uint8_t> decode(const std::vector<uint8_t>& in) {
        if (in.size() < 14)
            throw std::runtime_error("RNC: слишком короткий заголовок");
        if (in[0]!='R' || in[1]!='N' || in[2]!='C' || in[3]!=0x01)
            throw std::runtime_error("RNC: неверная магическая последовательность");

        uint32_t orig = 0;
        for (int i = 0; i < 4; ++i) orig = (orig << 8) | in[4+i];

        uint16_t stored_crc = static_cast<uint16_t>((in[12] << 8) | in[13]);

        std::vector<uint8_t> out;
        out.reserve(orig);

        size_t pos = 14;
        while (pos < in.size() && out.size() < orig) {
            if (pos + 2 > in.size())
                throw std::runtime_error("RNC: усечённый заголовок чанка");
            uint16_t chunk_size =
                static_cast<uint16_t>((in[pos] << 8) | in[pos+1]);
            pos += 2;

            if (pos + chunk_size > in.size())
                throw std::runtime_error("RNC: усечённый чанк");

            const uint8_t* cp  = in.data() + pos;
            const uint8_t* end = cp + chunk_size;

            while (cp < end && out.size() < orig) {
                // Флаг-байт
                uint8_t flags = *cp++;
                // До 8 токенов (MSB = первый)
                for (int bit = 7; bit >= 0 && cp < end && out.size() < orig; --bit) {
                    if (flags & (1u << bit)) {
                        // Ссылка: 2 байта
                        if (cp + 2 > end)
                            throw std::runtime_error("RNC: усечённая ссылка");
                        uint16_t ref =
                            static_cast<uint16_t>((*cp << 8) | *(cp+1));
                        cp += 2;
                        uint32_t dist = static_cast<uint32_t>(ref >> 4) + 1;
                        uint32_t len  = static_cast<uint32_t>(ref & 0xF) + MIN_MATCH;

                        if (dist > out.size())
                            throw std::runtime_error(
                                "RNC: недопустимое расстояние ("
                                + std::to_string(dist) + " > "
                                + std::to_string(out.size()) + ")");

                        size_t start = out.size() - dist;
                        for (uint32_t j = 0; j < len && out.size() < orig; ++j)
                            out.push_back(out[start + j]);
                    } else {
                        // Литерал
                        out.push_back(*cp++);
                    }
                }
            }
            pos += chunk_size;
        }

        if (out.size() != orig)
            throw std::runtime_error("RNC: несоответствие размера ("
                + std::to_string(out.size())
                + " != " + std::to_string(orig) + ")");

        uint16_t calc_crc = crc16(out.data(), out.size());
        if (calc_crc != stored_crc)
            throw std::runtime_error("RNC: CRC не совпадает – архив повреждён");

        return out;
    }
};
