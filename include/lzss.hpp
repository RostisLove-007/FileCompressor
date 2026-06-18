#pragma once
#include <vector>
#include <cstdint>
#include <stdexcept>
#include <algorithm>
#include <unordered_map>

/**
 * @file lzss.hpp
 * @brief Сжатие LZSS с управляющими байтами флагов, группирующими по 8 токенов.
 *
 * Формат сжатых данных:
 *  - 8 байт — исходный размер (big-endian uint64);
 *  - далее — группы токенов, каждая начинается управляющим байтом (ctrl),
 *    где каждый бит (от старшего к младшему) обозначает один токен:
 *      - 0 → литерал (следующий 1 байт — символ);
 *      - 1 → ссылка (следующие 2 байта — упакованные смещение и длина).
 *
 * Параметры ссылки: смещение занимает 12 бит (окно 4 КБ), длина — 4 бита
 * (диапазон длины совпадения 3..18).
 */

/**
 * @brief Кодек LZSS с фиксированным окном 4 КБ и группировкой токенов по 8 штук.
 *
 * В отличие от классического LZ77, LZSS не использует отдельный бит-флаг
 * перед каждым токеном в потоке — вместо этого 8 флагов собираются в один
 * управляющий байт перед группой из 8 токенов, что упрощает побайтовое
 * чтение/запись без явного побитового ввода-вывода.
 */
class LZSSCoder {
    /// Размер скользящего окна поиска совпадений.
    static constexpr uint32_t WIN    = 4096;
    /// Максимальная длина совпадения, представимая 4 битами.
    static constexpr uint32_t MAX_LN = 18;
    /// Минимальная длина совпадения, при которой ссылка выгоднее литерала.
    static constexpr uint32_t MIN_LN = 3;

    /// Тип хэш-таблицы: хэш трёхбайтовой подстроки → список позиций вхождения.
    using HashMap = std::unordered_map<uint32_t, std::vector<uint32_t>>;

    /**
     * @brief Вычисляет хэш для трёх байт, начинающихся с указанного адреса.
     * @param p Указатель на первый из трёх байт.
     * @return 24-битное хэш-значение, упакованное в uint32_t.
     */
    static uint32_t hash3(const uint8_t* p) noexcept {
        return (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2];
    }

    /**
     * @brief Ищет в пределах окна наилучшее совпадение для текущей позиции.
     * @param data   Полный буфер входных данных.
     * @param pos    Текущая позиция в @p data, для которой ищется совпадение.
     * @param chains Хэш-таблица цепочек позиций, накопленная по уже обработанным данным.
     * @return Пара (расстояние до начала совпадения, длина совпадения);
     *         оба значения равны 0, если совпадение длиной не менее MIN_LN не найдено.
     */
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
    /**
     * @brief Сжимает данные алгоритмом LZSS.
     *
     * Группирует выходные токены по 8 штук, предваряя каждую группу
     * управляющим байтом флагов «литерал/ссылка». Для каждой позиции
     * ищет совпадение в хэш-цепочках, ограниченных размером окна WIN.
     *
     * @param in Исходные данные для сжатия.
     * @return Сжатый буфер в формате, описанном в @ref lzss.hpp.
     */
    std::vector<uint8_t> encode(const std::vector<uint8_t>& in) {
        std::vector<uint8_t> out;
        out.reserve(in.size());

        uint64_t orig = in.size();
        for (int i = 7; i >= 0; --i) out.push_back((orig >> (i*8)) & 0xFF);

        HashMap chains;
        uint32_t pos = 0;
        uint32_t n   = static_cast<uint32_t>(in.size());

        while (pos < n) {
            uint8_t  ctrl  = 0;
            size_t   cpos  = out.size();
            out.push_back(0);

            for (int bit = 7; bit >= 0 && pos < n; --bit) {
                auto [dist, len] = find_match(in, pos, chains);

                if (len >= MIN_LN) {
                    ctrl |= (1u << bit);
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

    /**
     * @brief Восстанавливает исходные данные из буфера, сжатого encode().
     *
     * Читает исходный размер, затем последовательно разбирает группы
     * токенов по управляющему байту флагов, копируя литералы напрямую
     * и разворачивая ссылки из уже восстановленной части вывода.
     *
     * @param in Сжатый буфер в формате, описанном в @ref lzss.hpp.
     * @return Восстановленные исходные данные.
     * @throws std::runtime_error если заголовок слишком короткий, ссылка
     *         или литерал усечены, расстояние ссылки недопустимо, либо
     *         итоговый размер не совпадает с заявленным.
     */
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
