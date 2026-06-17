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
//  Формат архива CZIP / CRAR
//
//  ┌─ Заголовок ─────────────────────────────────────────────┐
//  │  Магия      4 байта  "CZIP" / "CRAR"                   │
//  │  Версия     1 байт   0x01                               │
//  │  Метод      1 байт   0=Huffman 1=LZ77 2=LZSS+RNC       │
//  │  Флаги      1 байт   зарезервировано                    │
//  │  Win-size   4 байта  BE — размер окна LZ77              │
//  │  Orig-size  8 байт   BE — исходный размер               │
//  │  CRC32      4 байта  BE — CRC32 исходника               │
//  │  Name-len   2 байта  BE                                 │
//  │  Name       N байт   имя исходного файла                │
//  └─────────────────────────────────────────────────────────┘
//  Затем: сырые сжатые данные (payload)
// ═══════════════════════════════════════════════════════════════

enum class ArchiveFormat { CZIP = 0, CRAR = 1 };
enum class CompressMethod {
    HUFFMAN      = 0,
    LZ77         = 1,
    LZSS_RNC     = 2,
};

struct ArchiveHeader {
    ArchiveFormat  format    = ArchiveFormat::CZIP;
    CompressMethod method    = CompressMethod::HUFFMAN;
    uint8_t        flags     = 0;
    uint32_t       win_size  = 32768;
    uint64_t       orig_size = 0;
    uint32_t       crc32     = 0;
    std::string    filename;
};

// ── Helpers ───────────────────────────────────────────────────
inline void w8 (std::ostream& f, uint8_t  v) { f.write((char*)&v, 1); }
inline void w16(std::ostream& f, uint16_t v) { uint8_t b[2]={(uint8_t)(v>>8),(uint8_t)v}; f.write((char*)b,2); }
inline void w32(std::ostream& f, uint32_t v) { uint8_t b[4]={(uint8_t)(v>>24),(uint8_t)(v>>16),(uint8_t)(v>>8),(uint8_t)v}; f.write((char*)b,4); }
inline void w64(std::ostream& f, uint64_t v) { for(int i=7;i>=0;--i){uint8_t b=(v>>(i*8))&0xFF;f.write((char*)&b,1);} }

inline uint8_t  r8 (std::istream& f){uint8_t v=0;f.read((char*)&v,1);return v;}
inline uint16_t r16(std::istream& f){uint8_t b[2]{};f.read((char*)b,2);return(uint16_t)((b[0]<<8)|b[1]);}
inline uint32_t r32(std::istream& f){uint8_t b[4]{};f.read((char*)b,4);return((uint32_t)b[0]<<24)|((uint32_t)b[1]<<16)|((uint32_t)b[2]<<8)|b[3];}
inline uint64_t r64(std::istream& f){uint64_t v=0;for(int i=0;i<8;++i){uint8_t b=0;f.read((char*)&b,1);v=(v<<8)|b;}return v;}

// ── Запись архива ─────────────────────────────────────────────
inline void write_archive(const std::string& path,
                          const ArchiveHeader& hdr,
                          const std::vector<uint8_t>& payload)
{
    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("Не удалось создать файл: " + path);

    const char* magic = (hdr.format == ArchiveFormat::CZIP) ? "CZIP" : "CRAR";
    f.write(magic, 4);
    w8(f,  0x01);
    w8(f,  static_cast<uint8_t>(hdr.method));
    w8(f,  hdr.flags);
    w32(f, hdr.win_size);
    w64(f, hdr.orig_size);
    w32(f, hdr.crc32);
    w16(f, static_cast<uint16_t>(hdr.filename.size()));
    f.write(hdr.filename.data(), static_cast<std::streamsize>(hdr.filename.size()));
    f.write(reinterpret_cast<const char*>(payload.data()),
            static_cast<std::streamsize>(payload.size()));
}

// ── Чтение архива ─────────────────────────────────────────────
inline std::pair<ArchiveHeader, std::vector<uint8_t>>
read_archive(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("Не удалось открыть файл: " + path);

    char magic[4];
    f.read(magic, 4);
    if (f.gcount() < 4) throw std::runtime_error("Файл слишком короткий");

    ArchiveHeader hdr;
    if      (std::memcmp(magic, "CZIP", 4) == 0) hdr.format = ArchiveFormat::CZIP;
    else if (std::memcmp(magic, "CRAR", 4) == 0) hdr.format = ArchiveFormat::CRAR;
    else throw std::runtime_error("Неверный формат — не является архивом CZip");

    uint8_t ver = r8(f);
    if (ver != 0x01) throw std::runtime_error("Неподдерживаемая версия архива");

    hdr.method    = static_cast<CompressMethod>(r8(f));
    hdr.flags     = r8(f);
    hdr.win_size  = r32(f);
    hdr.orig_size = r64(f);
    hdr.crc32     = r32(f);

    uint16_t name_len = r16(f);
    hdr.filename.resize(name_len);
    f.read(hdr.filename.data(), name_len);

    std::vector<uint8_t> payload(
        (std::istreambuf_iterator<char>(f)), {});

    return {hdr, payload};
}
