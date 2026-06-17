#pragma once
#include <vector>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <unordered_map>
#include <algorithm>

// ═══════════════════════════════════════════════════════════════
//  PPM (Prediction by Partial Matching) – порядок 1
//  + Арифметическое кодирование
//
//  Для каждого контекста (предыдущий символ) хранится
//  таблица вероятностей следующего символа.
//  ESC-символ (256) разрешает выход на порядок 0 (равномерное).
// ═══════════════════════════════════════════════════════════════

// ─────────────────────────────────────────────────────────────
//  Арифметический кодек (32-битные границы)
// ─────────────────────────────────────────────────────────────
class ArithEncoder {
    static constexpr uint32_t TOP  = 0x80000000u;
    static constexpr uint32_t BOT  = 0x00800000u;

    uint32_t low_ = 0, high_ = 0xFFFFFFFFu;
    int      buf_ = 0, cnt_ = 0;
    int      pending_ = 0;
    std::vector<uint8_t>& out_;

    void emit(int bit) {
        buf_ = (buf_ << 1) | bit;
        if (++cnt_ == 8) { out_.push_back(buf_); buf_ = 0; cnt_ = 0; }
    }
    void emit_pending(int bit, int& pending) {
        emit(bit);
        while (pending-- > 0) emit(!bit);
        pending = 0;
    }
public:
    explicit ArithEncoder(std::vector<uint8_t>& out) : out_(out) {}

    // Закодировать символ с кумулятивными вероятностями
    // cumul[sym], cumul[sym+1] / total
    void encode(uint32_t cum_lo, uint32_t cum_hi, uint32_t total) {
        uint32_t range = high_ - low_ + 1;
        high_ = low_ + range * cum_hi / total - 1;
        low_  = low_ + range * cum_lo / total;

        
        for (;;) {
            if (high_ < TOP) {
                emit_pending(0, pending_);
            } else if (low_ >= TOP) {
                emit_pending(1, pending_);
                low_  -= TOP; high_ -= TOP;
            } else if (low_ >= BOT && high_ < 3*BOT) {
                ++pending_;
                low_  -= BOT; high_ -= BOT;
            } else break;
            low_  <<= 1; high_ = (high_ << 1) | 1;
        }
    }

    void flush() {
        // Выбираем минимальное число бит для завершения
        ++low_;
        int bits = 1;
        while (bits < 32 && low_ >= (1u << (32 - bits))) ++bits;
        for (int i = bits - 1; i >= 0; --i)
            emit((low_ >> i) & 1);
        if (cnt_) { buf_ <<= (8 - cnt_); out_.push_back(buf_); }
    }
};

class ArithDecoder {
    static constexpr uint32_t TOP = 0x80000000u;
    static constexpr uint32_t BOT = 0x00800000u;

    uint32_t low_ = 0, high_ = 0xFFFFFFFFu, val_ = 0;
    const uint8_t* data_;
    size_t sz_, pos_ = 0;
    int bit_pos_ = 7;

    bool read_bit() {
        if (pos_ >= sz_) return false;
        bool b = (data_[pos_] >> bit_pos_) & 1;
        if (--bit_pos_ < 0) { bit_pos_ = 7; ++pos_; }
        return b;
    }
public:
    ArithDecoder(const uint8_t* data, size_t sz) : data_(data), sz_(sz) {
        for (int i = 0; i < 32; ++i) val_ = (val_ << 1) | read_bit();
    }

    // Возвращает символьный кумулятив по val_, не обновляя состояние
    uint32_t get_cum(uint32_t total) const {
        uint32_t range = high_ - low_ + 1;
        return ((val_ - low_ + 1) * total - 1) / range;
    }

    void update(uint32_t cum_lo, uint32_t cum_hi, uint32_t total) {
        uint32_t range = high_ - low_ + 1;
        high_ = low_ + range * cum_hi / total - 1;
        low_  = low_ + range * cum_lo / total;
        for (;;) {
            if (high_ < TOP) {
            } else if (low_ >= TOP) {
                val_ -= TOP; low_ -= TOP; high_ -= TOP;
            } else if (low_ >= BOT && high_ < 3*BOT) {
                val_ -= BOT; low_ -= BOT; high_ -= BOT;
            } else break;
            low_  <<= 1; high_ = (high_ << 1) | 1;
            val_  = (val_ << 1) | read_bit();
        }
    }
};

// ─────────────────────────────────────────────────────────────
//  Контекстная модель PPM-1
// ─────────────────────────────────────────────────────────────
struct PPMContext {
    std::array<uint16_t, 257> cnt{};   // cnt[256] = escape count
    uint32_t                  total = 0;

    void update(int sym) {
        ++cnt[sym]; ++total;
    }

