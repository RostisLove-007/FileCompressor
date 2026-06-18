/**
 * @file test_all.cpp
 * @brief Тесты алгоритмов сжатия (Google Test).
 *
 * Покрывают положительные и отрицательные сценарии для:
 *  - BitWriter / BitReader
 *  - CRC32
 *  - HuffmanCoder
 *  - LZ77Coder
 *  - LZSSCoder
 *  - RNCCoder
 *  - PPMCoder
 *  - FileDetector
 *  - Функций архива (archive.hpp)
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "archive.hpp"
#include "bit_io.hpp"
#include "crc32.hpp"
#include "file_detector.hpp"
#include "huffman.hpp"
#include "lz77.hpp"
#include "lzss.hpp"
#include "ppm.hpp"
#include "rnc.hpp"

namespace fs = std::filesystem;

// ─── Вспомогательные функции ──────────────────────────────────

static std::vector<uint8_t> from_str(const std::string& s) {
    return {s.begin(), s.end()};
}

static std::vector<uint8_t> rnd_bytes(size_t n, uint32_t seed = 42) {
    std::mt19937 rng(seed);
    std::vector<uint8_t> v(n);
    for (auto& b : v) b = static_cast<uint8_t>(rng());
    return v;
}

static std::vector<uint8_t> rep_bytes(uint8_t byte, size_t n) {
    return std::vector<uint8_t>(n, byte);
}

// ═════════════════════════════════════════════════════════════
//  BitWriter / BitReader
// ═════════════════════════════════════════════════════════════

class BitIOTest : public ::testing::Test {};

TEST_F(BitIOTest, WriteThenReadSingleBit) {
    std::vector<uint8_t> buf;
    BitWriter bw(buf);
    bw.write_bit(true);
    bw.flush();

    BitReader br(buf.data(), buf.size());
    EXPECT_TRUE(br.read_bit());
}

TEST_F(BitIOTest, WriteThenReadZeroBit) {
    std::vector<uint8_t> buf;
    BitWriter bw(buf);
    bw.write_bit(false);
    bw.flush();

    BitReader br(buf.data(), buf.size());
    EXPECT_FALSE(br.read_bit());
}

TEST_F(BitIOTest, WriteBitsRoundTrip5Bits) {
    std::vector<uint8_t> buf;
    BitWriter bw(buf);
    bw.write_bits(0b10110, 5);
    bw.flush();

    BitReader br(buf.data(), buf.size());
    EXPECT_EQ(br.read_bits(5), 0b10110u);
}

TEST_F(BitIOTest, WriteBitsRoundTrip16Bits) {
    std::vector<uint8_t> buf;
    BitWriter bw(buf);
    bw.write_bits(0xABCD, 16);
    bw.flush();

    BitReader br(buf.data(), buf.size());
    EXPECT_EQ(br.read_bits(16), 0xABCDu);
}

TEST_F(BitIOTest, MultipleBitSequences) {
    std::vector<uint8_t> buf;
    BitWriter bw(buf);
    bw.write_bits(0b111, 3);
    bw.write_bits(0b00000, 5);
    bw.write_bits(0xFF, 8);
    bw.flush();

    BitReader br(buf.data(), buf.size());
    EXPECT_EQ(br.read_bits(3), 0b111u);
    EXPECT_EQ(br.read_bits(5), 0b00000u);
    EXPECT_EQ(br.read_bits(8), 0xFFu);
}

TEST_F(BitIOTest, FlushReturnsCorrectPadCount) {
    std::vector<uint8_t> buf;
    BitWriter bw(buf);
    bw.write_bits(0b101, 3);  // 3 бита → 5 заполнителей
    int pad = bw.flush();
    EXPECT_EQ(pad, 5);
}

TEST_F(BitIOTest, FlushOnByteAlignedBoundaryReturnsZero) {
    std::vector<uint8_t> buf;
    BitWriter bw(buf);
    bw.write_bits(0xAB, 8);
    int pad = bw.flush();
    EXPECT_EQ(pad, 0);
}

TEST_F(BitIOTest, EofDetection) {
    std::vector<uint8_t> buf;
    BitWriter bw(buf);
    bw.write_bits(0xFF, 8);
    bw.flush();

    BitReader br(buf.data(), buf.size());
    EXPECT_FALSE(br.eof());
    br.read_bits(8);
    EXPECT_TRUE(br.eof());
}

TEST_F(BitIOTest, ReadBeyondEndThrows) {
    std::vector<uint8_t> buf;
    BitWriter bw(buf);
    bw.write_bits(0xFF, 8);
    bw.flush();

    BitReader br(buf.data(), buf.size());
    br.read_bits(8);
    EXPECT_THROW(br.read_bit(), std::runtime_error);
}

TEST_F(BitIOTest, BytesConsumed) {
    std::vector<uint8_t> buf;
    BitWriter bw(buf);
    bw.write_bits(0xFFFF, 16);
    bw.flush();

    BitReader br(buf.data(), buf.size());
    br.read_bits(8);
    EXPECT_EQ(br.bytes_consumed(), 1u);
    br.read_bits(8);
    EXPECT_EQ(br.bytes_consumed(), 2u);
}

// ═════════════════════════════════════════════════════════════
//  CRC32
// ═════════════════════════════════════════════════════════════

class CRC32Test : public ::testing::Test {};

TEST_F(CRC32Test, KnownValue123456789) {
    std::string s = "123456789";
    uint32_t c = CRC32::compute(reinterpret_cast<const uint8_t*>(s.data()), s.size());
    EXPECT_EQ(c, 0xCBF43926u);
}

TEST_F(CRC32Test, EmptyInputReturnsZero) {
    EXPECT_EQ(CRC32::compute(nullptr, 0), 0x00000000u);
}

TEST_F(CRC32Test, VectorOverload) {
    std::vector<uint8_t> v = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    EXPECT_EQ(CRC32::compute(v), 0xCBF43926u);
}

TEST_F(CRC32Test, IncrementalMatchesSingleCall) {
    std::vector<uint8_t> data = rnd_bytes(1024);
    uint32_t single = CRC32::compute(data.data(), data.size());

    uint32_t accum = 0xFFFFFFFFu;
    accum = CRC32::update(accum, data.data(), 512);
    accum = CRC32::update(accum, data.data() + 512, 512);
    uint32_t incremental = CRC32::finalize(accum);

    EXPECT_EQ(incremental, single);
}

TEST_F(CRC32Test, DifferentDataDifferentCRC) {
    std::vector<uint8_t> a = {0x01, 0x02};
    std::vector<uint8_t> b = {0x01, 0x03};
    EXPECT_NE(CRC32::compute(a), CRC32::compute(b));
}

TEST_F(CRC32Test, SingleByteKnownValue) {
    uint8_t b = 0x00;
    EXPECT_EQ(CRC32::compute(&b, 1), 0xD202EF8Du);
}

TEST_F(CRC32Test, ObjectUpdateAndValue) {
    CRC32 c;
    std::string s = "123456789";
    c.update(reinterpret_cast<const uint8_t*>(s.data()), s.size());
    EXPECT_EQ(c.value(), 0xCBF43926u);
}

TEST_F(CRC32Test, ObjectReset) {
    CRC32 c;
    c.update(reinterpret_cast<const uint8_t*>("abc"), 3);
    c.reset();
    // После сброса состояние как у пустого
    EXPECT_EQ(c.value(), 0x00000000u);
}

// ═════════════════════════════════════════════════════════════
//  HuffmanCoder
// ═════════════════════════════════════════════════════════════

class HuffmanTest : public ::testing::Test {
protected:
    HuffmanCoder hc;
};

TEST_F(HuffmanTest, EmptyInput) {
    auto enc = hc.encode({});
    auto dec = hc.decode(enc);
    EXPECT_TRUE(dec.empty());
}

TEST_F(HuffmanTest, SingleByte) {
    auto data = std::vector<uint8_t>{0x42};
    auto dec = hc.decode(hc.encode(data));
    EXPECT_EQ(dec, data);
}

TEST_F(HuffmanTest, AllSameBytes) {
    auto data = rep_bytes('A', 1000);
    auto dec = hc.decode(hc.encode(data));
    EXPECT_EQ(dec, data);
}

TEST_F(HuffmanTest, AllPossibleBytes) {
    std::vector<uint8_t> data(256);
    for (int i = 0; i < 256; ++i) data[i] = static_cast<uint8_t>(i);
    auto dec = hc.decode(hc.encode(data));
    EXPECT_EQ(dec, data);
}

TEST_F(HuffmanTest, RepetitiveText) {
    auto data = from_str("hello world hello world hello world");
    auto dec = hc.decode(hc.encode(data));
    EXPECT_EQ(dec, data);
}

TEST_F(HuffmanTest, RandomData4KB) {
    auto data = rnd_bytes(4096);
    auto dec = hc.decode(hc.encode(data));
    EXPECT_EQ(dec, data);
}

TEST_F(HuffmanTest, CompressesRepetitiveData) {
    auto data = rep_bytes('X', 10000);
    auto enc = hc.encode(data);
    // Хаффман должен хорошо сжимать однородные данные
    EXPECT_LT(enc.size(), data.size());
}

TEST_F(HuffmanTest, DecodeCorruptedHeaderThrows) {
    // Слишком короткий буфер
    std::vector<uint8_t> bad(10, 0);
    EXPECT_THROW(hc.decode(bad), std::runtime_error);
}

TEST_F(HuffmanTest, DecodeGarbageThrows) {
    // Буфер правильной длины (≥265) но данные указывают на orig_size=0xFFFF..
    // и мусорное дерево — кодек должен бросить исключение (любое std::exception)
    std::vector<uint8_t> bad(300, 0xFF);
    // orig_size в позиции [256..263] оставим 0xFFFF..., pad[264]=0xFF
    // Декодер попытается зарезервировать огромный вектор и бросит исключение
    EXPECT_THROW(hc.decode(bad), std::exception);
}

TEST_F(HuffmanTest, LargeData16KB) {
    auto data = rnd_bytes(16384, 99);
    auto dec = hc.decode(hc.encode(data));
    EXPECT_EQ(dec, data);
}

// ═════════════════════════════════════════════════════════════
//  LZ77Coder
// ═════════════════════════════════════════════════════════════

class LZ77Test : public ::testing::Test {};

TEST_F(LZ77Test, EmptyInput) {
    LZ77Coder lz;
    auto dec = lz.decode(lz.encode({}));
    EXPECT_TRUE(dec.empty());
}

TEST_F(LZ77Test, SingleByte) {
    LZ77Coder lz;
    auto data = std::vector<uint8_t>{0x55};
    EXPECT_EQ(lz.decode(lz.encode(data)), data);
}

TEST_F(LZ77Test, HighlyRepetitiveData) {
    LZ77Coder lz;
    auto data = rep_bytes('A', 5000);
    EXPECT_EQ(lz.decode(lz.encode(data)), data);
}

TEST_F(LZ77Test, RepeatingPattern) {
    LZ77Coder lz;
    auto data = from_str("abcdefabcdefabcdefabcdef");
    EXPECT_EQ(lz.decode(lz.encode(data)), data);
}

TEST_F(LZ77Test, RandomData) {
    LZ77Coder lz;
    auto data = rnd_bytes(2048, 7);
    EXPECT_EQ(lz.decode(lz.encode(data)), data);
}

TEST_F(LZ77Test, SmallWindow) {
    LZ77Coder lz(256);
    auto data = from_str("hello world hello world");
    EXPECT_EQ(lz.decode(lz.encode(data)), data);
}

TEST_F(LZ77Test, LargeWindow) {
    LZ77Coder lz(65536);
    auto data = rnd_bytes(4096, 13);
    EXPECT_EQ(lz.decode(lz.encode(data)), data);
}

TEST_F(LZ77Test, CompressesRepetitiveData) {
    LZ77Coder lz;
    auto data = from_str(std::string(200, 'A') + std::string(200, 'B'));
    auto enc = lz.encode(data);
    EXPECT_LT(enc.size(), data.size());
}

TEST_F(LZ77Test, SetWindowAccessor) {
    LZ77Coder lz;
    lz.set_window(4096);
    EXPECT_EQ(lz.window_size(), 4096u);
}

TEST_F(LZ77Test, DecodeCorruptedHeaderThrows) {
    LZ77Coder lz;
    std::vector<uint8_t> bad(5, 0);
    EXPECT_THROW(lz.decode(bad), std::runtime_error);
}

TEST_F(LZ77Test, DecodeTruncatedDataThrows) {
    LZ77Coder lz;
    auto data = from_str("hello world");
    auto enc = lz.encode(data);
    enc.resize(enc.size() / 2);  // обрезаем
    EXPECT_THROW(lz.decode(enc), std::runtime_error);
}

// ═════════════════════════════════════════════════════════════
//  LZSSCoder
// ═════════════════════════════════════════════════════════════

class LZSSTest : public ::testing::Test {};

TEST_F(LZSSTest, EmptyInput) {
    LZSSCoder lz;
    auto dec = lz.decode(lz.encode({}));
    EXPECT_TRUE(dec.empty());
}

TEST_F(LZSSTest, SingleByte) {
    LZSSCoder lz;
    auto data = std::vector<uint8_t>{0xAB};
    EXPECT_EQ(lz.decode(lz.encode(data)), data);
}

TEST_F(LZSSTest, RepetitiveData) {
    LZSSCoder lz;
    auto data = rep_bytes('Z', 3000);
    EXPECT_EQ(lz.decode(lz.encode(data)), data);
}

TEST_F(LZSSTest, TextWithRepeats) {
    LZSSCoder lz;
    auto data = from_str(
        "the quick brown fox jumps over the lazy dog "
        "the quick brown fox jumps over the lazy dog");
    EXPECT_EQ(lz.decode(lz.encode(data)), data);
}

TEST_F(LZSSTest, RandomData) {
    LZSSCoder lz;
    auto data = rnd_bytes(4096, 123);
    EXPECT_EQ(lz.decode(lz.encode(data)), data);
}

TEST_F(LZSSTest, AllPossibleByteValues) {
    LZSSCoder lz;
    std::vector<uint8_t> data(256);
    for (int i = 0; i < 256; ++i) data[i] = static_cast<uint8_t>(i);
    EXPECT_EQ(lz.decode(lz.encode(data)), data);
}

TEST_F(LZSSTest, CompressesRepetitiveData) {
    LZSSCoder lz;
    auto data = rep_bytes('Q', 5000);
    auto enc = lz.encode(data);
    EXPECT_LT(enc.size(), data.size());
}

TEST_F(LZSSTest, DecodeShortHeaderThrows) {
    LZSSCoder lz;
    std::vector<uint8_t> bad(4, 0);
    EXPECT_THROW(lz.decode(bad), std::runtime_error);
}

TEST_F(LZSSTest, DecodeTruncatedReferenceThrows) {
    LZSSCoder lz;
    // Создадим вручную «архив» с ctrl-байтом, где бит=1, но ссылка обрезана
    std::vector<uint8_t> faked(8 + 2, 0);
    // 8 байт orig_size = 100
    faked[7] = 100;
    // ctrl = 0b10000000 (первый токен — ссылка)
    faked[8] = 0x80;
    // только 1 байт ссылки вместо 2 → усечение
    faked[9] = 0xAA;
    EXPECT_THROW(lz.decode(faked), std::runtime_error);
}

// ═════════════════════════════════════════════════════════════
//  RNCCoder
// ═════════════════════════════════════════════════════════════

class RNCTest : public ::testing::Test {};

TEST_F(RNCTest, SingleByte) {
    RNCCoder rnc;
    auto data = std::vector<uint8_t>{0x42};
    EXPECT_EQ(rnc.decode(rnc.encode(data)), data);
}

TEST_F(RNCTest, RepetitiveData4KB) {
    RNCCoder rnc;
    auto data = rep_bytes('R', 4096);
    EXPECT_EQ(rnc.decode(rnc.encode(data)), data);
}

TEST_F(RNCTest, TextWithRepeats) {
    RNCCoder rnc;
    auto data = from_str("Hello RNC! Hello RNC! Hello RNC!");
    EXPECT_EQ(rnc.decode(rnc.encode(data)), data);
}

TEST_F(RNCTest, RandomData8KB) {
    RNCCoder rnc;
    auto data = rnd_bytes(8192, 77);
    EXPECT_EQ(rnc.decode(rnc.encode(data)), data);
}

TEST_F(RNCTest, MultipleChunks) {
    // Данные больше одного чанка (4096 байт)
    RNCCoder rnc;
    auto data = rnd_bytes(10000, 55);
    EXPECT_EQ(rnc.decode(rnc.encode(data)), data);
}

TEST_F(RNCTest, CrossChunkReferences) {
    // Повторяющиеся данные, пересекающие границы чанков
    RNCCoder rnc;
    std::string pattern = "abcdefghij";
    std::string text;
    for (int i = 0; i < 600; ++i) text += pattern;
    auto data = from_str(text);
    EXPECT_EQ(rnc.decode(rnc.encode(data)), data);
}

TEST_F(RNCTest, DecodeWrongMagicThrows) {
    RNCCoder rnc;
    std::vector<uint8_t> bad = {'X', 'N', 'C', 0x01, 0, 0, 0, 5, 0, 0, 0, 0, 0, 0};
    EXPECT_THROW(rnc.decode(bad), std::runtime_error);
}

TEST_F(RNCTest, DecodeTooShortThrows) {
    RNCCoder rnc;
    std::vector<uint8_t> bad(5, 0);
    EXPECT_THROW(rnc.decode(bad), std::runtime_error);
}

TEST_F(RNCTest, DetectsCRCCorruption) {
    RNCCoder rnc;
    auto enc = rnc.encode(from_str("test payload data for RNC"));
    // Портим данные чанка (после заголовка 14 байт + 2 байт размера чанка)
    enc[20] ^= 0xFF;
    EXPECT_THROW(rnc.decode(enc), std::runtime_error);
}

TEST_F(RNCTest, DetectsOrigSizeMismatch) {
    RNCCoder rnc;
    auto enc = rnc.encode(from_str("hello"));
    // Меняем orig_size в заголовке на несоответствующий
    enc[4] = 0;
    enc[5] = 0;
    enc[6] = 0;
    enc[7] = 99;
    EXPECT_THROW(rnc.decode(enc), std::runtime_error);
}

// ═════════════════════════════════════════════════════════════
//  PPMCoder
// ═════════════════════════════════════════════════════════════

class PPMTest : public ::testing::Test {};

TEST_F(PPMTest, EncodeProducesOutput) {
    // Проверяем, что encode возвращает непустой результат с правильным заголовком
    PPMCoder ppm;
    auto data = from_str("hello world");
    auto enc = ppm.encode(data);
    // Минимум: 8 байт orig_size + хоть что-то
    EXPECT_GT(enc.size(), 8u);
    // orig_size должен совпадать с длиной входа
    uint64_t orig = 0;
    for (int i = 0; i < 8; ++i) orig = (orig << 8) | enc[i];
    EXPECT_EQ(orig, data.size());
}

TEST_F(PPMTest, EncodeEmptyProducesHeader) {
    PPMCoder ppm;
    auto enc = ppm.encode({});
    EXPECT_GE(enc.size(), 8u);
    uint64_t orig = 0;
    for (int i = 0; i < 8; ++i) orig = (orig << 8) | enc[i];
    EXPECT_EQ(orig, 0u);
}

TEST_F(PPMTest, EncodeRepetitiveCompresses) {
    PPMCoder ppm;
    // PPM должен хорошо сжимать повторяющиеся данные
    auto data = rep_bytes('A', 500);
    auto enc = ppm.encode(data);
    EXPECT_LT(enc.size(), data.size());
}

TEST_F(PPMTest, EncodeDifferentOrigSizeInHeader) {
    PPMCoder ppm1, ppm2;
    auto enc1 = ppm1.encode(from_str("aaa"));
    auto enc2 = ppm2.encode(from_str("aaaaaaaaaa"));
    // orig_size в первых 8 байтах заголовка должен отличаться
    uint64_t orig1 = 0, orig2 = 0;
    for (int i = 0; i < 8; ++i) {
        orig1 = (orig1 << 8) | enc1[i];
        orig2 = (orig2 << 8) | enc2[i];
    }
    EXPECT_NE(orig1, orig2);
    EXPECT_EQ(orig1, 3u);
    EXPECT_EQ(orig2, 10u);
}

TEST_F(PPMTest, DecodeShortHeaderThrows) {
    PPMCoder ppm;
    std::vector<uint8_t> bad(5, 0);
    EXPECT_THROW(ppm.decode(bad), std::runtime_error);
}

TEST_F(PPMTest, DecodeZeroOrigSize) {
    // 8 байт orig_size=0, затем что-то — должен вернуть пустой вектор или не упасть
    PPMCoder ppm;
    std::vector<uint8_t> data(16, 0);  // orig_size = 0
    auto result = ppm.decode(data);
    EXPECT_TRUE(result.empty());
}

TEST_F(PPMTest, ArithEncoderProducesOutput) {
    std::vector<uint8_t> out;
    ArithEncoder enc(out);
    enc.encode(0, 1, 2);  // символ 0 из 2
    enc.encode(1, 2, 2);  // символ 1 из 2
    enc.flush();
    EXPECT_GT(out.size(), 0u);
}

// ═════════════════════════════════════════════════════════════
//  FileDetector
// ═════════════════════════════════════════════════════════════

class FileDetectorTest : public ::testing::Test {};

TEST_F(FileDetectorTest, ZIPMagic) {
    std::vector<uint8_t> data = {0x50, 0x4B, 0x03, 0x04, 0, 0, 0, 0};
    auto r = FileDetector::detect(data);
    EXPECT_EQ(r.mime, "application/zip");
    EXPECT_TRUE(r.is_archive);
}

TEST_F(FileDetectorTest, PDFMagic) {
    std::vector<uint8_t> data = {0x25, 0x50, 0x44, 0x46, 0x2D};
    auto r = FileDetector::detect(data);
    EXPECT_EQ(r.mime, "application/pdf");
    EXPECT_FALSE(r.is_archive);
}

TEST_F(FileDetectorTest, PNGMagic) {
    std::vector<uint8_t> data = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0, 0};
    auto r = FileDetector::detect(data);
    EXPECT_EQ(r.mime, "image/png");
}

TEST_F(FileDetectorTest, ELFMagic) {
    std::vector<uint8_t> data = {0x7F, 'E', 'L', 'F', 0, 0, 0, 0};
    auto r = FileDetector::detect(data);
    EXPECT_EQ(r.mime, "application/x-elf");
}

TEST_F(FileDetectorTest, GzipMagic) {
    std::vector<uint8_t> data = {0x1F, 0x8B, 0x08, 0, 0, 0, 0};
    auto r = FileDetector::detect(data);
    EXPECT_EQ(r.mime, "application/gzip");
    EXPECT_TRUE(r.is_archive);
}

TEST_F(FileDetectorTest, CZIPMagic) {
    std::vector<uint8_t> data = {0x43, 0x5A, 0x49, 0x50, 0x01, 0x00, 0x00};
    auto r = FileDetector::detect(data);
    EXPECT_EQ(r.mime, "application/x-czip");
    EXPECT_TRUE(r.is_archive);
}

TEST_F(FileDetectorTest, CRARMagic) {
    std::vector<uint8_t> data = {0x43, 0x52, 0x41, 0x52, 0x01, 0x00, 0x00};
    auto r = FileDetector::detect(data);
    EXPECT_EQ(r.mime, "application/x-crar");
    EXPECT_TRUE(r.is_archive);
}

TEST_F(FileDetectorTest, PlainTextDetection) {
    auto data = from_str("Hello, world!\nThis is plain text.\n");
    auto r = FileDetector::detect(data);
    EXPECT_TRUE(r.is_text || r.mime == "text/plain");
}

TEST_F(FileDetectorTest, BinaryUnknownFormat) {
    std::vector<uint8_t> data = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05};
    auto r = FileDetector::detect(data);
    EXPECT_FALSE(r.is_text);
}

TEST_F(FileDetectorTest, EmptyInput) {
    auto r = FileDetector::detect({});
    EXPECT_EQ(r.mime, "application/octet-stream");
}

TEST_F(FileDetectorTest, NullByteDetectedAsBinary) {
    std::vector<uint8_t> data = {'H', 'e', 'l', 'l', 'o', 0x00, 'W', 'o', 'r', 'l', 'd'};
    auto r = FileDetector::detect(data);
    EXPECT_FALSE(r.is_text);
}

TEST_F(FileDetectorTest, DetectFilePDF) {
    // Создаём временный файл с PDF-магией
    auto tmp = fs::temp_directory_path() / "test_detect.pdf";
    {
        std::ofstream f(tmp, std::ios::binary);
        std::vector<uint8_t> hdr = {0x25, 0x50, 0x44, 0x46, 0x2D};
        f.write(reinterpret_cast<const char*>(hdr.data()), hdr.size());
    }
    auto r = FileDetector::detect_file(tmp.string());
    EXPECT_EQ(r.mime, "application/pdf");
    fs::remove(tmp);
}

TEST_F(FileDetectorTest, DetectFileMissingReturnsOctetStream) {
    auto r = FileDetector::detect_file("/nonexistent/path/file.bin");
    EXPECT_EQ(r.mime, "application/octet-stream");
}

// ═════════════════════════════════════════════════════════════
//  Archive (потоковый API)
// ═════════════════════════════════════════════════════════════

class ArchiveTest : public ::testing::Test {
protected:
    fs::path tmp_path;

    void SetUp() override { tmp_path = fs::temp_directory_path() / "fc_archive_test.czip"; }
    void TearDown() override {
        fs::remove(tmp_path);
        fs::remove(tmp_path.string() + ".ckpt");
    }
};

TEST_F(ArchiveTest, WriteAndReadHeader) {
    ArchiveHeader hdr;
    hdr.format = ArchiveFormat::CZIP;
    hdr.method = CompressMethod::HUFFMAN;
    hdr.win_size = 32768;
    hdr.orig_size = 1024;
    hdr.crc32 = 0xDEADBEEF;
    hdr.filename = "test.txt";

    {
        std::fstream f(tmp_path, std::ios::out | std::ios::binary | std::ios::trunc);
        auto off = begin_stream_archive(f, hdr);
        end_stream_archive(f, off, 1024, 0xDEADBEEF, 2);
    }

    auto [h2, payload] = read_archive(tmp_path.string());
    EXPECT_EQ(h2.format, ArchiveFormat::CZIP);
    EXPECT_EQ(h2.method, CompressMethod::HUFFMAN);
    EXPECT_EQ(h2.win_size, 32768u);
    EXPECT_EQ(h2.orig_size, 1024u);
    EXPECT_EQ(h2.crc32, 0xDEADBEEFu);
    EXPECT_EQ(h2.filename, "test.txt");
}

TEST_F(ArchiveTest, CRARFormat) {
    ArchiveHeader hdr;
    hdr.format = ArchiveFormat::CRAR;
    hdr.method = CompressMethod::PPM_LZSS_RNC;
    hdr.filename = "data.bin";

    {
        std::fstream f(tmp_path, std::ios::out | std::ios::binary | std::ios::trunc);
        auto off = begin_stream_archive(f, hdr);
        end_stream_archive(f, off, 0, 0, 0);
    }

    auto [h2, payload] = read_archive(tmp_path.string());
    EXPECT_EQ(h2.format, ArchiveFormat::CRAR);
    EXPECT_EQ(h2.method, CompressMethod::PPM_LZSS_RNC);
}

TEST_F(ArchiveTest, ReadInvalidMagicThrows) {
    {
        std::ofstream f(tmp_path, std::ios::binary);
        f.write("JUNK", 4);
        std::vector<uint8_t> pad(50, 0);
        f.write(reinterpret_cast<const char*>(pad.data()), pad.size());
    }
    EXPECT_THROW(read_archive(tmp_path.string()), std::runtime_error);
}

TEST_F(ArchiveTest, ReadNonexistentFileThrows) {
    EXPECT_THROW(read_archive("/nonexistent/archive.czip"), std::runtime_error);
}

TEST_F(ArchiveTest, CheckpointSaveLoad) {
    Checkpoint ckpt;
    ckpt.chunks_done = 5;
    ckpt.crc_accum = 0xAABBCCDD;
    ckpt.output_pos = 123456;
    ckpt.orig_size = 999999;

    save_checkpoint(tmp_path.string(), ckpt);
    EXPECT_TRUE(has_checkpoint(tmp_path.string()));

    auto loaded = load_checkpoint(tmp_path.string());
    EXPECT_EQ(loaded.chunks_done, 5u);
    EXPECT_EQ(loaded.crc_accum, 0xAABBCCDDu);
    EXPECT_EQ(loaded.output_pos, 123456u);
    EXPECT_EQ(loaded.orig_size, 999999u);

    remove_checkpoint(tmp_path.string());
    EXPECT_FALSE(has_checkpoint(tmp_path.string()));
}

TEST_F(ArchiveTest, LoadMissingCheckpointReturnsDefault) {
    auto ckpt = load_checkpoint(tmp_path.string());
    EXPECT_EQ(ckpt.chunks_done, 0u);
}

TEST_F(ArchiveTest, WriteStreamChunkAndRecover) {
    ArchiveHeader hdr;
    hdr.filename = "chunk_test.bin";
    hdr.orig_size = 100;

    std::vector<uint8_t> chunk_data = {1, 2, 3, 4, 5};

    {
        std::fstream f(tmp_path, std::ios::out | std::ios::binary | std::ios::trunc);
        auto off = begin_stream_archive(f, hdr);
        write_stream_chunk(f, chunk_data, 5);
        end_stream_archive(f, off, 100, 0xCAFEBABE, 1);
    }

    auto [h2, payload] = read_archive(tmp_path.string());
    EXPECT_EQ(h2.crc32, 0xCAFEBABEu);
    // payload начинается с n_chunks (4 байта) + chunk header (8 байт) + data
    ASSERT_GE(payload.size(), 4u + 8u + 5u);
    uint32_t n_chunks = (uint32_t(payload[0]) << 24) | (uint32_t(payload[1]) << 16) |
                        (uint32_t(payload[2]) << 8) | payload[3];
    EXPECT_EQ(n_chunks, 1u);
}

// ═════════════════════════════════════════════════════════════
//  BigEndian I/O helpers
// ═════════════════════════════════════════════════════════════

class BigEndianIOTest : public ::testing::Test {};

TEST_F(BigEndianIOTest, WriteReadU8) {
    std::ostringstream oss;
    write_u8(oss, 0xAB);
    auto s = oss.str();
    std::istringstream iss(s);
    EXPECT_EQ(read_u8(iss), 0xABu);
}

TEST_F(BigEndianIOTest, WriteReadU16) {
    std::ostringstream oss;
    write_u16(oss, 0x1234);
    auto s = oss.str();
    std::istringstream iss(s);
    EXPECT_EQ(read_u16(iss), 0x1234u);
}

TEST_F(BigEndianIOTest, WriteReadU32) {
    std::ostringstream oss;
    write_u32(oss, 0xDEADBEEF);
    auto s = oss.str();
    std::istringstream iss(s);
    EXPECT_EQ(read_u32(iss), 0xDEADBEEFu);
}

TEST_F(BigEndianIOTest, WriteReadU64) {
    std::ostringstream oss;
    write_u64(oss, 0x0102030405060708ULL);
    auto s = oss.str();
    std::istringstream iss(s);
    EXPECT_EQ(read_u64(iss), 0x0102030405060708ULL);
}

TEST_F(BigEndianIOTest, U32BigEndianByteOrder) {
    std::ostringstream oss;
    write_u32(oss, 0x01020304);
    auto s = oss.str();
    EXPECT_EQ(static_cast<uint8_t>(s[0]), 0x01u);
    EXPECT_EQ(static_cast<uint8_t>(s[1]), 0x02u);
    EXPECT_EQ(static_cast<uint8_t>(s[2]), 0x03u);
    EXPECT_EQ(static_cast<uint8_t>(s[3]), 0x04u);
}

// ═════════════════════════════════════════════════════════════
//  Комбинированные тесты (pipeline)
// ═════════════════════════════════════════════════════════════

class PipelineTest : public ::testing::Test {};

TEST_F(PipelineTest, LZ77ThenHuffmanRoundTrip) {
    auto data = from_str("abcabcabcabc hello world abcabc");
    LZ77Coder lz(4096);
    HuffmanCoder hc;
    auto dec = lz.decode(hc.decode(hc.encode(lz.encode(data))));
    EXPECT_EQ(dec, data);
}

TEST_F(PipelineTest, LZSSThenRNCRoundTrip) {
    auto data = from_str(
        "the quick brown fox jumps over the lazy dog "
        "the quick brown fox jumps over the lazy dog");
    LZSSCoder lzss;
    RNCCoder rnc;
    auto intermediate = lzss.encode(data);
    auto compressed = rnc.encode(intermediate);
    auto restored_mid = rnc.decode(compressed);
    auto restored = lzss.decode(restored_mid);
    EXPECT_EQ(restored, data);
}

TEST_F(PipelineTest, LZ77CompressesTextBetter) {
    std::string text;
    for (int i = 0; i < 100; ++i) text += "The quick brown fox jumps over the lazy dog. ";
    auto data = from_str(text);

    LZ77Coder lz;
    auto enc = lz.encode(data);
    double ratio = static_cast<double>(enc.size()) / data.size();
    EXPECT_LT(ratio, 0.5);  // LZ77 должен сжать длинный повторяющийся текст > 50%
}
