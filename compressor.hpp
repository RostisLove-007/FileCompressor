#pragma once
#include <string>
#include <vector>
#include <functional>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <atomic>
#include <cstdio>
#include "archive.hpp"
#include "huffman.hpp"
#include "lz77.hpp"
#include "lzss.hpp"
#include "rnc.hpp"
#include "crc32.hpp"
#include "file_detector.hpp"

namespace fs = std::filesystem;

static constexpr size_t CHUNK_SIZE = 512 * 1024;

struct CompressOptions {
    ArchiveFormat  format   = ArchiveFormat::CZIP;
    CompressMethod method   = CompressMethod::HUFFMAN;
    uint32_t       win_size = 32768;
    bool           resume   = false;   // ← НОВОЕ: продолжить прерванное
};

using ProgressCb = std::function<void(double)>;
using StatusCb   = std::function<void(const std::string&)>;

inline std::string human_size(uint64_t sz) {
    if (sz < 1024)    return std::to_string(sz) + " Б";
    if (sz < 1048576) return std::to_string(sz/1024) + " КБ";
    char buf[32]; std::snprintf(buf,sizeof(buf),"%.1f МБ",(double)sz/1048576.0);
    return buf;
}
inline std::string method_name(CompressMethod m) {
    switch (m) {
        case CompressMethod::HUFFMAN:  return "Huffman";
        case CompressMethod::LZ77:     return "LZ77 + Huffman";
        case CompressMethod::LZSS_RNC: return "LZSS + RNC";
    }
    return "Неизвестный";
}

class Compressor {
    ProgressCb        on_progress_;
    StatusCb          on_status_;
    std::atomic<bool> cancelled_{false};

    void progress(double p)           const { if (on_progress_) on_progress_(p); }
    void status(const std::string& s) const { if (on_status_)   on_status_(s);   }

    std::vector<uint8_t> compress_chunk(const std::vector<uint8_t>& chunk,
                                        CompressMethod method,
                                        uint32_t win_size) const {
        switch (method) {
            case CompressMethod::LZ77: {
                LZ77Coder lz(win_size); HuffmanCoder hc;
                return hc.encode(lz.encode(chunk));
            }
            case CompressMethod::LZSS_RNC: {
                LZSSCoder lzss; RNCCoder rnc;
                return rnc.encode(lzss.encode(chunk));
            }
            default: { HuffmanCoder hc; return hc.encode(chunk); }
        }
    }

    static std::vector<uint8_t> decompress_chunk(
        const std::vector<uint8_t>& data,
        CompressMethod method, uint32_t win_size) {
        switch (method) {
            case CompressMethod::LZ77: {
                HuffmanCoder hc; LZ77Coder lz(win_size);
                return lz.decode(hc.decode(data));
            }
            case CompressMethod::LZSS_RNC: {
                RNCCoder rnc; LZSSCoder lzss;
                return lzss.decode(rnc.decode(data));
            }
            default: { HuffmanCoder hc; return hc.decode(data); }
        }
    }

public:
    Compressor(ProgressCb prog=nullptr, StatusCb stat=nullptr)
        : on_progress_(std::move(prog)), on_status_(std::move(stat)) {}

    void cancel() { cancelled_ = true; }

