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
//  ┌─ Заголовок ─────────────────────────────────────────────┐
//  │  Магия      4 байта  "CZIP" / "CRAR"                   │
//  │  Версия     1 байт   0x01                               │
//  │  Метод      1 байт   0=Huffman 1=LZ77 2=LZSS+RNC       │
//  │  Флаги      1 байт   зарезервировано                    │
//  │  Win-size   4 байта  BE                                 │
//  │  Orig-size  8 байт   BE  ← заглушка, обновляется после │
//  │  CRC32      4 байта  BE  ← заглушка, обновляется после │
//  │  Name-len   2 байта  BE                                 │
//  │  Name       N байт                                      │
//  └─────────────────────────────────────────────────────────┘
//  ┌─ Данные ────────────────────────────────────────────────┐
//  │  n_chunks   4 байта  BE  ← заглушка, обновляется после │
//  │  [чанк]*n:                                              │
//  │    csz      4 байта  BE  (бит31=1 → raw, без сжатия)   │
//  │    orig_sz  4 байта  BE                                 │
//  │    data     csz байт                                    │
//  └─────────────────────────────────────────────────────────┘
// ═══════════════════════════════════════════════════════════════

enum class ArchiveFormat { CZIP = 0, CRAR = 1 };
enum class CompressMethod { HUFFMAN = 0, LZ77 = 1, LZSS_RNC = 2 };

struct ArchiveHeader {
    ArchiveFormat  format    = ArchiveFormat::CZIP;
    CompressMethod method    = CompressMethod::HUFFMAN;
    uint8_t        flags     = 0;
    uint32_t       win_size  = 32768;
    uint64_t       orig_size = 0;
    uint32_t       crc32     = 0;
    std::string    filename;
};

// Смещения placeholder-полей для обновления seekp()
struct ArchiveOffsets {
    std::streampos orig_size_pos;
    std::streampos crc32_pos;
    std::streampos n_chunks_pos;
};

// ── I/O helpers ───────────────────────────────────────────────
inline void w8 (std::ostream& f,uint8_t v){f.write((char*)&v,1);}
inline void w16(std::ostream& f,uint16_t v){uint8_t b[2]={(uint8_t)(v>>8),(uint8_t)v};f.write((char*)b,2);}
inline void w32(std::ostream& f,uint32_t v){uint8_t b[4]={(uint8_t)(v>>24),(uint8_t)(v>>16),(uint8_t)(v>>8),(uint8_t)v};f.write((char*)b,4);}
inline void w64(std::ostream& f,uint64_t v){for(int i=7;i>=0;--i){uint8_t b=(v>>(i*8))&0xFF;f.write((char*)&b,1);}}
inline uint8_t  r8 (std::istream& f){uint8_t v=0;f.read((char*)&v,1);return v;}
inline uint16_t r16(std::istream& f){uint8_t b[2]{};f.read((char*)b,2);return(uint16_t)((b[0]<<8)|b[1]);}
inline uint32_t r32(std::istream& f){uint8_t b[4]{};f.read((char*)b,4);return((uint32_t)b[0]<<24)|((uint32_t)b[1]<<16)|((uint32_t)b[2]<<8)|b[3];}
inline uint64_t r64(std::istream& f){uint64_t v=0;for(int i=0;i<8;++i){uint8_t b=0;f.read((char*)&b,1);v=(v<<8)|b;}return v;}

// ── Потоковая запись ──────────────────────────────────────────

// Шаг 1: записать заголовок с заглушками
inline ArchiveOffsets begin_archive(std::fstream& f, const ArchiveHeader& hdr) {
    const char* magic = (hdr.format == ArchiveFormat::CZIP) ? "CZIP" : "CRAR";
    f.write(magic, 4);
    w8(f, 0x01);
    w8(f, static_cast<uint8_t>(hdr.method));
    w8(f, hdr.flags);
    w32(f, hdr.win_size);

    ArchiveOffsets off;
    off.orig_size_pos = f.tellp(); w64(f, 0);   // placeholder
    off.crc32_pos     = f.tellp(); w32(f, 0);   // placeholder
    w16(f, static_cast<uint16_t>(hdr.filename.size()));
    f.write(hdr.filename.data(),
            static_cast<std::streamsize>(hdr.filename.size()));
    off.n_chunks_pos  = f.tellp(); w32(f, 0);   // placeholder
    return off;
}

// Шаг 2: записать один чанк
inline void write_chunk(std::fstream& f,
                        const std::vector<uint8_t>& data,
                        uint32_t orig_sz_with_flags) {
    w32(f, static_cast<uint32_t>(data.size()));
    w32(f, orig_sz_with_flags);
    f.write(reinterpret_cast<const char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
}

// Шаг 3: заполнить заглушки реальными значениями
inline void end_archive(std::fstream& f, const ArchiveOffsets& off,
                        uint64_t orig_size, uint32_t crc32,
                        uint32_t n_chunks) {
    f.seekp(off.orig_size_pos); w64(f, orig_size);
    f.seekp(off.crc32_pos);     w32(f, crc32);
    f.seekp(off.n_chunks_pos);  w32(f, n_chunks);
    f.seekp(0, std::ios::end);
}

// ── Чтение архива ─────────────────────────────────────────────
inline std::pair<ArchiveHeader, std::vector<uint8_t>>
read_archive(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("Не удалось открыть: " + path);

    char magic[4]; f.read(magic, 4);
    if (f.gcount() < 4) throw std::runtime_error("Файл слишком короткий");

    ArchiveHeader hdr;
    if      (std::memcmp(magic,"CZIP",4)==0) hdr.format = ArchiveFormat::CZIP;
    else if (std::memcmp(magic,"CRAR",4)==0) hdr.format = ArchiveFormat::CRAR;
    else throw std::runtime_error("Неверный формат архива");

    if (r8(f) != 0x01) throw std::runtime_error("Неподдерживаемая версия");
    hdr.method   = static_cast<CompressMethod>(r8(f));
    hdr.flags    = r8(f);
    hdr.win_size = r32(f);
    hdr.orig_size= r64(f);
    hdr.crc32    = r32(f);
    uint16_t nl  = r16(f);
    hdr.filename.resize(nl); f.read(hdr.filename.data(), nl);

    std::vector<uint8_t> payload(
        (std::istreambuf_iterator<char>(f)), {});
    return {hdr, payload};
}
