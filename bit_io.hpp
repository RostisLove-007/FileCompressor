#pragma once
#include <vector>
#include <cstdint>
#include <stdexcept>

// ─────────────────────────────────────────────────────────────
//  BitWriter — запись битов в буфер (MSB-first)
// ─────────────────────────────────────────────────────────────
class BitWriter {
    std::vector<uint8_t>& buf_;
    uint8_t cur_ = 0;
    int     cnt_ = 0;   // сколько бит уже в cur_ (0..7)
public:
    explicit BitWriter(std::vector<uint8_t>& buf) : buf_(buf) {}

    void write_bit(bool b) {
        cur_ = static_cast<uint8_t>((cur_ << 1) | (b ? 1 : 0));
        if (++cnt_ == 8) { buf_.push_back(cur_); cur_ = 0; cnt_ = 0; }
    }

    // Возвращает кол-во «пустых» бит в последнем байте
    int flush() {
        if (cnt_ == 0) return 0;
        int pad = 8 - cnt_;
        cur_ = static_cast<uint8_t>(cur_ << pad);
        buf_.push_back(cur_);
        cur_ = 0; cnt_ = 0;
        return pad;
    }
};

// ─────────────────────────────────────────────────────────────
//  BitReader — чтение битов из буфера (MSB-first)
// ─────────────────────────────────────────────────────────────
class BitReader {
    const uint8_t* data_;
    size_t         size_;
    size_t         byte_ = 0;
    int            bit_  = 7;   // текущий бит в байте (7 = MSB)
public:
    BitReader(const uint8_t* data, size_t size)
        : data_(data), size_(size) {}

    bool read_bit() {
        if (byte_ >= size_) throw std::runtime_error("BitReader: данные закончились");
        bool b = (data_[byte_] >> bit_) & 1;
        if (--bit_ < 0) { ++byte_; bit_ = 7; }
        return b;
    }

    bool eof() const { return byte_ >= size_; }
};
