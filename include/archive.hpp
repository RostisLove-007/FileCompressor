#pragma once
#include <string>
#include <vector>
#include <fstream>
#include <filesystem>
#include <stdexcept>
#include <cstring>
#include <cstdint>
#include "crc32.hpp"

/**
 * @file archive.hpp
 * @brief Формат архива CZIP/CRAR, потоковая запись/чтение и контрольные точки.
 *
 * Структура архива:
 *
 * Заголовок (фиксированная часть):
 *  - Магия       4 байта  "CZIP" / "CRAR"
 *  - Версия      1 байт   0x01
 *  - Метод       1 байт   0=Huffman 1=LZ77 2=LZSS+RNC
 *  - Флаги       1 байт   зарезервировано
 *  - Win-size    4 байта  (big-endian)
 *  - Orig-size   8 байт   исходный размер (обновляется по завершении)
 *  - CRC32       4 байта  CRC исходного файла (обновляется по завершении)
 *  - Name-len    2 байта
 *  - Name        N байт
 *
 * Данные:
 *  - n_chunks    4 байта  количество чанков (обновляется по завершении)
 *  - [чанк]*n:
 *      - csz     4 байта  размер сжатого блока (бит 31 = признак "хранится без сжатия")
 *      - orig_sz 4 байта  размер блока в исходном виде
 *      - data    csz байт сжатые (или исходные) данные блока
 *
 * Контрольная точка (файл "<архив>.ckpt"):
 *  - 4 байта  chunks_done  — сколько чанков уже записано
 *  - 4 байта  crc_accum    — накопленный CRC32 (без финальной инверсии)
 *  - 8 байт   output_pos   — позиция конца последнего записанного чанка в архиве
 *  - 8 байт   orig_size    — полный размер исходного файла (для проверки совместимости)
 */

namespace fs = std::filesystem;

/**
 * @brief Формат контейнера архива.
 */
enum class ArchiveFormat {
    CZIP = 0,  ///< ZIP-подобный формат (Huffman или LZ77+Huffman).
    CRAR = 1   ///< RAR-подобный формат (LZSS+RNC).
};

/**
 * @brief Метод сжатия, применённый к данным архива.
 */
enum class CompressMethod {
    HUFFMAN      = 0,  ///< Классическое кодирование Хаффмана.
    LZ77         = 1,  ///< LZ77 со скользящим окном, дополнительно сжатый Хаффманом.
    PPM_LZSS_RNC = 2   ///< Конвейер LZSS → RNC.
};

/**
 * @brief Заголовок архива CZIP/CRAR.
 *
 * Соответствует фиксированной части заголовка, описанной в формате
 * файла (см. @ref archive.hpp).
 */
struct ArchiveHeader {
    ArchiveFormat  format   = ArchiveFormat::CZIP;     ///< Формат контейнера (CZIP/CRAR).
    CompressMethod method   = CompressMethod::HUFFMAN; ///< Метод сжатия данных.
    uint8_t        flags    = 0;                       ///< Зарезервированные флаги.
    uint32_t       win_size = 32768;                   ///< Размер окна, использованный при сжатии.
    uint64_t       orig_size= 0;                       ///< Исходный (несжатый) размер файла.
    uint32_t       crc32    = 0;                       ///< CRC32 исходных данных файла.
    std::string    filename;                           ///< Имя исходного файла без пути.
};

/**
 * @brief Позиции полей заголовка, которые обновляются после записи всех чанков.
 *
 * Вычисляются один раз при открытии (создании) архива методом
 * begin_stream_archive() и используются позже в end_stream_archive()
 * для записи итоговых значений размера, CRC и числа чанков.
 */
struct ArchiveOffsets {
    std::streampos orig_size_pos;  ///< Позиция поля orig_size в заголовке архива.
    std::streampos crc32_pos;      ///< Позиция поля crc32 в заголовке архива.
    std::streampos n_chunks_pos;   ///< Позиция поля n_chunks в данных архива.
};

/**
 * @brief Записывает 8-битное целое в поток.
 * @param f Поток вывода, в который выполняется запись.
 * @param v Значение для записи.
 */
inline void write_u8 (std::ostream& f, uint8_t  v) { f.write(reinterpret_cast<const char*>(&v), 1); }

