#pragma once
#include <vector>
#include <cstdint>
#include <stdexcept>
#include <algorithm>
#include <array>

/**
 * @file rnc.hpp
 * @brief Упрощённая реализация RNC (Rob Northen Compression).
 *
 * Алгоритм обрабатывает данные чанками по 4096 байт. Внутри чанка токены
 * группируются по 8 штук; перед каждой группой записывается байт флагов
 * (бит = 1 → ссылка, бит = 0 → литерал, старший бит соответствует первому
 * токену группы). Ссылка кодируется двумя байтами: биты [15:4] — (dist-1),
 * биты [3:0] — (len-2). Допустимый диапазон: dist от 1 до 4096, len от 2 до 17.
 *
 * Формат файла:
 *  - "RNC\x01" — 4 байта магической последовательности;
 *  - orig_size — 4 байта big-endian, исходный размер данных;
 *  - comp_size — 4 байта big-endian, размер сжатых данных после заголовка;
 *  - crc16     — 2 байта, контрольная сумма исходных данных;
 *  - далее     — последовательность чанков, каждый из которых начинается
 *    2-байтовым размером, за которым следуют сжатые данные чанка.
 */

/**
 * @brief Кодек RNC: чанковое сжатие LZ-подобными ссылками с проверкой CRC16.
 *
 * Каждый чанк сжимается с учётом «истории» — уже обработанных предыдущих
 * чанков, что позволяет находить совпадения через границы чанков в пределах
 * окна WIN. Целостность данных при распаковке проверяется контрольной
 * суммой CRC16 всего файла.
 */
class RNCCoder {
    /// Размер одного обрабатываемого блока входных данных.
    static constexpr uint32_t CHUNK     = 4096;
    /// Размер окна поиска совпадений (включает историю предыдущих чанков).
    static constexpr uint32_t WIN       = 4096;
    /// Минимальная длина совпадения, кодируемая ссылкой.
    static constexpr uint32_t MIN_MATCH = 2;
    /// Максимальная длина совпадения, представимая 4 битами.
    static constexpr uint32_t MAX_MATCH = 17;

    /**
     * @brief Вычисляет CRC-16 (полином 0x8005) для блока данных.
     * @param d Указатель на начало буфера данных.
     * @param n Количество байт в буфере @p d.
     * @return Значение контрольной суммы CRC-16.
     */
    static uint16_t crc16(const uint8_t* d, size_t n) noexcept {
        uint16_t crc = 0;
        for (size_t i = 0; i < n; ++i) {
            crc ^= static_cast<uint16_t>(d[i] << 8);
            for (int j = 0; j < 8; ++j)
                crc = (crc & 0x8000) ? ((crc << 1) ^ 0x8005) : (crc << 1);
        }
        return crc;
    }

    /**
     * @brief Результат поиска совпадения: расстояние и длина.
     */
    struct Match {
        uint32_t dist = 0;  ///< Расстояние от текущей позиции до начала совпадения.
        uint32_t len  = 0;  ///< Длина найденного совпадения в байтах.
    };

    /**
     * @brief Ищет наилучшее совпадение для позиции @p pos простым линейным перебором окна.
     * @param buf  Буфер данных (история + текущий чанк), в котором выполняется поиск.
     * @param pos  Позиция в @p buf, для которой ищется совпадение.
     * @param size Общий размер буфера @p buf.
     * @return Найденное совпадение; поле len равно 0, если подходящее совпадение не найдено.
     */
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

