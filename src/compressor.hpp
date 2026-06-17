#pragma once
#include <string>
#include <vector>
#include <functional>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <atomic>
#include <cstdint>
#include "archive.hpp"
#include "huffman.hpp"
#include "lz77.hpp"
#include "lzss.hpp"
#include "ppm.hpp"
#include "rnc.hpp"
#include "crc32.hpp"
#include "file_detector.hpp"

namespace fs = std::filesystem;

// ─────────────────────────────────────────────────────────────
//  Настройки
// ─────────────────────────────────────────────────────────────
struct CompressOptions {
    ArchiveFormat  format     = ArchiveFormat::CZIP;
    CompressMethod method     = CompressMethod::HUFFMAN;
    uint32_t       win_size   = 32768;
    bool           resume     = false;
};

using ProgressCb = std::function<void(double)>;
using StatusCb   = std::function<void(const std::string&)>;

// ─────────────────────────────────────────────────────────────
//  Размер чанка — 512 КБ.  Каждый чанк сжимается независимо,
//  что делает обработку больших файлов быстрой и предсказуемой.
//  Формат данных в архиве (после заголовка):
//    [4 байта BE] кол-во чанков
//    Для каждого чанка:
//      [4 байта BE] compressed_size  (0 → чанк хранится без сжатия)
//      [4 байта BE] original_size
//      [N байт]    данные
// ─────────────────────────────────────────────────────────────
static constexpr size_t CHUNK_SIZE = 512 * 1024;  // 512 КБ

class Compressor {
    ProgressCb        on_progress_;
    StatusCb          on_status_;
    std::atomic<bool> cancelled_{false};

    void progress(double p) const { if (on_progress_) on_progress_(p); }
    void status(const std::string& s) const { if (on_status_) on_status_(s); }

    // ── Сжать один чанк выбранным методом ────────────────────
    std::vector<uint8_t> compress_chunk(
        const std::vector<uint8_t>& chunk,
        CompressMethod method,
        uint32_t win_size) const
    {
        switch (method) {
            case CompressMethod::HUFFMAN: {
                HuffmanCoder hc;
                return hc.encode(chunk);
            }
            case CompressMethod::LZ77: {
                LZ77Coder lz(win_size);
                auto lz_out = lz.encode(chunk);
                // Дополнительно сжимаем Хаффманом
                HuffmanCoder hc;
                return hc.encode(lz_out);
            }
            case CompressMethod::PPM_LZSS_RNC: {
                LZSSCoder lzss;
                auto lzss_out = lzss.encode(chunk);
                RNCCoder rnc;
                return rnc.encode(lzss_out);
            }
        }
        return chunk;
    }

    // ── Распаковать один чанк ─────────────────────────────────
    static std::vector<uint8_t> decompress_chunk(
        const std::vector<uint8_t>& data,
        CompressMethod method,
        uint32_t win_size)
    {
        switch (method) {
            case CompressMethod::HUFFMAN: {
                HuffmanCoder hc;
                return hc.decode(data);
            }
            case CompressMethod::LZ77: {
                // Сначала Хаффман, потом LZ77
                HuffmanCoder hc;
                auto huff_out = hc.decode(data);
                LZ77Coder lz(win_size);
                return lz.decode(huff_out);
            }
            case CompressMethod::PPM_LZSS_RNC: {
                RNCCoder rnc;
                auto rnc_out = rnc.decode(data);
                LZSSCoder lzss;
                return lzss.decode(rnc_out);
            }
            default:
                throw std::runtime_error("Неизвестный метод сжатия");
        }
    }

    // ── Сериализовать чанки в байтовый поток ──────────────────

public:
    Compressor(ProgressCb prog = nullptr, StatusCb stat = nullptr)
        : on_progress_(std::move(prog)), on_status_(std::move(stat)) {}

    void cancel() { cancelled_ = true; }