/**
 * @brief Записывает 16-битное целое в поток в порядке big-endian.
 * @param f Поток вывода, в который выполняется запись.
 * @param v Значение для записи.
 */
inline void write_u16(std::ostream& f, uint16_t v) { uint8_t b[2]={(uint8_t)(v>>8),(uint8_t)v}; f.write((char*)b,2); }

/**
 * @brief Записывает 32-битное целое в поток в порядке big-endian.
 * @param f Поток вывода, в который выполняется запись.
 * @param v Значение для записи.
 */
inline void write_u32(std::ostream& f, uint32_t v) { uint8_t b[4]={(uint8_t)(v>>24),(uint8_t)(v>>16),(uint8_t)(v>>8),(uint8_t)v}; f.write((char*)b,4); }

/**
 * @brief Записывает 64-битное целое в поток в порядке big-endian.
 * @param f Поток вывода, в который выполняется запись.
 * @param v Значение для записи.
 */
inline void write_u64(std::ostream& f, uint64_t v) { for(int i=7;i>=0;--i){uint8_t b=(v>>(i*8))&0xFF;f.write((char*)&b,1);} }

/**
 * @brief Читает 8-битное целое из потока.
 * @param f Поток ввода, из которого выполняется чтение.
 * @return Прочитанное значение.
 */
inline uint8_t  read_u8 (std::istream& f) { uint8_t v=0; f.read((char*)&v,1); return v; }

/**
 * @brief Читает 16-битное целое из потока в порядке big-endian.
 * @param f Поток ввода, из которого выполняется чтение.
 * @return Прочитанное значение.
 */
inline uint16_t read_u16(std::istream& f) { uint8_t b[2]{}; f.read((char*)b,2); return (uint16_t)((b[0]<<8)|b[1]); }

/**
 * @brief Читает 32-битное целое из потока в порядке big-endian.
 * @param f Поток ввода, из которого выполняется чтение.
 * @return Прочитанное значение.
 */
inline uint32_t read_u32(std::istream& f) { uint8_t b[4]{}; f.read((char*)b,4); return ((uint32_t)b[0]<<24)|((uint32_t)b[1]<<16)|((uint32_t)b[2]<<8)|b[3]; }

/**
 * @brief Читает 64-битное целое из потока в порядке big-endian.
 * @param f Поток ввода, из которого выполняется чтение.
 * @return Прочитанное значение.
 */
inline uint64_t read_u64(std::istream& f) { uint64_t v=0; for(int i=0;i<8;++i){uint8_t b=0;f.read((char*)&b,1);v=(v<<8)|b;} return v; }

/**
 * @brief Открывает поток на запись и пишет фиксированную часть заголовка архива.
 *
 * Пишет магическую последовательность, версию, метод сжатия, флаги, размер
 * окна и имя файла; для изменяемых далее полей (orig_size, crc32, n_chunks)
 * резервирует место плейсхолдерами и запоминает их позиции для последующего
 * обновления функцией end_stream_archive().
 *
 * @param f   Открытый файловый поток, готовый к записи (в режиме binary/out).
 * @param hdr Заголовок архива с параметрами формата, метода и именем файла.
 * @return Структура смещений изменяемых полей заголовка.
 */
inline ArchiveOffsets begin_stream_archive(std::fstream& f,
                                           const ArchiveHeader& hdr)
{
    const char* magic = (hdr.format == ArchiveFormat::CZIP) ? "CZIP" : "CRAR";
    f.write(magic, 4);
    write_u8(f, 0x01);
    write_u8(f, static_cast<uint8_t>(hdr.method));
    write_u8(f, hdr.flags);
    write_u32(f, hdr.win_size);

    ArchiveOffsets off;
    off.orig_size_pos = f.tellp();
    write_u64(f, 0);

    off.crc32_pos = f.tellp();
    write_u32(f, 0);

    write_u16(f, static_cast<uint16_t>(hdr.filename.size()));
    f.write(hdr.filename.data(), static_cast<std::streamsize>(hdr.filename.size()));

    off.n_chunks_pos = f.tellp();
    write_u32(f, 0);

    return off;
}

