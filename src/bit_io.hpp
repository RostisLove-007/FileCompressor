#pragma once
#include <vector>
#include <cstdint>
#include <stdexcept>
#include <string>

// ─────────────────────────────────────────────────────────────
//  BitWriter – записывает биты MSB-first в вектор байт
// ─────────────────────────────────────────────────────────────
class BitWriter {
    std::vector<uint8_t>& buf_;
    uint8_t   cur_  = 0;
    int       bits_ = 0;   // сколько бит накоплено в cur_ (0..7)

public:
    explicit BitWriter(std::vector<uint8_t>& buf) : buf_(buf) {}

    void write_bit(bool b) {
        cur_ = static_cast<uint8_t>((cur_ << 1) | (b ? 1 : 0));
        if (++bits_ == 8) { buf_.push_back(cur_); cur_ = 0; bits_ = 0; }
    }

    void write_bits(uint64_t val, int n) {
        for (int i = n - 1; i >= 0; --i)
            write_bit((val >> i) & 1u);
    }

    // Дописывает нулями до границы байта; возвращает число бит-заполнителей
    int flush() {
        if (bits_ == 0) return 0;
        int pad = 8 - bits_;
        cur_ = static_cast<uint8_t>(cur_ << pad);
        buf_.push_back(cur_);
        cur_ = 0; bits_ = 0;
        return pad;
    }
};

// ─────────────────────────────────────────────────────────────
//  BitReader – читает биты MSB-first из массива байт
// ─────────────────────────────────────────────────────────────
class BitReader {
    const uint8_t* data_;
    size_t   size_;
    size_t   byte_pos_ = 0;
    int      bit_pos_  = 7;   // следующий бит внутри текущего байта

public:
    BitReader(const uint8_t* data, size_t size)
        : data_(data), size_(size) {}

    bool read_bit() {
        if (byte_pos_ >= size_) throw std::runtime_error("BitReader: неожиданный конец данных");
        bool b = (data_[byte_pos_] >> bit_pos_) & 1u;
        if (--bit_pos_ < 0) { bit_pos_ = 7; ++byte_pos_; }
        return b;
    }

    uint64_t read_bits(int n) {
        uint64_t v = 0;
        for (int i = 0; i < n; ++i) v = (v << 1) | read_bit();
        return v;
    }

    bool eof() const { return byte_pos_ >= size_; }
    size_t bytes_consumed() const { return byte_pos_ + (bit_pos_ < 7 ? 1 : 0); }
};