    // ─────────────────────────────────────────────────────────
    //  Сжатие файла — потоковое, с поддержкой продолжения
    // ─────────────────────────────────────────────────────────
    void compress(const std::string& input_path,
                  const std::string& output_path,
                  const CompressOptions& opts)
    {
        cancelled_ = false;

        if (!fs::exists(input_path))
            throw std::runtime_error("Исходный файл не найден: " + input_path);

        uint64_t file_size = static_cast<uint64_t>(fs::file_size(input_path));
        if (file_size == 0)
            throw std::runtime_error("Исходный файл пустой");

        size_t n_chunks = (file_size + CHUNK_SIZE - 1) / CHUNK_SIZE;

        // ── Тип файла ────────────────────────────────────────
        {
            std::ifstream ft(input_path, std::ios::binary);
            std::vector<uint8_t> hdr_bytes(std::min<uint64_t>(file_size, 16));
            ft.read(reinterpret_cast<char*>(hdr_bytes.data()),
                    static_cast<std::streamsize>(hdr_bytes.size()));
            auto ftype = FileDetector::detect(hdr_bytes);
            status("Тип файла: " + ftype.description);
            if (ftype.is_archive)
                status("⚠ Файл уже сжат (" + ftype.description +
                       ") — коэффициент сжатия будет минимальным");
        }

        // ── Загрузка / создание чекпоинта ────────────────────
        Checkpoint ckpt;
        bool resuming = opts.resume && has_checkpoint(output_path);

        if (resuming) {
            ckpt = load_checkpoint(output_path);
            // Проверяем совместимость
            if (ckpt.orig_size != file_size) {
                status("⚠ Чекпоинт от другого файла — начинаем заново");
                resuming = false;
                ckpt = {};
            } else {
                status("Продолжение с чанка " +
                       std::to_string(ckpt.chunks_done + 1) +
                       " из " + std::to_string(n_chunks) + "...");
            }
        }

        if (!resuming) {
            ckpt.crc_accum   = 0xFFFFFFFFu;
            ckpt.chunks_done = 0;
        }

        progress(0.02);
        if (cancelled_) throw std::runtime_error("Отменено пользователем");

        // ── Открываем выходной файл ───────────────────────────
        ArchiveHeader hdr;
        hdr.format   = opts.format;
        hdr.method   = opts.method;
        hdr.win_size = opts.win_size;
        hdr.orig_size= file_size;
        hdr.filename = fs::path(input_path).filename().string();

        ArchiveOffsets off{};

        std::fstream fout;
        if (resuming) {
            // Открываем существующий файл для дозаписи
            fout.open(output_path,
                      std::ios::in | std::ios::out | std::ios::binary);
            if (!fout)
                throw std::runtime_error(
                    "Не удалось открыть архив для продолжения: " + output_path);
            // Вычитываем смещения полей из существующего заголовка
            fout.seekg(0, std::ios::beg);
            char magic[4]; fout.read(magic, 4);
            fout.seekg(1+1+1+4, std::ios::cur);        // version+method+flags+win_size
            off.orig_size_pos = fout.tellg();
            fout.seekg(8, std::ios::cur);               // orig_size
            off.crc32_pos = fout.tellg();
            fout.seekg(4, std::ios::cur);               // crc32
            uint16_t name_len = read_u16(fout);
            fout.seekg(name_len, std::ios::cur);
            off.n_chunks_pos = fout.tellg();
            // Позиционируем указатель записи на конец уже записанных чанков
            fout.seekp(static_cast<std::streamoff>(ckpt.output_pos));
        } else {
            fout.open(output_path,
                      std::ios::out | std::ios::binary | std::ios::trunc);
            if (!fout)
                throw std::runtime_error(
                    "Не удалось создать файл: " + output_path);
            off = begin_stream_archive(fout, hdr);
            // Запоминаем позицию после заголовка (до первого чанка)
            ckpt.output_pos = static_cast<uint64_t>(fout.tellp());
            ckpt.orig_size  = file_size;
        }

        // ── Открываем исходный файл, пропускаем обработанные чанки ──
        std::ifstream fin(input_path, std::ios::binary);
        if (!fin) throw std::runtime_error("Не удалось открыть файл: " + input_path);

        if (ckpt.chunks_done > 0) {
            // Перепрыгиваем через уже посчитанные CRC-байты и данные
            fin.seekg(static_cast<std::streamoff>(
                static_cast<uint64_t>(ckpt.chunks_done) * CHUNK_SIZE));
        }

        // ── Обработка чанков ─────────────────────────────────
        std::vector<uint8_t> buf(CHUNK_SIZE);
        uint32_t chunk_idx = ckpt.chunks_done;
        uint64_t total_compressed = 0;

        while (fin && !fin.eof()) {
            // Проверяем отмену ДО чтения — чтобы CRC соответствовал
            // только уже записанным чанкам
            if (cancelled_) {
                ckpt.output_pos = static_cast<uint64_t>(fout.tellp());
                save_checkpoint(output_path, ckpt);
                status("Прервано. Прогресс сохранён — "
                       "выберите тот же файл и нажмите «Продолжить»");
                throw std::runtime_error("Отменено пользователем");
            }

            fin.read(reinterpret_cast<char*>(buf.data()),
                     static_cast<std::streamsize>(CHUNK_SIZE));
            auto bytes_read = static_cast<size_t>(fin.gcount());
            if (bytes_read == 0) break;

            std::vector<uint8_t> chunk(buf.begin(), buf.begin() + bytes_read);

            ckpt.crc_accum = CRC32::update(ckpt.crc_accum,
                                           chunk.data(), chunk.size());

            double p = 0.02 + 0.86 * static_cast<double>(chunk_idx) / n_chunks;
            progress(p);
            char pct[8]; snprintf(pct, sizeof(pct), "%.0f%%", p * 100.0);
            status(std::string("Сжатие... ") + pct);

            auto compressed = compress_chunk(chunk, opts.method, opts.win_size);

            bool raw = compressed.size() >= chunk.size();
            const auto& out_data = raw ? chunk : compressed;
            uint32_t orig_flag = static_cast<uint32_t>(chunk.size()) |
                                 (raw ? 0x80000000u : 0u);

            write_stream_chunk(fout, out_data, orig_flag);
            total_compressed += out_data.size();
            ++chunk_idx;
            ckpt.chunks_done = chunk_idx;  // обновляем после успешной записи
        }
        fin.close();

        // ── Финализация ───────────────────────────────────────
        progress(0.92);
        status("Финализация архива...");

        uint32_t orig_crc = CRC32::finalize(ckpt.crc_accum);
        end_stream_archive(fout, off, file_size, orig_crc,
                           static_cast<uint32_t>(chunk_idx));
        fout.close();

        // Чекпоинт больше не нужен
        remove_checkpoint(output_path);

        progress(1.0);
        double ratio = static_cast<double>(total_compressed) / file_size;
        status("Готово! " + human_size(file_size) +
               " → " + human_size(total_compressed) +
               " (" + ratio_str(ratio) + ")");
    }

