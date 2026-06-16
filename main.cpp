#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <filesystem>
#include "huffman.hpp"
#include "lz77.hpp"

namespace fs = std::filesystem;

static std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("Невозможно открыть файл: " + path);
    return {std::istreambuf_iterator<char>(f), {}};
}

static void write_file(const std::string& path, const std::vector<uint8_t>& data) {
    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("Невозможно записать файл: " + path);
    f.write(reinterpret_cast<const char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
}

static std::string human_size(size_t sz) {
    if (sz < 1024)       return std::to_string(sz) + " Б";
    if (sz < 1024*1024)  return std::to_string(sz/1024) + " КБ";
    return std::to_string(sz/(1024*1024)) + " МБ";
}

// Первый байт архива — идентификатор метода
static constexpr uint8_t METHOD_HUFFMAN = 0x01;
static constexpr uint8_t METHOD_LZ77    = 0x02;

static void print_usage(const char* prog) {
    std::cerr
        << "CZip v1.1 — File Compressor\n\n"
        << "Использование:\n"
        << "  " << prog << " -c [--lz77] <исходник> <архив>   # сжать\n"
        << "  " << prog << " -d <архив> <результат>            # распаковать\n\n"
        << "Методы:\n"
        << "  (по умолчанию)  Huffman\n"
        << "  --lz77          LZ77 + Huffman\n";
}

int main(int argc, char* argv[]) {
    if (argc < 4) { print_usage(argv[0]); return 1; }

    std::string mode  = argv[1];
    bool use_lz77 = false;
    int  file_arg = 2;

    if (mode == "-c" && std::string(argv[2]) == "--lz77") {
        use_lz77 = true;
        file_arg = 3;
        if (argc < 5) { print_usage(argv[0]); return 1; }
    }

    std::string input  = argv[file_arg];
    std::string output = argv[file_arg + 1];

    try {
        if (mode == "-c") {
            auto data = read_file(input);
            std::cout << "Сжатие: " << input << "\n"
                      << "  Исходный размер: " << human_size(data.size()) << "\n"
                      << "  Метод: " << (use_lz77 ? "LZ77 + Huffman" : "Huffman") << "\n";

            std::vector<uint8_t> compressed;
            uint8_t method_id;

            if (use_lz77) {
                LZ77Coder lz;
                auto lz_out = lz.encode(data);
                HuffmanCoder hc;
                compressed = hc.encode(lz_out);
                method_id  = METHOD_LZ77;
            } else {
                HuffmanCoder hc;
                compressed = hc.encode(data);
                method_id  = METHOD_HUFFMAN;
            }

            // Добавляем 1 байт метода в начало
            compressed.insert(compressed.begin(), method_id);
            write_file(output, compressed);

            double ratio = 100.0 * compressed.size() / data.size();
            std::cout << "  Сжатый размер:   " << human_size(compressed.size())
                      << " (" << static_cast<int>(ratio) << "%)\n"
                      << "Готово.\n";

        } else if (mode == "-d") {
            auto data = read_file(input);
            if (data.empty()) throw std::runtime_error("Пустой архив");

            uint8_t method_id = data[0];
            std::vector<uint8_t> payload(data.begin() + 1, data.end());

            std::vector<uint8_t> result;
            if (method_id == METHOD_LZ77) {
                HuffmanCoder hc;
                auto huff_out = hc.decode(payload);
                LZ77Coder lz;
                result = lz.decode(huff_out);
            } else if (method_id == METHOD_HUFFMAN) {
                HuffmanCoder hc;
                result = hc.decode(payload);
            } else {
                throw std::runtime_error("Неизвестный метод: " +
                                         std::to_string(method_id));
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