    /**
     * @brief Сжимает один чанк данных с учётом истории предыдущих чанков.
     *
     * Объединяет хвост истории (не более WIN байт) с текущим чанком в один
     * буфер для поиска совпадений, затем кодирует данные чанка группами
     * по 8 токенов с управляющим байтом флагов перед каждой группой.
     *
     * @param src     Указатель на начало данных текущего чанка.
     * @param src_len Длина текущего чанка в байтах.
     * @param history Накопленные ранее обработанные данные (для межчанковых совпадений).
     * @return Сжатые данные чанка (без заголовка размера).
     */
    static std::vector<uint8_t> compress_chunk(
        const uint8_t* src, uint32_t src_len,
        const std::vector<uint8_t>& history)
    {
        std::vector<uint8_t> buf;
        uint32_t hist_len = static_cast<uint32_t>(
            std::min(history.size(), static_cast<size_t>(WIN)));
        buf.insert(buf.end(), history.end() - hist_len, history.end());
        uint32_t offset = hist_len;
        buf.insert(buf.end(), src, src + src_len);

        std::vector<uint8_t> out;
        uint32_t pos = offset;

        while (pos < offset + src_len) {
            uint8_t flags = 0;
            std::vector<uint8_t> group;
            for (int bit = 7; bit >= 0 && pos < offset + src_len; --bit) {
                Match m = find_match(buf.data(), pos,
                                     static_cast<uint32_t>(buf.size()));
                if (m.len >= MIN_MATCH) {
                    flags |= static_cast<uint8_t>(1u << bit);
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
    /**
     * @brief Сжимает данные алгоритмом RNC.
     *
     * Записывает заголовок файла (магия, исходный размер, резерв под
     * итоговый сжатый размер, CRC16 исходных данных), затем разбивает
     * вход на чанки по CHUNK байт, сжимая каждый с учётом истории
     * предыдущих чанков, и обновляет поле итогового сжатого размера.
     *
     * @param in Исходные данные для сжатия.
     * @return Сжатый буфер в формате, описанном в @ref rnc.hpp.
     */
    std::vector<uint8_t> encode(const std::vector<uint8_t>& in) {
        std::vector<uint8_t> out;
        out.reserve(in.size() + 32);

        out.push_back('R'); out.push_back('N');
        out.push_back('C'); out.push_back(0x01);

        uint32_t orig = static_cast<uint32_t>(in.size());
        for (int i = 3; i >= 0; --i) out.push_back((orig >> (i*8)) & 0xFF);

        size_t comp_size_pos = out.size();
        out.push_back(0); out.push_back(0); out.push_back(0); out.push_back(0);

        uint16_t crc = crc16(in.data(), in.size());
        out.push_back(crc >> 8); out.push_back(crc & 0xFF);

        std::vector<uint8_t> history;
        for (uint32_t off = 0; off < orig; off += CHUNK) {
            uint32_t len = std::min<uint32_t>(CHUNK, orig - off);
            auto chunk = compress_chunk(in.data() + off, len, history);

            out.push_back(static_cast<uint8_t>(chunk.size() >> 8));
            out.push_back(static_cast<uint8_t>(chunk.size() & 0xFF));
            out.insert(out.end(), chunk.begin(), chunk.end());

            history.insert(history.end(),
                           in.begin() + off, in.begin() + off + len);
        }

        uint32_t comp_size = static_cast<uint32_t>(out.size() - 14);
        for (int i = 3; i >= 0; --i)
            out[comp_size_pos + (3-i)] = (comp_size >> (i*8)) & 0xFF;

        return out;
    }

    /**
     * @brief Восстанавливает исходные данные из буфера, сжатого encode().
     *
     * Проверяет магическую последовательность и читает заголовок,
     * затем последовательно разбирает чанки, в каждом из которых
     * декодирует группы токенов «литерал/ссылка». После полной
     * распаковки проверяет совпадение контрольной суммы CRC16.
     *
     * @param in Сжатый буфер в формате, описанном в @ref rnc.hpp.
     * @return Восстановленные исходные данные.
     * @throws std::runtime_error если заголовок повреждён, магическая
     *         последовательность неверна, чанк или ссылка усечены,
     *         расстояние ссылки недопустимо, итоговый размер не совпадает
     *         с заявленным, либо контрольная сумма CRC16 не совпадает.
     */
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
                uint8_t flags = *cp++;
                for (int bit = 7; bit >= 0 && cp < end && out.size() < orig; --bit) {
                    if (flags & (1u << bit)) {
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