    // ─────────────────────────────────────────────────────────
    //  Восстановление из архива (чанковое)
    // ─────────────────────────────────────────────────────────
    void decompress(const std::string& input_path,
                    const std::string& output_path)
    {
        cancelled_ = false;

        status("Открытие архива...");
        progress(0.0);

        ArchiveHeader hdr;
        std::vector<uint8_t> payload;
        try {
            auto [h, d] = read_archive(input_path);
            hdr     = std::move(h);
            payload = std::move(d);
        } catch (const std::runtime_error& e) {
            throw std::runtime_error(
                std::string("Повреждённый архив: ") + e.what());
        }

        status("Формат: " + format_name(hdr.format) +
               ", Метод: " + method_name(hdr.method));
        progress(0.05);

        // Разбираем чанки из payload
        if (payload.size() < 4)
            throw std::runtime_error("Архив слишком короткий");

        size_t pos = 0;
        auto rb4 = [&]() -> uint32_t {
            if (pos + 4 > payload.size())
                throw std::runtime_error("Неожиданный конец данных архива");
            uint32_t v = (uint32_t(payload[pos])<<24) | (uint32_t(payload[pos+1])<<16)
                       | (uint32_t(payload[pos+2])<<8) | payload[pos+3];
            pos += 4;
            return v;
        };

        uint32_t n_chunks = rb4();
        if (n_chunks == 0 || n_chunks > 1024*1024)
            throw std::runtime_error("Некорректное количество чанков: " +
                                     std::to_string(n_chunks));

        std::ofstream fout(output_path, std::ios::binary);
        if (!fout) throw std::runtime_error("Не удалось создать файл: " + output_path);

        uint32_t crc_accum = 0xFFFFFFFFu;
        uint64_t total_written = 0;

        for (uint32_t i = 0; i < n_chunks; ++i) {
            if (cancelled_) throw std::runtime_error("Отменено пользователем");

            double p = 0.05 + 0.90 * static_cast<double>(i) / n_chunks;
            progress(p);
            char pct[8]; snprintf(pct, sizeof(pct), "%.0f%%", p * 100.0);
            status(std::string("Распаковка... ") + pct);

            uint32_t csz     = rb4();
            uint32_t orig_sz = rb4();

            bool raw = (orig_sz & 0x80000000u) != 0;
            orig_sz &= 0x7FFFFFFFu;
            uint32_t data_sz = raw ? orig_sz : csz;

            if (pos + data_sz > payload.size())
                throw std::runtime_error("Усечённый чанк " + std::to_string(i));

            std::vector<uint8_t> cdata(payload.begin() + static_cast<ptrdiff_t>(pos),
                                       payload.begin() + static_cast<ptrdiff_t>(pos) + data_sz);
            pos += data_sz;

            std::vector<uint8_t> result;
            if (raw) {
                result = std::move(cdata);
            } else {
                try {
                    result = decompress_chunk(cdata, hdr.method, hdr.win_size);
                } catch (const std::exception& e) {
                    throw std::runtime_error(
                        std::string("Ошибка распаковки чанка ") +
                        std::to_string(i) + ": " + e.what());
                }
            }

            if (result.size() != orig_sz)
                throw std::runtime_error(
                    "Размер чанка " + std::to_string(i) + " не совпадает");

            crc_accum = CRC32::update(crc_accum, result.data(), result.size());
            fout.write(reinterpret_cast<const char*>(result.data()),
                       static_cast<std::streamsize>(result.size()));
            total_written += result.size();
        }
        fout.close();

        if (total_written != hdr.orig_size)
            throw std::runtime_error(
                "Итоговый размер (" + human_size(total_written) +
                ") не совпадает с ожидаемым (" + human_size(hdr.orig_size) + ")");

        status("Проверка CRC32...");
        progress(0.97);
        uint32_t calc_crc = CRC32::finalize(crc_accum);
        if (calc_crc != hdr.crc32)
            throw std::runtime_error(
                "CRC32 не совпадает — архив повреждён. "
                "Ожидалось: " + crc_hex(hdr.crc32) +
                ", получено: " + crc_hex(calc_crc));

        remove_checkpoint(input_path);
        status("Восстановлено: " + human_size(total_written) +
               " → \"" + hdr.filename + "\"");
        progress(1.0);
    }

