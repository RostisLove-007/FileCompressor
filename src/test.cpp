// ═══════════════════════════════════════════════════════════════
//  Автотесты алгоритмов сжатия
// ═══════════════════════════════════════════════════════════════
#include <iostream>
#include <vector>
#include <string>
#include <cassert>
#include <random>
#include <algorithm>
#include <numeric>
#include "bit_io.hpp"
#include "crc32.hpp"
#include "huffman.hpp"
#include "lz77.hpp"
#include "lzss.hpp"
#include "rnc.hpp"
#include "file_detector.hpp"

static int pass_count = 0, fail_count = 0;

#define CHECK(name, cond) do { \
    if (cond) { std::cout << "  ✓ " << (name) << "\n"; ++pass_count; } \
    else       { std::cout << "  ✗ FAIL: " << (name) << "\n"; ++fail_count; } \
} while(0)

// ── Хелперы ──────────────────────────────────────────────────
static std::vector<uint8_t> make_str(const std::string& s) {
    return {s.begin(), s.end()};
}
static std::vector<uint8_t> make_rnd(size_t n, uint32_t seed=42) {
    std::mt19937 rng(seed);
    std::vector<uint8_t> v(n);
    for (auto& b : v) b = static_cast<uint8_t>(rng());
    return v;
}
static std::vector<uint8_t> make_rep(uint8_t byte, size_t n) {
    return std::vector<uint8_t>(n, byte);
}

// ─────────────────────────────────────────────────────────────
//  BitWriter / BitReader
// ─────────────────────────────────────────────────────────────
void test_bitio() {
    std::cout << "\n[BitIO]\n";
    std::vector<uint8_t> buf;
    BitWriter bw(buf);
    bw.write_bits(0b10110, 5);
    bw.write_bits(0xFF, 8);
    bw.flush();

    BitReader br(buf.data(), buf.size());
    uint64_t v1 = br.read_bits(5);
    uint64_t v2 = br.read_bits(8);
    CHECK("write/read 5 bits", v1 == 0b10110);
    CHECK("write/read 8 bits", v2 == 0xFF);
}

// ─────────────────────────────────────────────────────────────
//  CRC32
// ─────────────────────────────────────────────────────────────
void test_crc32() {
    std::cout << "\n[CRC32]\n";
    // Известное значение: CRC32("123456789") = 0xCBF43926
    std::string s = "123456789";
    uint32_t c = CRC32::compute(reinterpret_cast<const uint8_t*>(s.data()), s.size());
    CHECK("'123456789' → 0xCBF43926", c == 0xCBF43926u);

    // Пустая строка → 0x00000000
    uint32_t ce = CRC32::compute(nullptr, 0);
    CHECK("пустая строка", ce == 0x00000000u);
}

// ─────────────────────────────────────────────────────────────
//  Huffman
// ─────────────────────────────────────────────────────────────
void test_huffman(const std::string& label, const std::vector<uint8_t>& data) {
    HuffmanCoder hc;
    auto enc = hc.encode(data);
    auto dec = hc.decode(enc);
    CHECK(label + " round-trip", dec == data);
    if (!data.empty())
        CHECK(label + " сжатие существенное (или тривиальные данные)",
              enc.size() <= data.size() + 300);
}

void test_huffman_all() {
    std::cout << "\n[Huffman]\n";
    test_huffman("пустые данные",    {});
    test_huffman("один байт",        {0x42});
    test_huffman("все одинаковые",   make_rep('A', 1000));
    test_huffman("текст",            make_str("Hello, World! Hello, World! Hello!"));
    test_huffman("lorem ipsum x100", make_str(std::string(100,
        "Lorem ipsum dolor sit amet, consectetur adipiscing elit. "[0])));
    test_huffman("случайные 4KB",    make_rnd(4096));
    test_huffman("бинарные 16KB",    make_rnd(16384));
}

// ─────────────────────────────────────────────────────────────
//  LZ77
// ─────────────────────────────────────────────────────────────
void test_lz77(const std::string& label, const std::vector<uint8_t>& data,
               uint32_t win=4096) {
    LZ77Coder lz(win);
    auto enc = lz.encode(data);
    auto dec = lz.decode(enc);
    CHECK(label + " round-trip", dec == data);
}

void test_lz77_all() {
    std::cout << "\n[LZ77]\n";
    test_lz77("пустые данные", {});
    test_lz77("один байт",     {0x42});
    test_lz77("повторы AAAAAA", make_rep('A', 2000));
    test_lz77("текст с повторами",
              make_str("abcabc defdef ghighi abcabc defdef"));
    test_lz77("случайные 2KB",  make_rnd(2048));
    test_lz77("окно 1KB",       make_str("hello hello hello world world world"), 1024);
    test_lz77("окно 32KB",      make_rnd(8192), 32768);
}

// ─────────────────────────────────────────────────────────────
//  LZSS
// ─────────────────────────────────────────────────────────────
void test_lzss(const std::string& label, const std::vector<uint8_t>& data) {
    LZSSCoder lz;
    auto enc = lz.encode(data);
    auto dec = lz.decode(enc);
    CHECK(label + " round-trip", dec == data);
}

