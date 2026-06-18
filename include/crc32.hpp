#pragma once
#include <cstdint>
#include <array>
#include <vector>

/**
 * @file crc32.hpp
 * @brief Вычисление контрольной суммы CRC-32 (полином IEEE 802.3).
 */

namespace detail {

/**
 * @brief Строит таблицу значений CRC-32 для всех 256 значений байта.
 *
 * Таблица используется для быстрого побайтового вычисления CRC-32
 * по стандартному полиному IEEE 802.3 (0xEDB88320 в отражённой форме).
 *
 * @return Массив из 256 предвычисленных 32-битных значений CRC.
 */
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

/// Предвычисленная таблица CRC-32, используемая всеми функциями класса CRC32.
static constexpr auto CRC_TABLE = make_crc_table();

}  // namespace detail

/**
 * @brief Инкрементальный и одноразовый вычислитель контрольной суммы CRC-32.
 *
 * Класс позволяет либо накапливать контрольную сумму по частям данных
 * методом update(), либо вычислить её сразу для всего буфера статическим
 * методом compute(). Внутреннее состояние хранится в инвертированном виде
 * (как того требует алгоритм CRC-32) и инвертируется обратно при вызове
 * value().
 */
class CRC32 {
    /// Ссылка на общую таблицу коэффициентов CRC-32.
    static constexpr auto& TABLE = detail::CRC_TABLE;

    /// Текущее накопленное (инвертированное) значение CRC.
    uint32_t crc_ = 0xFFFFFFFFu;

public:
    /**
     * @brief Добавляет блок байт к накопленной контрольной сумме.
     * @param d Указатель на начало буфера данных.
     * @param n Количество байт в буфере @p d.
     */
    void update(const uint8_t* d, size_t n) noexcept {
        for (size_t i = 0; i < n; ++i)
            crc_ = TABLE[(crc_ ^ d[i]) & 0xFF] ^ (crc_ >> 8);
    }

    /**
     * @brief Добавляет содержимое вектора байт к накопленной контрольной сумме.
     * @param v Вектор байт, которые нужно учесть в контрольной сумме.
     */
    void update(const std::vector<uint8_t>& v) noexcept { update(v.data(), v.size()); }

    /**
     * @brief Возвращает итоговое значение контрольной суммы.
     * @return Финализированное (инвертированное обратно) значение CRC-32.
     */
    uint32_t value()  const noexcept { return crc_ ^ 0xFFFFFFFFu; }

    /**
     * @brief Сбрасывает накопитель в начальное состояние.
     */
    void     reset()        noexcept { crc_ = 0xFFFFFFFFu; }

    /**
     * @brief Вычисляет CRC-32 для одного буфера данных «за один проход».
     * @param d Указатель на начало буфера данных.
     * @param n Количество байт в буфере @p d.
     * @return Значение CRC-32 для данного буфера.
     */
    static uint32_t compute(const uint8_t* d, size_t n) noexcept {
        CRC32 c; c.update(d, n); return c.value();
    }

    /**
     * @brief Вычисляет CRC-32 для вектора байт «за один проход».
     * @param v Вектор байт, для которого вычисляется контрольная сумма.
     * @return Значение CRC-32 для данного вектора.
     */
    static uint32_t compute(const std::vector<uint8_t>& v) noexcept {
        return compute(v.data(), v.size());
    }

    /**
     * @brief Обновляет внешний (инвертированный) накопитель CRC новым блоком данных.
     *
     * Используется для инкрементального вычисления CRC по частям, когда
     * накопитель хранится вызывающим кодом (например, между чанками файла),
     * а не внутри объекта CRC32. Перед первым вызовом накопитель должен быть
     * инициализирован значением 0xFFFFFFFF.
     *
     * @param accum Текущее инвертированное значение накопителя CRC.
     * @param d     Указатель на начало буфера данных.
     * @param n     Количество байт в буфере @p d.
     * @return Обновлённое инвертированное значение накопителя CRC.
     */
    static uint32_t update(uint32_t accum, const uint8_t* d, size_t n) noexcept {
        for (size_t i = 0; i < n; ++i)
            accum = TABLE[(accum ^ d[i]) & 0xFF] ^ (accum >> 8);
        return accum;
    }

    /**
     * @brief Финализирует инкрементальный накопитель CRC, полученный через update().
     * @param accum Инвертированное значение накопителя CRC.
     * @return Итоговое (не инвертированное) значение CRC-32.
     */
    static uint32_t finalize(uint32_t accum) noexcept {
        return accum ^ 0xFFFFFFFFu;
    }
};
