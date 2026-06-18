#pragma once
#include <vector>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <unordered_map>
#include <algorithm>

/**
 * @file ppm.hpp
 * @brief PPM (Prediction by Partial Matching) порядка 1 на основе
 *        арифметического кодирования.
 *
 * Для каждого контекста (предыдущего символа) хранится таблица частот
 * следующего символа. Специальный символ ESC (256) позволяет «выйти»
 * из контекста порядка 1 на равномерную модель порядка 0, если текущий
 * символ ещё не встречался в данном контексте.
 */

/**
 * @brief Кодирующая часть 32-битного арифметического кодека.
 *
 * Реализует классическую схему арифметического кодирования с
 * масштабированием границ [low_, high_] и отложенными битами (pending)
 * для корректной обработки ситуации, когда границы сходятся around
 * середины диапазона (underflow).
 */
class ArithEncoder {
    /// Верхняя половина 32-битного диапазона (для нормализации после переноса).
    static constexpr uint32_t TOP  = 0x80000000u;
    /// Порог нормализации midpoint-сходимости границ.
    static constexpr uint32_t BOT  = 0x00800000u;

    /// Нижняя граница текущего диапазона кодирования.
    uint32_t low_ = 0;
    /// Верхняя граница текущего диапазона кодирования.
    uint32_t high_ = 0xFFFFFFFFu;
    /// Буфер для накопления бит перед записью очередного байта.
    int      buf_ = 0;
    /// Количество бит, уже накопленных в buf_.
    int      cnt_ = 0;
    /// Счётчик отложенных (pending) бит, возникающих при сходимости границ.
    int      pending_ = 0;
    /// Ссылка на выходной буфер байт, в который пишутся закодированные данные.
    std::vector<uint8_t>& out_;

    /**
     * @brief Добавляет один бит в выходной битовый буфер.
     * @param bit Значение бита (0 или 1) для записи.
     */
    void emit(int bit) {
        buf_ = (buf_ << 1) | bit;
        if (++cnt_ == 8) { out_.push_back(buf_); buf_ = 0; cnt_ = 0; }
    }

    /**
     * @brief Записывает бит вместе со всеми накопленными отложенными битами.
     *
     * Отложенные биты всегда инвертированы относительно основного бита —
     * это часть стандартного алгоритма обработки сходимости границ
     * арифметического кодирования (E3-условие).
     *
     * @param bit     Основной бит для записи.
     * @param pending Счётчик отложенных бит; после вызова сбрасывается в 0.
     */
    void emit_pending(int bit, int& pending) {
        emit(bit);
        while (pending-- > 0) emit(!bit);
        pending = 0;
    }
public:
    /**
     * @brief Создаёт кодер, пишущий результат в заданный буфер.
     * @param out Буфер байт, в который добавляются закодированные данные.
     */
    explicit ArithEncoder(std::vector<uint8_t>& out) : out_(out) {}

    /**
     * @brief Кодирует один символ по его кумулятивному частотному диапазону.
     *
     * Сужает текущий диапазон [low_, high_] пропорционально интервалу
     * [cum_lo, cum_hi) относительно общей суммы частот total, после чего
     * нормализует границы, испуская готовые биты при необходимости.
     *
     * @param cum_lo Нижняя кумулятивная частота символа (включительно).
     * @param cum_hi Верхняя кумулятивная частота символа (исключительно).
     * @param total  Суммарная частота всех символов текущей модели.
     */
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

    /**
     * @brief Завершает кодирование, дописывая минимально необходимое число
     *        бит, чтобы декодер мог однозначно восстановить последний символ.
     *
     * После завершения сброса оставшиеся неполные биты буфера дополняются
     * нулями справа и записываются как последний байт потока.
     */
    void flush() {
        ++low_;
        int bits = 1;
        while (bits < 32 && low_ >= (1u << (32 - bits))) ++bits;
        for (int i = bits - 1; i >= 0; --i)
            emit((low_ >> i) & 1);
        if (cnt_) { buf_ <<= (8 - cnt_); out_.push_back(buf_); }
    }
};