    // ─────────────────────────────────────────────────────────
    //  Вспомогательные
    // ─────────────────────────────────────────────────────────
    static std::string method_name(CompressMethod m) {
        switch (m) {
            case CompressMethod::HUFFMAN:      return "Хаффман";
            case CompressMethod::LZ77:         return "LZ77 + Хаффман";
            case CompressMethod::PPM_LZSS_RNC: return "LZSS → RNC";
        }
        return "Неизвестный";
    }
    static std::string format_name(ArchiveFormat f) {
        return (f == ArchiveFormat::CZIP) ? "CZIP" : "CRAR";
    }
    static std::string human_size(uint64_t sz) {
        if (sz < 1024)          return std::to_string(sz) + " Б";
        if (sz < 1024*1024)     return std::to_string(sz/1024) + " КБ";
        if (sz < 1024*1024*1024)
            return std::to_string(sz/(1024*1024)) + " МБ";
        char buf[32];
        snprintf(buf, sizeof(buf), "%.1f ГБ",
                 static_cast<double>(sz) / (1024.0*1024*1024));
        return buf;
    }
    static std::string ratio_str(double r) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.1f%%", r * 100.0);
        return buf;
    }
    static std::string crc_hex(uint32_t v) {
        char buf[16]; snprintf(buf, sizeof(buf), "%08X", v); return buf;
    }
};
