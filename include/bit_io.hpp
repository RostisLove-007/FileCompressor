#pragma once
#include <vector>
#include <cstdint>
#include <stdexcept>
#include <string>

/**
 * @file bit_io.hpp
 * @brief Побитовый ввод/вывод (MSB-first) для кодеков сжатия данных.
 */

/**
 * @brief Записывает отдельные биты в порядке MSB-first в байтовый вектор.
 *
 * Биты накапливаются во внутреннем однобайтовом буфере; как только
 * накоплено 8 бит, готовый байт добавляется в целевой вектор @c buf_.
 * После окончания записи необходимо вызвать flush(), чтобы дописать
 * оставшиеся неполные биты нулями до границы байта.
 */
class BitWriter {
    /// Ссылка на внешний буфер байт, в который выполняется запись.
    std::vector<uint8_t>& buf_;
    /// Текущий не до конца заполненный байт.
    uint8_t   cur_  = 0;
    /// Количество бит, уже накопленных в cur_ (от 0 до 7).
    int       bits_ = 0;

public:
    /**
     * @brief Создаёт писатель битов, пишущий в заданный буфер.
     * @param buf Буфер байт, в конец которого будут добавляться данные.
     */
    explicit BitWriter(std::vector<uint8_t>& buf) : buf_(buf) {}

    /**
     * @brief Записывает один бит.
     * @param b Значение бита: @c true соответствует 1, @c false — 0.
     */
    void write_bit(bool b) {
        cur_ = static_cast<uint8_t>((cur_ << 1) | (b ? 1 : 0));
        if (++bits_ == 8) { buf_.push_back(cur_); cur_ = 0; bits_ = 0; }
    }

    /**
     * @brief Записывает несколько младших бит значения, начиная со старшего.
     * @param val Значение, биты которого записываются (используются младшие @p n бит).
     * @param n   Количество бит для записи.
     */
    void write_bits(uint64_t val, int n) {
        for (int i = n - 1; i >= 0; --i)
            write_bit((val >> i) & 1u);
    }

    /**
     * @brief Дописывает нулевые биты до границы байта.
     *
     * Если в буфере есть незавершённый байт, он дополняется нулями справа
     * и сбрасывается в выходной вектор.
     *
     * @return Количество добавленных бит-заполнителей (0..7).
     */
    int flush() {
        if (bits_ == 0) return 0;
        int pad = 8 - bits_;
        cur_ = static_cast<uint8_t>(cur_ << pad);
        buf_.push_back(cur_);
        cur_ = 0; bits_ = 0;
        return pad;
    }
};

/**
 * @brief Читает отдельные биты в порядке MSB-first из массива байт.
 *
 * Класс не владеет переданным буфером данных — он должен оставаться
 * валидным на протяжении всего времени использования объекта BitReader.
 */
class BitReader {
    /// Указатель на начало буфера с данными для чтения.
    const uint8_t* data_;
    /// Размер буфера данных в байтах.
    size_t   size_;
    /// Индекс текущего читаемого байта в буфере.
    size_t   byte_pos_ = 0;
    /// Индекс следующего бита внутри текущего байта (7 — старший, 0 — младший).
    int      bit_pos_  = 7;

public:
    /**
     * @brief Создаёт читатель битов для заданного буфера.
     * @param data Указатель на начало буфера с данными.
     * @param size Размер буфера данных в байтах.
     */
    BitReader(const uint8_t* data, size_t size)
        : data_(data), size_(size) {}

    /**
     * @brief Считывает один бит из потока.
     * @return Значение очередного бита: @c true для 1, @c false для 0.
     * @throws std::runtime_error если данные в буфере закончились.
     */
    bool read_bit() {
        if (byte_pos_ >= size_) throw std::runtime_error("BitReader: неожиданный конец данных");
        bool b = (data_[byte_pos_] >> bit_pos_) & 1u;
        if (--bit_pos_ < 0) { bit_pos_ = 7; ++byte_pos_; }
        return b;
    }

    /**
     * @brief Считывает несколько бит и собирает их в целое число.
     * @param n Количество бит для считывания.
     * @return Считанное значение, где первый прочитанный бит — старший.
     * @throws std::runtime_error если данные в буфере закончились раньше времени.
     */
    uint64_t read_bits(int n) {
        uint64_t v = 0;
        for (int i = 0; i < n; ++i) v = (v << 1) | read_bit();
        return v;
    }

    /**
     * @brief Проверяет, достигнут ли конец буфера данных.
     * @return @c true, если все байты буфера прочитаны.
     */
    bool eof() const { return byte_pos_ >= size_; }

    /**
     * @brief Возвращает количество байт, затронутых чтением.
     * @return Число байт буфера, из которых был прочитан хотя бы один бит.
     */
    size_t bytes_consumed() const { return byte_pos_ + (bit_pos_ < 7 ? 1 : 0); }
};