    // cum_lo, cum_hi для символа (0..255) или 256=ESC
    std::pair<uint32_t,uint32_t> cumul(int sym) const {
        uint32_t lo = 0;
        for (int i = 0; i < sym; ++i) lo += cnt[i];
        return {lo, lo + cnt[sym]};
    }

    bool has(int sym) const { return cnt[sym] > 0; }
};

class PPMCoder {
    // Контексты: -1 = order-1 (нет контекста), 0..255 = предыдущий символ
    std::unordered_map<int, PPMContext> ctx_;
    static constexpr int ESC = 256;
    static constexpr int EOF_SYM = 257;  // в total не входит

public:
    // ── КОДИРОВАНИЕ ──────────────────────────────────────────
    std::vector<uint8_t> encode(const std::vector<uint8_t>& in) {
        std::vector<uint8_t> out;
        out.reserve(in.size());

        // Исходный размер
        uint64_t orig = in.size();
        for (int i = 7; i >= 0; --i) out.push_back((orig >> (i*8)) & 0xFF);

        ctx_.clear();
        // Инициализируем order-(-1): все символы равновероятны
        // (escape всегда доступен)

        ArithEncoder enc(out);

        int prev = -1;   // нет предыдущего

        auto encode_sym = [&](int sym) {
            // Сначала пробуем order-1 контекст
            PPMContext* c1 = (prev >= 0) ? &ctx_[prev] : nullptr;
            if (c1 && c1->total > 0 && (c1->has(sym) || true)) {
                // Контекст существует
                if (c1->has(sym)) {
                    auto [lo, hi] = c1->cumul(sym);
                    enc.encode(lo, hi, c1->total);
                } else {
                    // ESC → выход на order-0
                    auto [lo, hi] = c1->cumul(ESC);
                    enc.encode(lo, hi, c1->total);
                    // Order-0 равномерный (среди всех 256)
                    enc.encode(sym, sym + 1, 256);
                }
            } else {
                // order-(-1): равномерный
                enc.encode(sym, sym + 1, 256);
            }

            // Обновляем модель
            if (prev >= 0) {
                ctx_[prev].update(sym);
                if (!ctx_[prev].has(ESC)) ctx_[prev].cnt[ESC] = 1, ctx_[prev].total++;
            }
        };

        for (uint8_t b : in) {
            encode_sym(b);
            prev = b;
        }
        // Код конца файла
        if (prev >= 0 && ctx_[prev].total > 0) {
            // Сначала ESC если нужен
            if (!ctx_[prev].has(255 + 1)) {
                auto [lo, hi] = ctx_[prev].cumul(ESC);
                enc.encode(lo, hi, ctx_[prev].total);
            }
        }
        // В order-0 кодируем специальный символ 255 как конец
        // Уже хранится orig_size, так что просто завершаем

        enc.flush();
        return out;
    }

    // ── ДЕКОДИРОВАНИЕ ────────────────────────────────────────
    std::vector<uint8_t> decode(const std::vector<uint8_t>& in) {
        if (in.size() < 8)
            throw std::runtime_error("PPM: слишком короткий заголовок");

        uint64_t orig = 0;
        for (int i = 0; i < 8; ++i) orig = (orig << 8) | in[i];

        ctx_.clear();
        std::vector<uint8_t> out;
        out.reserve(static_cast<size_t>(orig));

        ArithDecoder dec(in.data() + 8, in.size() - 8);
        int prev = -1;

        while (out.size() < orig) {
            PPMContext* c1 = (prev >= 0) ? &ctx_[prev] : nullptr;
            int sym = -1;

            if (c1 && c1->total > 0) {
                uint32_t target = dec.get_cum(c1->total);
                uint32_t cum = 0;
                for (int s = 0; s < 257; ++s) {
                    if (cum + c1->cnt[s] > target) {
                        auto [lo, hi] = c1->cumul(s);
                        dec.update(lo, hi, c1->total);
                        sym = s;
                        break;
                    }
                    cum += c1->cnt[s];
                }
                if (sym == ESC) {
                    // Order-0 равномерный
                    sym = static_cast<int>(dec.get_cum(256));
                    dec.update(sym, sym + 1, 256);
                }
            } else {
                sym = static_cast<int>(dec.get_cum(256));
                dec.update(sym, sym + 1, 256);
            }

            if (sym < 0 || sym > 255)
                throw std::runtime_error("PPM: некорректный символ при декодировании");

            out.push_back(static_cast<uint8_t>(sym));

            if (prev >= 0) {
                ctx_[prev].update(sym);
                if (!ctx_[prev].has(ESC)) { ctx_[prev].cnt[ESC] = 1; ctx_[prev].total++; }
            }
            prev = sym;
        }

        if (out.size() != orig)
            throw std::runtime_error("PPM: несоответствие размера");
        return out;
    }
};
