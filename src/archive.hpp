#pragma once
#include <string>
#include <vector>
#include <fstream>
#include <filesystem>
#include <stdexcept>
#include <cstring>
#include <cstdint>
#include "crc32.hpp"

namespace fs = std::filesystem;

// ═══════════════════════════════════════════════════════════════
//  Формат архива CZIP / CRAR  (потоковая запись)
//
//  ┌─ Заголовок (фиксированная часть) ───────────────────────┐
//  │  Магия       4 байта  "CZIP" / "CRAR"                   │
//  │  Версия      1 байт   0x01                               │
//  │  Метод       1 байт   0=Huffman 1=LZ77 2=LZSS+RNC       │
//  │  Флаги       1 байт   зарезервировано                    │
//  │  Win-size    4 байта  (big-endian)                       │
//  │  Orig-size   8 байт   исходный размер   ← обновляется   │
//  │  CRC32       4 байта  CRC исходного     ← обновляется   │
//  │  Name-len    2 байта                                     │
//  │  Name        N байт                                      │
//  └─────────────────────────────────────────────────────────-┘
//  ┌─ Данные ─────────────────────────────────────────────────┐
//  │  n_chunks    4 байта  кол-во чанков     ← обновляется   │
//  │  [чанк]*n:                                               │
//  │    csz       4 байта  размер сжатого (бит31=raw)         │
//  │    orig_sz   4 байта  размер оригинала                   │
//  │    data      csz байт                                    │
//  └──────────────────────────────────────────────────────────┘
//
//  Контрольная точка — файл <архив>.ckpt:
//   [4 байта] chunks_done  — сколько чанков записано
//   [4 байта] crc_accum    — накопленный CRC32 (XOR не применён)
//   [8 байта] output_pos   — позиция конца последнего чанка в архиве
//   [8 байта] orig_size    — полный размер исходника (для проверки)
// ═══════════════════════════════════════════════════════════════

enum class ArchiveFormat { CZIP = 0, CRAR = 1 };
enum class CompressMethod {
    HUFFMAN      = 0,
    LZ77         = 1,
    PPM_LZSS_RNC = 2
};

struct ArchiveHeader {
    ArchiveFormat  format   = ArchiveFormat::CZIP;
    CompressMethod method   = CompressMethod::HUFFMAN;
    uint8_t        flags    = 0;
    uint32_t       win_size = 32768;
    uint64_t       orig_size= 0;
    uint32_t       crc32    = 0;
    std::string    filename;
};

// Смещения полей, которые обновляются после записи чанков.
// Вычисляются при открытии архива.
struct ArchiveOffsets {
    std::streampos orig_size_pos;  // позиция поля orig_size в заголовке
    std::streampos crc32_pos;      // позиция поля crc32
    std::streampos n_chunks_pos;   // позиция поля n_chunks (в данных)
};

// ─────────────────────────────────────────────────────────────
//  Вспомогательные функции записи/чтения big-endian
// ─────────────────────────────────────────────────────────────
inline void write_u8 (std::ostream& f, uint8_t  v) { f.write(reinterpret_cast<const char*>(&v), 1); }
inline void write_u16(std::ostream& f, uint16_t v) { uint8_t b[2]={(uint8_t)(v>>8),(uint8_t)v}; f.write((char*)b,2); }
inline void write_u32(std::ostream& f, uint32_t v) { uint8_t b[4]={(uint8_t)(v>>24),(uint8_t)(v>>16),(uint8_t)(v>>8),(uint8_t)v}; f.write((char*)b,4); }
inline void write_u64(std::ostream& f, uint64_t v) { for(int i=7;i>=0;--i){uint8_t b=(v>>(i*8))&0xFF;f.write((char*)&b,1);} }

inline uint8_t  read_u8 (std::istream& f) { uint8_t v=0; f.read((char*)&v,1); return v; }
inline uint16_t read_u16(std::istream& f) { uint8_t b[2]{}; f.read((char*)b,2); return (uint16_t)((b[0]<<8)|b[1]); }
inline uint32_t read_u32(std::istream& f) { uint8_t b[4]{}; f.read((char*)b,4); return ((uint32_t)b[0]<<24)|((uint32_t)b[1]<<16)|((uint32_t)b[2]<<8)|b[3]; }
inline uint64_t read_u64(std::istream& f) { uint64_t v=0; for(int i=0;i<8;++i){uint8_t b=0;f.read((char*)&b,1);v=(v<<8)|b;} return v; }

// ─────────────────────────────────────────────────────────────
//  Начало потоковой записи архива
//  Возвращает смещения для последующего обновления полей.
// ─────────────────────────────────────────────────────────────
inline ArchiveOffsets begin_stream_archive(std::fstream& f,
                                           const ArchiveHeader& hdr)
{
    // Магия
    const char* magic = (hdr.format == ArchiveFormat::CZIP) ? "CZIP" : "CRAR";
    f.write(magic, 4);
    write_u8(f, 0x01);                              // версия
    write_u8(f, static_cast<uint8_t>(hdr.method));
    write_u8(f, hdr.flags);
    write_u32(f, hdr.win_size);

    ArchiveOffsets off;
    off.orig_size_pos = f.tellp();
    write_u64(f, 0);   // placeholder orig_size

    off.crc32_pos = f.tellp();
    write_u32(f, 0);   // placeholder crc32

    write_u16(f, static_cast<uint16_t>(hdr.filename.size()));
    f.write(hdr.filename.data(), static_cast<std::streamsize>(hdr.filename.size()));

    off.n_chunks_pos = f.tellp();
    write_u32(f, 0);   // placeholder n_chunks

    return off;
}