/**
 * @brief Декодирующая часть 32-битного арифметического кодека.
 *
 * Симметричен ArithEncoder: поддерживает те же границы [low_, high_] и
 * текущее считанное значение val_, по которому можно определить, какому
 * символу принадлежит закодированный диапазон, не изменяя состояние
 * (см. get_cum()), а затем подтвердить выбор вызовом update().
 */
class ArithDecoder {
    /// Верхняя половина 32-битного диапазона (см. ArithEncoder::TOP).
    static constexpr uint32_t TOP = 0x80000000u;
    /// Порог нормализации midpoint-сходимости границ (см. ArithEncoder::BOT).
    static constexpr uint32_t BOT = 0x00800000u;

    /// Нижняя граница текущего диапазона декодирования.
    uint32_t low_ = 0;
    /// Верхняя граница текущего диапазона декодирования.
    uint32_t high_ = 0xFFFFFFFFu;
    /// Текущее считанное из потока значение (32 бита).
    uint32_t val_ = 0;
    /// Указатель на буфер входных сжатых данных.
    const uint8_t* data_;
    /// Размер буфера входных данных в байтах.
    size_t sz_;
    /// Индекс текущего читаемого байта в буфере данных.
    size_t pos_ = 0;
    /// Индекс следующего бита внутри текущего байта (7 — старший).
    int bit_pos_ = 7;

    /**
     * @brief Считывает один бит из входного буфера.
     * @return Значение очередного бита, либо @c false, если данные закончились.
     */
    bool read_bit() {
        if (pos_ >= sz_) return false;
        bool b = (data_[pos_] >> bit_pos_) & 1;
        if (--bit_pos_ < 0) { bit_pos_ = 7; ++pos_; }
        return b;
    }
public:
    /**
     * @brief Создаёт декодер и инициализирует его первыми 32 битами потока.
     * @param data Указатель на буфер со сжатыми данными.
     * @param sz   Размер буфера @p data в байтах.
     */
    ArithDecoder(const uint8_t* data, size_t sz) : data_(data), sz_(sz) {
        for (int i = 0; i < 32; ++i) val_ = (val_ << 1) | read_bit();
    }

    /**
     * @brief Вычисляет кумулятивную частоту, соответствующую текущему значению val_.
     *
     * Не изменяет состояние декодера — может вызываться многократно для
     * поиска символа, чей интервал [cum_lo, cum_hi) содержит результат,
     * прежде чем зафиксировать выбор вызовом update().
     *
     * @param total Суммарная частота всех символов текущей модели.
     * @return Кумулятивная частота, соответствующая текущему закодированному значению.
     */
    uint32_t get_cum(uint32_t total) const {
        uint32_t range = high_ - low_ + 1;
        return ((val_ - low_ + 1) * total - 1) / range;
    }

    /**
     * @brief Подтверждает декодирование символа и продвигает поток.
     *
     * Сужает диапазон [low_, high_] так же, как это делает
     * ArithEncoder::encode(), и нормализует его, считывая новые биты
     * во val_ при необходимости.
     *
     * @param cum_lo Нижняя кумулятивная частота выбранного символа (включительно).
     * @param cum_hi Верхняя кумулятивная частота выбранного символа (исключительно).
     * @param total  Суммарная частота всех символов текущей модели.
     */
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

/**
 * @brief Контекстная модель частот символов для одного контекста PPM порядка 1.
 *
 * Хранит счётчики встречаемости для каждого из 256 значений байта плюс
 * отдельный счётчик для символа ESC (выход на модель порядка 0).
 */
struct PPMContext {
    /// Счётчики частот: индексы 0..255 — байтовые символы, индекс 256 — ESC.
    std::array<uint16_t, 257> cnt{};
    /// Суммарная частота всех символов в данном контексте (включая ESC).
    uint32_t                  total = 0;

    /**
     * @brief Увеличивает счётчик заданного символа и общую сумму на 1.
     * @param sym Код символа (0..255) или ESC (256), частоту которого нужно увеличить.
     */
    void update(int sym) {
        ++cnt[sym]; ++total;
    }

    /**
     * @brief Вычисляет кумулятивный частотный диапазон для заданного символа.
     * @param sym Код символа (0..255) или ESC (256).
     * @return Пара (нижняя кумулятивная частота, верхняя кумулятивная частота)
     *         для использования в арифметическом кодере/декодере.
     */
    std::pair<uint32_t,uint32_t> cumul(int sym) const {
        uint32_t lo = 0;
        for (int i = 0; i < sym; ++i) lo += cnt[i];
        return {lo, lo + cnt[sym]};
    }