    // ─────────────────────────────────────────────────────────
    //  СЖАТИЕ с поддержкой resume
    // ─────────────────────────────────────────────────────────
    void compress(const std::string& input_path,
                  const std::string& output_path,
                  const CompressOptions& opts)
    {
        cancelled_ = false;

        if (!fs::exists(input_path))
            throw std::runtime_error("Файл не найден: " + input_path);

        uint64_t file_size = static_cast<uint64_t>(fs::file_size(input_path));
        if (file_size == 0) throw std::runtime_error("Файл пустой");

        size_t n_chunks = (file_size + CHUNK_SIZE - 1) / CHUNK_SIZE;

        // ── Определяем тип файла ─────────────────────────────
        {
            std::ifstream ft(input_path, std::ios::binary);
            std::vector<uint8_t> hb(std::min<uint64_t>(file_size,16));
            ft.read(reinterpret_cast<char*>(hb.data()),
                    static_cast<std::streamsize>(hb.size()));
            auto ftype = FileDetector::detect(hb);
            status("Тип файла: " + ftype.description);
            if (ftype.is_archive)
                status("⚠ Файл уже сжат — коэффициент будет минимальным");
        }

        progress(0.02);
        if (cancelled_) throw std::runtime_error("Отменено");

        // ── Загрузка / создание чекпоинта ────────────────────
        Checkpoint ck;
        bool resuming = opts.resume && has_checkpoint(output_path);

        if (resuming) {
            ck = load_checkpoint(output_path);
            if (ck.orig_size != file_size) {
                status("⚠ Чекпоинт от другого файла — начинаем заново");
                resuming = false;
                ck = {};
            } else {
                status("Продолжение с чанка " +
                       std::to_string(ck.chunks_done + 1) +
                       " из " + std::to_string(n_chunks));
            }
        }

        if (!resuming) {
            ck.crc_accum    = 0xFFFFFFFFu;
            ck.chunks_done  = 0;
            ck.orig_size    = file_size;
        }

        // ── Открываем выходной файл ───────────────────────────
        ArchiveHeader hdr;
        hdr.format   = opts.format;  hdr.method   = opts.method;
        hdr.win_size = opts.win_size; hdr.orig_size= file_size;
        hdr.filename = fs::path(input_path).filename().string();

        ArchiveOffsets off{};
        std::fstream fout;

        if (resuming) {
            // Открываем существующий файл для дозаписи
            fout.open(output_path,
                std::ios::in | std::ios::out | std::ios::binary);
            if (!fout)
                throw std::runtime_error(
                    "Не удалось открыть архив для продолжения");
            off = read_offsets(fout, output_path);
            fout.seekp(static_cast<std::streamoff>(ck.output_pos));
        } else {
            fout.open(output_path,
                std::ios::out | std::ios::binary | std::ios::trunc);
            if (!fout)
                throw std::runtime_error(
                    "Не удалось создать файл: " + output_path);
            off = begin_archive(fout, hdr);
            ck.output_pos = static_cast<uint64_t>(fout.tellp());
        }

        // ── Читаем и сжимаем чанки ───────────────────────────
        std::ifstream fin(input_path, std::ios::binary);
        // Пропускаем уже обработанные чанки
        if (ck.chunks_done > 0)
            fin.seekg(static_cast<std::streamoff>(
                static_cast<uint64_t>(ck.chunks_done) * CHUNK_SIZE));

        std::vector<uint8_t> buf(CHUNK_SIZE);
        uint64_t total_comp = 0;
        uint32_t chunk_idx  = ck.chunks_done;

        while (fin && !fin.eof()) {
            // Проверяем отмену ДО чтения: CRC должен соответствовать
            // только уже записанным чанкам
            if (cancelled_) {
                ck.output_pos = static_cast<uint64_t>(fout.tellp());
                save_checkpoint(output_path, ck);
                status("Прервано. Прогресс сохранён — "
                       "включите «Продолжить» и нажмите Старт");
                throw std::runtime_error("Отменено");
            }

            fin.read(reinterpret_cast<char*>(buf.data()),
                     static_cast<std::streamsize>(CHUNK_SIZE));
            auto bytes_read = static_cast<size_t>(fin.gcount());
            if (bytes_read == 0) break;

            std::vector<uint8_t> chunk(buf.begin(),
                                       buf.begin() + bytes_read);

            ck.crc_accum = CRC32::update(ck.crc_accum,
                                         chunk.data(), chunk.size());

            double p = 0.02 + 0.86 * static_cast<double>(chunk_idx) / n_chunks;
            progress(p);
            char pct[16];
            std::snprintf(pct, sizeof(pct), "%.0f%%", p * 100.0);
            status(std::string("Сжатие... ") + pct);

            auto compressed = compress_chunk(chunk, opts.method, opts.win_size);
            bool raw = compressed.size() >= chunk.size();
            const auto& out_data = raw ? chunk : compressed;
            uint32_t orig_flag = static_cast<uint32_t>(chunk.size())
                               | (raw ? 0x80000000u : 0u);

            write_chunk(fout, out_data, orig_flag);
            total_comp += out_data.size();
            ++chunk_idx;
            ck.chunks_done = chunk_idx;   // обновляем после успешной записи
        }
        fin.close();

        // ── Финализация ───────────────────────────────────────
        uint32_t final_crc = CRC32::finalize(ck.crc_accum);
        end_archive(fout, off, file_size, final_crc, chunk_idx);
        fout.close();

        remove_checkpoint(output_path);

        char ratio[16];
        std::snprintf(ratio, sizeof(ratio), "%.1f%%",
                      100.0 * total_comp / file_size);
        status("Готово! " + human_size(file_size) +
               " → " + human_size(total_comp) +
               " (" + ratio + ")");
        progress(1.0);
    }