// ─────────────────────────────────────────────────────────────
//  Запись одного чанка в поток
// ─────────────────────────────────────────────────────────────
inline void write_stream_chunk(std::fstream& f,
                                const std::vector<uint8_t>& data,
                                uint32_t orig_sz)
{
    write_u32(f, static_cast<uint32_t>(data.size()));
    write_u32(f, orig_sz);
    f.write(reinterpret_cast<const char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
}

// ─────────────────────────────────────────────────────────────
//  Финализация архива: обновляем placeholders
// ─────────────────────────────────────────────────────────────
inline void end_stream_archive(std::fstream& f,
                                const ArchiveOffsets& off,
                                uint64_t orig_size,
                                uint32_t crc32,
                                uint32_t n_chunks)
{
    f.seekp(off.orig_size_pos);
    write_u64(f, orig_size);

    f.seekp(off.crc32_pos);
    write_u32(f, crc32);

    f.seekp(off.n_chunks_pos);
    write_u32(f, n_chunks);

    f.seekp(0, std::ios::end);  // вернуть указатель в конец
}

// ─────────────────────────────────────────────────────────────
//  Чтение архива (для распаковки)
// ─────────────────────────────────────────────────────────────
inline std::pair<ArchiveHeader, std::vector<uint8_t>>
read_archive(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("Невозможно открыть файл: " + path);

    char magic[4];
    f.read(magic, 4);
    if (f.gcount() < 4) throw std::runtime_error("Файл слишком короткий");

    ArchiveHeader hdr;
    if      (std::memcmp(magic, "CZIP", 4) == 0) hdr.format = ArchiveFormat::CZIP;
    else if (std::memcmp(magic, "CRAR", 4) == 0) hdr.format = ArchiveFormat::CRAR;
    else throw std::runtime_error(
        "Неверная магическая последовательность — файл повреждён или не является архивом");

    uint8_t ver = read_u8(f);
    if (ver != 0x01) throw std::runtime_error("Неподдерживаемая версия архива");

    hdr.method    = static_cast<CompressMethod>(read_u8(f));
    hdr.flags     = read_u8(f);
    hdr.win_size  = read_u32(f);
    hdr.orig_size = read_u64(f);
    hdr.crc32     = read_u32(f);

    uint16_t name_len = read_u16(f);
    hdr.filename.resize(name_len);
    f.read(hdr.filename.data(), name_len);

    std::vector<uint8_t> data(
        (std::istreambuf_iterator<char>(f)),
         std::istreambuf_iterator<char>());

    return {hdr, data};
}

// ─────────────────────────────────────────────────────────────
//  Контрольные точки
// ─────────────────────────────────────────────────────────────
struct Checkpoint {
    uint32_t chunks_done = 0;
    uint32_t crc_accum   = 0xFFFFFFFFu;
    uint64_t output_pos  = 0;   // позиция в архиве после chunks_done чанков
    uint64_t orig_size   = 0;   // размер исходника (для проверки)
};

inline std::string checkpoint_path(const std::string& archive_path) {
    return archive_path + ".ckpt";
}

inline void save_checkpoint(const std::string& archive_path,
                            const Checkpoint& ckpt)
{
    std::ofstream f(checkpoint_path(archive_path), std::ios::binary);
    if (!f) return;
    write_u32(f, ckpt.chunks_done);
    write_u32(f, ckpt.crc_accum);
    write_u64(f, ckpt.output_pos);
    write_u64(f, ckpt.orig_size);
}

inline Checkpoint load_checkpoint(const std::string& archive_path) {
    std::ifstream f(checkpoint_path(archive_path), std::ios::binary);
    if (!f) return {};
    Checkpoint ckpt;
    ckpt.chunks_done = read_u32(f);
    ckpt.crc_accum   = read_u32(f);
    ckpt.output_pos  = read_u64(f);
    ckpt.orig_size   = read_u64(f);
    return ckpt;
}

inline void remove_checkpoint(const std::string& archive_path) {
    fs::remove(checkpoint_path(archive_path));
}

inline bool has_checkpoint(const std::string& archive_path) {
    return fs::exists(checkpoint_path(archive_path));
}

// Старый API оставляем для совместимости с decompress
inline void write_archive(const std::string& path,
                          const ArchiveHeader& hdr,
                          const std::vector<uint8_t>& data)
{
    std::fstream f(path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!f) throw std::runtime_error("Невозможно создать файл: " + path);
    auto off = begin_stream_archive(f, hdr);
    f.write(reinterpret_cast<const char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
    (void)off;
}
