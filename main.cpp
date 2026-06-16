#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <filesystem>
#include "huffman.hpp"

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

int main(int argc, char* argv[]) {
    std::cout << "CZip v1.0 — Huffman compressor\n";

    if (argc != 4) {
        std::cerr << "Использование:\n"
                  << "  " << argv[0] << " -c <исходник> <архив>    # сжать\n"
                  << "  " << argv[0] << " -d <архив> <результат>   # распаковать\n";
        return 1;
    }

    std::string mode   = argv[1];
    std::string input  = argv[2];
    std::string output = argv[3];

    try {
        HuffmanCoder hc;

        if (mode == "-c") {
            std::cout << "Сжатие: " << input << " → " << output << "\n";
            auto data = read_file(input);
            std::cout << "  Исходный размер: " << human_size(data.size()) << "\n";

            auto compressed = hc.encode(data);
            write_file(output, compressed);

            double ratio = 100.0 * compressed.size() / data.size();
            std::cout << "  Сжатый размер:   " << human_size(compressed.size())
                      << " (" << static_cast<int>(ratio) << "%)\n";
            std::cout << "Готово.\n";

        } else if (mode == "-d") {
            std::cout << "Распаковка: " << input << " → " << output << "\n";
            auto data = read_file(input);
            auto result = hc.decode(data);
            write_file(output, result);
            std::cout << "  Восстановлено: " << human_size(result.size()) << "\n";
            std::cout << "Готово.\n";

        } else {
            std::cerr << "Неизвестный режим: " << mode << "\n";
            return 1;
        }

    } catch (const std::exception& e) {
        std::cerr << "Ошибка: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