    /**
     * @brief Проверяет, встречался ли символ в данном контексте хотя бы раз.
     * @param sym Код символа (0..255) или ESC (256).
     * @return @c true, если счётчик символа больше нуля.
     */
    bool has(int sym) const { return cnt[sym] > 0; }
};

/**
 * @brief Кодек PPM порядка 1 с арифметическим кодированием и механизмом escape.
 *
 * Контекстом для предсказания следующего символа служит предыдущий
 * обработанный байт (порядок 1). Если в данном контексте текущий символ
 * ранее не встречался, кодируется специальный символ ESC, после чего
 * символ кодируется равномерно в рамках модели порядка 0 (все 256 значений
 * равновероятны).
 */
class PPMCoder {
    /// Контексты порядка 1, индексированные кодом предыдущего байта (0..255).
    std::unordered_map<int, PPMContext> ctx_;
    /// Код специального символа "escape", означающего выход на порядок 0.
    static constexpr int ESC = 256;
    /// Зарезервированный код конца данных (не участвует в подсчёте total).
    static constexpr int EOF_SYM = 257;

public:
    /**
     * @brief Сжимает данные моделью PPM порядка 1 с арифметическим кодированием.
     *
     * Для каждого входного байта сначала пробует контекст порядка 1
     * (предыдущий байт); если символ в этом контексте уже встречался —
     * кодирует его кумулятивным диапазоном этого контекста, иначе кодирует
     * ESC и затем сам символ равномерно (порядок 0). После кодирования
     * модель контекста обновляется встретившимся символом.
     *
     * @param in Исходные данные для сжатия.
     * @return Сжатый буфер: 8 байт исходного размера (big-endian) с
     *         последующим арифметически закодированным потоком.
     */
    std::vector<uint8_t> encode(const std::vector<uint8_t>& in) {
        std::vector<uint8_t> out;
        out.reserve(in.size());

        uint64_t orig = in.size();
        for (int i = 7; i >= 0; --i) out.push_back((orig >> (i*8)) & 0xFF);

        ctx_.clear();

        ArithEncoder enc(out);

        int prev = -1;

        auto encode_sym = [&](int sym) {
            PPMContext* c1 = (prev >= 0) ? &ctx_[prev] : nullptr;
            if (c1 && c1->total > 0 && (c1->has(sym) || true)) {
                if (c1->has(sym)) {
                    auto [lo, hi] = c1->cumul(sym);
                    enc.encode(lo, hi, c1->total);
                } else {
                    auto [lo, hi] = c1->cumul(ESC);
                    enc.encode(lo, hi, c1->total);
                    enc.encode(sym, sym + 1, 256);
                }
            } else {
                enc.encode(sym, sym + 1, 256);
            }

            if (prev >= 0) {
                ctx_[prev].update(sym);
                if (!ctx_[prev].has(ESC)) ctx_[prev].cnt[ESC] = 1, ctx_[prev].total++;
            }
        };

        for (uint8_t b : in) {
            encode_sym(b);
            prev = b;
        }
        if (prev >= 0 && ctx_[prev].total > 0) {
            if (!ctx_[prev].has(255 + 1)) {
                auto [lo, hi] = ctx_[prev].cumul(ESC);
                enc.encode(lo, hi, ctx_[prev].total);
            }
        }

        enc.flush();
        return out;
    }

    /**
     * @brief Восстанавливает исходные данные из буфера, сжатого encode().
     *
     * Воссоздаёт ту же последовательность решений модели PPM, что и
     * кодер: для каждого следующего байта по значению декодера определяет
     * символ в текущем контексте порядка 1 (либо ESC, означающий переход
     * на равномерную модель порядка 0), обновляет модель контекста
     * аналогично кодеру, и переходит к следующему символу.
     *
     * @param in Сжатый буфер, полученный от encode().
     * @return Восстановленные исходные данные.
     * @throws std::runtime_error если заголовок слишком короткий, при
     *         декодировании получен некорректный символ, либо итоговый
     *         размер не совпадает с заявленным.
     */
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
