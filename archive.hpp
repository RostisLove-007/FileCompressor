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
//  [Заголовок]
//    Магия      4 Б  "CZIP"/"CRAR"
//    Версия     1 Б  0x01
//    Метод      1 Б  0=Huffman 1=LZ77 2=LZSS+RNC
//    Флаги      1 Б  резерв
//    Win-size   4 Б  BE
//    Orig-size  8 Б  BE  ← заглушка
//    CRC32      4 Б  BE  ← заглушка
//    Name-len   2 Б  BE
//    Name       N Б
//  [Данные]
//    n_chunks   4 Б  BE  ← заглушка
//    [чанки]:
//      csz      4 Б  BE  (бит31=raw)
//      orig_sz  4 Б  BE
//      data     csz Б
//
//  Контрольная точка (.ckpt, рядом с архивом):
//    chunks_done  4 Б  BE — сколько чанков записано
//    crc_accum    4 Б  BE — накопленный CRC (без финализации)
//    output_pos   8 Б  BE — позиция записи в архиве
//    orig_size    8 Б  BE — размер исходника (для валидации)
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

struct ArchiveOffsets {
    std::streampos orig_size_pos;
    std::streampos crc32_pos;
    std::streampos n_chunks_pos;
};

struct Checkpoint {
    uint32_t chunks_done = 0;
    uint32_t crc_accum   = 0xFFFFFFFFu;
    uint64_t output_pos  = 0;
    uint64_t orig_size   = 0;
};

// ── I/O helpers ───────────────────────────────────────────────
inline void w8 (std::ostream& f,uint8_t  v){f.write((char*)&v,1);}
inline void w16(std::ostream& f,uint16_t v){uint8_t b[2]={(uint8_t)(v>>8),(uint8_t)v};f.write((char*)b,2);}
inline void w32(std::ostream& f,uint32_t v){uint8_t b[4]={(uint8_t)(v>>24),(uint8_t)(v>>16),(uint8_t)(v>>8),(uint8_t)v};f.write((char*)b,4);}
inline void w64(std::ostream& f,uint64_t v){for(int i=7;i>=0;--i){uint8_t b=(v>>(i*8))&0xFF;f.write((char*)&b,1);}}
inline uint8_t  r8 (std::istream& f){uint8_t  v=0;f.read((char*)&v,1);return v;}
inline uint16_t r16(std::istream& f){uint8_t  b[2]{};f.read((char*)b,2);return(uint16_t)((b[0]<<8)|b[1]);}
inline uint32_t r32(std::istream& f){uint8_t  b[4]{};f.read((char*)b,4);return((uint32_t)b[0]<<24)|((uint32_t)b[1]<<16)|((uint32_t)b[2]<<8)|b[3];}
inline uint64_t r64(std::istream& f){uint64_t v=0;for(int i=0;i<8;++i){uint8_t b=0;f.read((char*)&b,1);v=(v<<8)|b;}return v;}

// ── Потоковая запись ──────────────────────────────────────────
inline ArchiveOffsets begin_archive(std::fstream& f,
                                    const ArchiveHeader& hdr) {
    const char* magic = (hdr.format==ArchiveFormat::CZIP) ? "CZIP":"CRAR";
    f.write(magic,4);
    w8(f,0x01); w8(f,static_cast<uint8_t>(hdr.method)); w8(f,hdr.flags);
    w32(f,hdr.win_size);
    ArchiveOffsets off;
    off.orig_size_pos=f.tellp(); w64(f,0);
    off.crc32_pos    =f.tellp(); w32(f,0);
    w16(f,static_cast<uint16_t>(hdr.filename.size()));
    f.write(hdr.filename.data(),
            static_cast<std::streamsize>(hdr.filename.size()));
    off.n_chunks_pos =f.tellp(); w32(f,0);
    return off;
}

inline void write_chunk(std::fstream& f,
                        const std::vector<uint8_t>& data,
                        uint32_t orig_sz_with_flags) {
    w32(f,static_cast<uint32_t>(data.size()));
    w32(f,orig_sz_with_flags);
    f.write(reinterpret_cast<const char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
}

inline void end_archive(std::fstream& f, const ArchiveOffsets& off,
                        uint64_t orig_size, uint32_t crc32,
                        uint32_t n_chunks) {
    f.seekp(off.orig_size_pos); w64(f,orig_size);
    f.seekp(off.crc32_pos);     w32(f,crc32);
    f.seekp(off.n_chunks_pos);  w32(f,n_chunks);
    f.seekp(0,std::ios::end);
}

// ── Чтение архива ─────────────────────────────────────────────
inline std::pair<ArchiveHeader, std::vector<uint8_t>>
read_archive(const std::string& path) {
    std::ifstream f(path,std::ios::binary);
    if (!f) throw std::runtime_error("Не удалось открыть: "+path);
    char magic[4]; f.read(magic,4);
    if (f.gcount()<4) throw std::runtime_error("Файл слишком короткий");
    ArchiveHeader hdr;
    if      (std::memcmp(magic,"CZIP",4)==0) hdr.format=ArchiveFormat::CZIP;
    else if (std::memcmp(magic,"CRAR",4)==0) hdr.format=ArchiveFormat::CRAR;
    else throw std::runtime_error("Неверный формат архива");
    if (r8(f)!=0x01) throw std::runtime_error("Неподдерживаемая версия");
    hdr.method  =static_cast<CompressMethod>(r8(f));
    hdr.flags   =r8(f); hdr.win_size=r32(f);
    hdr.orig_size=r64(f); hdr.crc32=r32(f);
    uint16_t nl=r16(f); hdr.filename.resize(nl); f.read(hdr.filename.data(),nl);
    std::vector<uint8_t> payload((std::istreambuf_iterator<char>(f)),{});
    return {hdr,payload};
}

// ── Контрольные точки ─────────────────────────────────────────
inline std::string ckpt_path(const std::string& archive) {
    return archive + ".ckpt";
}

inline void save_checkpoint(const std::string& archive,
                            const Checkpoint& ck) {
    std::ofstream f(ckpt_path(archive),std::ios::binary);
    if (!f) return;
    w32(f,ck.chunks_done); w32(f,ck.crc_accum);
    w64(f,ck.output_pos);  w64(f,ck.orig_size);
}

inline Checkpoint load_checkpoint(const std::string& archive) {
    std::ifstream f(ckpt_path(archive),std::ios::binary);
    if (!f) return {};
    Checkpoint ck;
    ck.chunks_done=r32(f); ck.crc_accum=r32(f);
    ck.output_pos =r64(f); ck.orig_size=r64(f);
    return ck;
}

inline bool has_checkpoint(const std::string& archive) {
    return fs::exists(ckpt_path(archive));
}

inline void remove_checkpoint(const std::string& archive) {
    fs::remove(ckpt_path(archive));
}

// ── Восстановление смещений из существующего заголовка ────────
inline ArchiveOffsets read_offsets(std::fstream& f,
                                   const std::string& path) {
    // Читаем уже существующий заголовок чтобы найти позиции полей
    f.seekg(0);
    char magic[4]; f.read(magic,4);
    f.seekg(1+1+1+4, std::ios::cur);   // version+method+flags+win_size
    ArchiveOffsets off;
    off.orig_size_pos = f.tellg(); f.seekg(8, std::ios::cur);
    off.crc32_pos     = f.tellg(); f.seekg(4, std::ios::cur);
    uint16_t nl=r16(f);             f.seekg(nl,std::ios::cur);
    off.n_chunks_pos  = f.tellg();
    f.seekp(0, std::ios::end);
    return off;
}