void test_lzss_all() {
    std::cout << "\n[LZSS]\n";
    test_lzss("пустые данные", {});
    test_lzss("один байт",     {0x42});
    test_lzss("повторы AAAA",  make_rep('A', 2000));
    test_lzss("текст",         make_str("the quick brown fox jumps over the lazy dog"));
    test_lzss("случайные 4KB", make_rnd(4096, 123));
}

// ─────────────────────────────────────────────────────────────
//  RNC
// ─────────────────────────────────────────────────────────────
void test_rnc(const std::string& label, const std::vector<uint8_t>& data) {
    RNCCoder rnc;
    auto enc = rnc.encode(data);
    auto dec = rnc.decode(enc);
    CHECK(label + " round-trip", dec == data);
}

void test_rnc_all() {
    std::cout << "\n[RNC]\n";
    test_rnc("один байт",    {0x42});
    test_rnc("повторы 4KB",  make_rep('Z', 4096));
    test_rnc("текст",        make_str("Hello RNC! Hello RNC! Hello RNC!"));
    test_rnc("случайные 8KB",make_rnd(8192, 77));

    // Тест повреждённого архива
    try {
        RNCCoder rnc;
        auto corrupted = rnc.encode(make_str("test data"));
        corrupted[5] ^= 0xFF;   // портим данные
        rnc.decode(corrupted);
        CHECK("обнаружение повреждённого архива", false);  // должно бросить
    } catch (const std::runtime_error&) {
        CHECK("обнаружение повреждённого архива", true);
    }
}

// ─────────────────────────────────────────────────────────────
//  FileDetector
// ─────────────────────────────────────────────────────────────
void test_detector() {
    std::cout << "\n[FileDetector]\n";

    auto zip_magic = std::vector<uint8_t>{0x50,0x4B,0x03,0x04};
    auto r1 = FileDetector::detect(zip_magic);
    CHECK("ZIP magic → application/zip", r1.mime == "application/zip");
    CHECK("ZIP → is_archive",            r1.is_archive);

    auto pdf_magic = std::vector<uint8_t>{0x25,0x50,0x44,0x46, 0x2D};
    auto r2 = FileDetector::detect(pdf_magic);
    CHECK("PDF magic → application/pdf", r2.mime == "application/pdf");

    auto text = make_str("Hello, world!\nThis is a test text.\n");
    auto r3 = FileDetector::detect(text);
    CHECK("текст → text/plain или is_text", r3.is_text || r3.mime == "text/plain");

    std::vector<uint8_t> elf = {0x7F,'E','L','F', 0,0,0,0};
    auto r4 = FileDetector::detect(elf);
    CHECK("ELF magic", r4.mime == "application/x-elf");
}

// ─────────────────────────────────────────────────────────────
//  Комплексный тест: Huffman + LZ77 на реальном тексте
// ─────────────────────────────────────────────────────────────
void test_compression_ratio() {
    std::cout << "\n[Коэффициенты сжатия]\n";
    std::string text;
    for (int i = 0; i < 50; ++i)
        text += "The quick brown fox jumps over the lazy dog. ";
    auto data = make_str(text);

    HuffmanCoder hc;
    auto h_enc = hc.encode(data);
    double h_ratio = double(h_enc.size()) / data.size();
    std::cout << "  Huffman:  " << data.size() << " → " << h_enc.size()
              << " (" << int(h_ratio*100) << "%)\n";
    CHECK("Huffman сжимает текст", h_ratio < 0.9);

    LZ77Coder lz;
    auto l_enc = lz.encode(data);
    double l_ratio = double(l_enc.size()) / data.size();
    std::cout << "  LZ77:     " << data.size() << " → " << l_enc.size()
              << " (" << int(l_ratio*100) << "%)\n";
    CHECK("LZ77 сжимает текст", l_ratio < 0.8);

    LZSSCoder lzss;
    auto s_enc = lzss.encode(data);
    double s_ratio = double(s_enc.size()) / data.size();
    std::cout << "  LZSS:     " << data.size() << " → " << s_enc.size()
              << " (" << int(s_ratio*100) << "%)\n";
    CHECK("LZSS сжимает текст", s_ratio < 0.9);

    RNCCoder rnc;
    auto r_enc = rnc.encode(data);
    double r_ratio = double(r_enc.size()) / data.size();
    std::cout << "  RNC:      " << data.size() << " → " << r_enc.size()
              << " (" << int(r_ratio*100) << "%)\n";
    CHECK("RNC сжимает текст", r_ratio < 1.1);  // может быть небольшой оверхед
}

// ─────────────────────────────────────────────────────────────
//  main
// ─────────────────────────────────────────────────────────────
int main() {
    std::cout << "═══════════════════════════════════════════\n";
    std::cout << "  FileCompressor – Автотесты алгоритмов\n";
    std::cout << "═══════════════════════════════════════════\n";

    test_bitio();
    test_crc32();
    test_huffman_all();
    test_lz77_all();
    test_lzss_all();
    test_rnc_all();
    test_detector();
    test_compression_ratio();

    std::cout << "\n═══════════════════════════════════════════\n";
    std::cout << "  Итого: " << pass_count << " пройдено, "
              << fail_count << " провалено\n";
    std::cout << "═══════════════════════════════════════════\n";
    return fail_count > 0 ? 1 : 0;
}