/**
 * @brief Записывает один чанк сжатых данных в открытый поток архива.
 * @param f        Открытый файловый поток архива, позиционированный на конец данных.
 * @param data      Байты чанка (сжатые либо исходные, если сжатие было невыгодным).
 * @param orig_sz   Размер чанка в исходном виде; старший бит (0x80000000) установлен,
 *                  если @p data хранит исходные (несжатые) данные.
 */
inline void write_stream_chunk(std::fstream& f,
                                const std::vector<uint8_t>& data,
                                uint32_t orig_sz)
{
    write_u32(f, static_cast<uint32_t>(data.size()));
    write_u32(f, orig_sz);
    f.write(reinterpret_cast<const char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
}

/**
 * @brief Завершает запись архива, обновляя ранее зарезервированные поля заголовка.
 *
 * Записывает итоговый исходный размер, CRC32 и количество чанков на места,
 * вычисленные begin_stream_archive(), после чего возвращает позицию записи
 * в конец файла.
 *
 * @param f        Открытый файловый поток архива.
 * @param off      Смещения полей заголовка, полученные от begin_stream_archive().
 * @param orig_size Итоговый исходный размер сжатого файла.
 * @param crc32     Итоговое значение CRC32 исходных данных.
 * @param n_chunks  Итоговое количество записанных чанков.
 */
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

    f.seekp(0, std::ios::end);
}

/**
 * @brief Читает архив целиком и разбирает его заголовок.
 * @param path Путь к файлу архива.
 * @return Пара из заголовка архива и оставшихся данных (всё, что идёт после заголовка).
 * @throws std::runtime_error если файл не удалось открыть, он слишком короткий,
 *         содержит неверную магическую последовательность или неподдерживаемую версию.
 */
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

/**
 * @brief Контрольная точка, описывающая прогресс прерванного сжатия.
 *
 * Используется для возобновления сжатия большого файла после отмены
 * или сбоя, без повторной обработки уже записанных чанков.
 */
struct Checkpoint {
    uint32_t chunks_done = 0;          ///< Количество уже записанных чанков.
    uint32_t crc_accum   = 0xFFFFFFFFu;///< Накопленный (неинвертированный) CRC32.
    uint64_t output_pos  = 0;          ///< Позиция в архиве сразу после последнего записанного чанка.
    uint64_t orig_size   = 0;          ///< Полный размер исходного файла (для проверки совместимости).
};

/**
 * @brief Строит путь к файлу контрольной точки для заданного архива.
 * @param archive_path Путь к файлу архива.
 * @return Путь к файлу контрольной точки (архив с суффиксом ".ckpt").
 */
inline std::string checkpoint_path(const std::string& archive_path) {
    return archive_path + ".ckpt";
}

/**
 * @brief Сохраняет контрольную точку на диск.
 * @param archive_path Путь к архиву, для которого сохраняется контрольная точка.
 * @param ckpt         Данные контрольной точки для сохранения.
 */
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

/**
 * @brief Загружает контрольную точку с диска.
 * @param archive_path Путь к архиву, для которого загружается контрольная точка.
 * @return Загруженная контрольная точка, либо контрольная точка по умолчанию
 *         (нулевой прогресс), если файл контрольной точки не найден.
 */
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

/**
 * @brief Удаляет файл контрольной точки, связанной с архивом.
 * @param archive_path Путь к архиву, чья контрольная точка удаляется.
 */
inline void remove_checkpoint(const std::string& archive_path) {
    fs::remove(checkpoint_path(archive_path));
}

/**
 * @brief Проверяет наличие файла контрольной точки для архива.
 * @param archive_path Путь к архиву.
 * @return @c true, если файл контрольной точки существует.
 */
inline bool has_checkpoint(const std::string& archive_path) {
    return fs::exists(checkpoint_path(archive_path));
}

/**
 * @brief Создаёт архив и записывает в него заголовок и весь блок данных целиком.
 *
 * Упрощённый (нечанковый) способ записи архива, оставленный для совместимости
 * с кодом, использующим простую модель «записать всё за один вызов»
 * (например, чтение в decompress() через read_archive()).
 *
 * @param path Путь к создаваемому файлу архива.
 * @param hdr  Заголовок архива.
 * @param data Полный блок данных, который требуется записать после заголовка.
 * @throws std::runtime_error если файл не удалось создать.
 */
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