    // ─────────────────────────────────────────────────────────
    //  РАСПАКОВКА
    // ─────────────────────────────────────────────────────────
    void decompress(const std::string& input_path,
                    const std::string& output_path)
    {
        cancelled_ = false;
        status("Открытие архива...");
        progress(0.02);

        auto [hdr, payload] = read_archive(input_path);
        status("Метод: " + method_name(hdr.method));

        if (payload.size() < 4)
            throw std::runtime_error("Архив повреждён");

        size_t pos = 0;
        auto rb4 = [&]() -> uint32_t {
            if (pos+4 > payload.size())
                throw std::runtime_error("Неожиданный конец архива");
            uint32_t v = ((uint32_t)payload[pos]<<24)
                       | ((uint32_t)payload[pos+1]<<16)
                       | ((uint32_t)payload[pos+2]<<8)
                       | payload[pos+3];
            pos+=4; return v;
        };

        uint32_t n_chunks = rb4();
        if (n_chunks==0 || n_chunks>4*1024*1024)
            throw std::runtime_error("Некорректное число чанков");

        std::ofstream fout(output_path, std::ios::binary);
        if (!fout) throw std::runtime_error("Не удалось создать: "+output_path);

        uint32_t crc_acc   = 0xFFFFFFFFu;
        uint64_t total_out = 0;

        for (uint32_t i = 0; i < n_chunks; ++i) {
            if (cancelled_) throw std::runtime_error("Отменено");

            double p = 0.02 + 0.86 * static_cast<double>(i) / n_chunks;
            progress(p);
            char pct[16]; std::snprintf(pct,sizeof(pct),"%.0f%%",p*100.0);
            status(std::string("Распаковка... ") + pct);

            uint32_t csz      = rb4();
            uint32_t orig_flag= rb4();
            bool     raw      = (orig_flag & 0x80000000u) != 0;
            uint32_t orig_sz  = orig_flag & 0x7FFFFFFFu;
            uint32_t data_sz  = raw ? orig_sz : csz;

            if (pos+data_sz > payload.size())
                throw std::runtime_error("Файл повреждён, восстановление прервано");

            std::vector<uint8_t> cdata(
                payload.begin()+static_cast<ptrdiff_t>(pos),
                payload.begin()+static_cast<ptrdiff_t>(pos)+data_sz);
            pos += data_sz;

            std::vector<uint8_t> result;
            try {
                result = raw ? std::move(cdata)
                             : decompress_chunk(cdata, hdr.method, hdr.win_size);
            } catch (...) {
                throw std::runtime_error("Файл повреждён, восстановление прервано");
            }

            if (result.size() != orig_sz)
                throw std::runtime_error("Файл повреждён, восстановление прервано");

            crc_acc = CRC32::update(crc_acc, result.data(), result.size());
            fout.write(reinterpret_cast<const char*>(result.data()),
                       static_cast<std::streamsize>(result.size()));
            total_out += result.size();
        }
        fout.close();

        if (total_out != hdr.orig_size)
            throw std::runtime_error("Файл повреждён, восстановление прервано");

        progress(0.95);
        status("Проверка CRC32...");

        if (CRC32::finalize(crc_acc) != hdr.crc32)
            throw std::runtime_error("Файл повреждён, восстановление прервано");

        status("Восстановлено: " + human_size(total_out) +
               " → \"" + hdr.filename + "\"");
        progress(1.0);
    }
};
