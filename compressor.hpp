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

struct CompressOptions {
    ArchiveFormat  format   = ArchiveFormat::CZIP;
    CompressMethod method   = CompressMethod::HUFFMAN;
    uint32_t       win_size = 32768;
};

using ProgressCb = std::function<void(double)>;
using StatusCb   = std::function<void(const std::string&)>;

// ─────────────────────────────────────────────────────────────
//  Вспомогательные функции
// ─────────────────────────────────────────────────────────────
static inline std::string human_size(uint64_t sz) {
    if (sz < 1024)    return std::to_string(sz) + " Б";
    if (sz < 1048576) return std::to_string(sz / 1024) + " КБ";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f МБ",
                  static_cast<double>(sz) / 1048576.0);
    return buf;
}

static inline std::string method_name(CompressMethod m) {
    switch (m) {
        case CompressMethod::HUFFMAN:  return "Huffman";
        case CompressMethod::LZ77:     return "LZ77 + Huffman";
        case CompressMethod::LZSS_RNC: return "LZSS + RNC";
    }
    return "Неизвестный";
}

// ─────────────────────────────────────────────────────────────
//  Compressor — обёртка над алгоритмами с progress/status CB
// ─────────────────────────────────────────────────────────────
class Compressor {
    ProgressCb        on_progress_;
    StatusCb          on_status_;
    std::atomic<bool> cancelled_{false};

    void progress(double p)          const { if (on_progress_) on_progress_(p); }
    void status(const std::string& s) const { if (on_status_)   on_status_(s);   }

    std::vector<uint8_t> do_compress(const std::vector<uint8_t>& data,
                                     CompressMethod method,
                                     uint32_t win_size) const {
        switch (method) {
            case CompressMethod::LZ77: {
                LZ77Coder lz(win_size);
                HuffmanCoder hc;
                return hc.encode(lz.encode(data));
            }
            case CompressMethod::LZSS_RNC: {
                LZSSCoder lzss;
                RNCCoder  rnc;
                return rnc.encode(lzss.encode(data));
            }
            default: {
                HuffmanCoder hc;
                return hc.encode(data);
            }
        }
    }

    static std::vector<uint8_t> do_decompress(const std::vector<uint8_t>& payload,
                                              CompressMethod method,
                                              uint32_t win_size) {
        switch (method) {
            case CompressMethod::LZ77: {
                HuffmanCoder hc;
                LZ77Coder lz(win_size);
                return lz.decode(hc.decode(payload));
            }
            case CompressMethod::LZSS_RNC: {
                RNCCoder  rnc;
                LZSSCoder lzss;
                return lzss.decode(rnc.decode(payload));
            }
            default: {
                HuffmanCoder hc;
                return hc.decode(payload);
            }
        }
    }

public:
    Compressor(ProgressCb prog = nullptr, StatusCb stat = nullptr)
        : on_progress_(std::move(prog)), on_status_(std::move(stat)) {}

    void cancel() { cancelled_ = true; }

    // ── Сжатие ───────────────────────────────────────────────
    void compress(const std::string& input_path,
                  const std::string& output_path,
                  const CompressOptions& opts)
    {
        cancelled_ = false;
        if (!fs::exists(input_path))
            throw std::runtime_error("Файл не найден: " + input_path);

        status("Чтение файла...");
        progress(0.05);

        std::ifstream fin(input_path, std::ios::binary);
        std::vector<uint8_t> data(
            (std::istreambuf_iterator<char>(fin)), {});
        fin.close();

        if (cancelled_) throw std::runtime_error("Отменено");

        // Определяем тип
        std::vector<uint8_t> hdr_bytes(
            data.begin(),
            data.begin() + static_cast<ptrdiff_t>(
                std::min(data.size(), size_t(16))));
        auto ftype = FileDetector::detect(hdr_bytes);
        status("Тип файла: " + ftype.description);
        if (ftype.is_archive)
            status("⚠ Файл уже сжат — коэффициент будет минимальным");

        progress(0.15);
        if (cancelled_) throw std::runtime_error("Отменено");

        status("Сжатие (" + method_name(opts.method) + ")...");
        progress(0.20);

        uint32_t crc     = CRC32::compute(data);
        auto     payload = do_compress(data, opts.method, opts.win_size);

        if (cancelled_) throw std::runtime_error("Отменено");

        progress(0.90);
        status("Запись архива...");

        ArchiveHeader hdr;
        hdr.format    = opts.format;
        hdr.method    = opts.method;
        hdr.win_size  = opts.win_size;
        hdr.orig_size = data.size();
        hdr.crc32     = crc;
        hdr.filename  = fs::path(input_path).filename().string();

        write_archive(output_path, hdr, payload);

        double ratio = 100.0 * payload.size() / data.size();
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.1f%%", ratio);

        status("Готово! " + human_size(data.size()) +
               " → " + human_size(payload.size()) +
               " (" + buf + ")");
        progress(1.0);
    }

    // ── Распаковка ───────────────────────────────────────────
    void decompress(const std::string& input_path,
                    const std::string& output_path)
    {
        cancelled_ = false;
        status("Открытие архива...");
        progress(0.05);

        auto [hdr, payload] = read_archive(input_path);

        status("Метод: " + method_name(hdr.method));
        progress(0.15);

        if (cancelled_) throw std::runtime_error("Отменено");

        status("Распаковка...");
        progress(0.20);

        auto result = do_decompress(payload, hdr.method, hdr.win_size);

        progress(0.90);
        status("Проверка целостности...");

        if (result.size() != hdr.orig_size)
            throw std::runtime_error(
                "Файл повреждён — размер не совпадает");

        if (CRC32::compute(result) != hdr.crc32)
            throw std::runtime_error(
                "Файл повреждён — CRC32 не совпадает");

        status("Запись результата...");
        std::ofstream fout(output_path, std::ios::binary);
        if (!fout)
            throw std::runtime_error("Не удалось создать файл: " + output_path);
        fout.write(reinterpret_cast<const char*>(result.data()),
                   static_cast<std::streamsize>(result.size()));
        fout.close();

        status("Восстановлено: " + human_size(result.size()) +
               " → \"" + hdr.filename + "\"");
        progress(1.0);
    }
};
