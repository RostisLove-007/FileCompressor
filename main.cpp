#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <filesystem>
#include "huffman.hpp"
#include "lz77.hpp"
#include "lzss.hpp"
#include "rnc.hpp"

namespace fs = std::filesystem;

static std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("Невозможно открыть файл: " + path);
    return {std::istreambuf_iterator<char>(f), {}};
}

static void write_file(const std::string& path,
                       const std::vector<uint8_t>& data) {
    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("Невозможно записать файл: " + path);
    f.write(reinterpret_cast<const char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
}

static std::string human_size(size_t sz) {
    if (sz < 1024)      return std::to_string(sz) + " Б";
    if (sz < 1048576)   return std::to_string(sz / 1024) + " КБ";
    return std::to_string(sz / 1048576) + " МБ";
}

// Идентификаторы методов (первый байт архива)
static constexpr uint8_t METHOD_HUFFMAN  = 0x01;
static constexpr uint8_t METHOD_LZ77     = 0x02;
static constexpr uint8_t METHOD_LZSS_RNC = 0x03;

static void print_usage(const char* prog) {
    std::cerr
        << "CZip v1.2 — File Compressor\n\n"
        << "Использование:\n"
        << "  " << prog << " -c [метод] <исходник> <архив>\n"
        << "  " << prog << " -d <архив> <результат>\n\n"
        << "Методы сжатия:\n"
        << "  (по умолчанию)  Huffman\n"
        << "  --lz77          LZ77 + Huffman\n"
        << "  --rnc           LZSS + RNC\n";
}

int main(int argc, char* argv[]) {
    if (argc < 4) { print_usage(argv[0]); return 1; }

    std::string mode   = argv[1];
    uint8_t     method = METHOD_HUFFMAN;
    int         farg   = 2;

    if (mode == "-c" && argc >= 5) {
        std::string flag = argv[2];
        if (flag == "--lz77")     { method = METHOD_LZ77;     farg = 3; }
        else if (flag == "--rnc") { method = METHOD_LZSS_RNC; farg = 3; }
    }

    if (farg + 1 >= argc) { print_usage(argv[0]); return 1; }
    std::string input  = argv[farg];
    std::string output = argv[farg + 1];

    try {
        if (mode == "-c") {
            auto data = read_file(input);
            const char* method_name =
                method == METHOD_LZ77     ? "LZ77 + Huffman" :
                method == METHOD_LZSS_RNC ? "LZSS + RNC"     : "Huffman";

            std::cout << "Сжатие: " << input << "\n"
                      << "  Исходный размер: " << human_size(data.size()) << "\n"
                      << "  Метод: " << method_name << "\n";

            std::vector<uint8_t> compressed;
            switch (method) {
                case METHOD_LZ77: {
                    LZ77Coder lz;
                    HuffmanCoder hc;
                    compressed = hc.encode(lz.encode(data));
                    break;
                }
                case METHOD_LZSS_RNC: {
                    LZSSCoder lzss;
                    RNCCoder  rnc;
                    compressed = rnc.encode(lzss.encode(data));
                    break;
                }
                default: {
                    HuffmanCoder hc;
                    compressed = hc.encode(data);
                }
            }

            // Первый байт — идентификатор метода
            compressed.insert(compressed.begin(), method);
            write_file(output, compressed);

            double ratio = 100.0 * compressed.size() / data.size();
            std::cout << "  Сжатый размер:   " << human_size(compressed.size())
                      << " (" << static_cast<int>(ratio) << "%)\n"
                      << "Готово.\n";

        } else if (mode == "-d") {
            auto data = read_file(input);
            if (data.empty()) throw std::runtime_error("Пустой архив");

            uint8_t mid = data[0];
            std::vector<uint8_t> payload(data.begin() + 1, data.end());
            std::vector<uint8_t> result;

            switch (mid) {
                case METHOD_LZ77: {
                    HuffmanCoder hc;
                    LZ77Coder lz;
                    result = lz.decode(hc.decode(payload));
                    break;
                }
                case METHOD_LZSS_RNC: {
                    RNCCoder  rnc;
                    LZSSCoder lzss;
                    result = lzss.decode(rnc.decode(payload));
                    break;
                }
                case METHOD_HUFFMAN: {
                    HuffmanCoder hc;
                    result = hc.decode(payload);
                    break;
                }
                default:
                    throw std::runtime_error(
                        "Неизвестный метод: " + std::to_string(mid));
            }

            write_file(output, result);
            std::cout << "Распаковка: " << input << "\n"
                      << "  Восстановлено: " << human_size(result.size()) << "\n"
                      << "Готово.\n";

        } else {
            print_usage(argv[0]);
            return 1;
        }

    } catch (const std::exception& e) {
        std::cerr << "Ошибка: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
