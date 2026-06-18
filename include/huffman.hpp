#pragma once
#include <vector>
#include <array>
#include <queue>
#include <string>
#include <cstdint>
#include <stdexcept>
#include <algorithm>
#include <numeric>
#include "bit_io.hpp"

/**
 * @file huffman.hpp
 * @brief Классическое кодирование Хаффмана с каноническими кодами.
 *
 * Формат сжатых данных:
 *  - 256 байт — длины кодов для каждого байтового значения (0 — символ не встречается);
 *  - 8 байт   — исходный размер данных (big-endian uint64);
 *  - 1 байт   — число бит-заполнителей в последнем байте закодированных данных;
 *  - далее    — закодированный битовый поток.
 */

/**
 * @brief Кодек Хаффмана: построение дерева, генерация канонических кодов,
 *        кодирование и декодирование произвольных байтовых данных.
 *
 * Кодек использует канонические коды Хаффмана (по аналогии с RFC 1951),
 * что позволяет передавать только таблицу длин кодов, а не сами коды:
 * кодер и декодер восстанавливают одинаковые битовые последовательности
 * по одному и тому же алгоритму на основе длин.
 */
class HuffmanCoder {
    /**
     * @brief Узел двоичного дерева Хаффмана.
     */
    struct Node {
        uint32_t freq;        ///< Суммарная частота поддерева (для листа — частота символа).
        int      sym;         ///< Код символа (0..255) для листа, -1 для внутреннего узла.
        int      left  = -1;  ///< Индекс левого потомка в nodes_, либо -1.
        int      right = -1;  ///< Индекс правого потомка в nodes_, либо -1.
    };

    /// Узлы дерева Хаффмана, хранящиеся в плоском массиве (индексация вместо указателей).
    std::vector<Node>            nodes_;
    /// Индекс корневого узла в nodes_, либо -1, если дерево не построено.
    int                          root_   = -1;
    /// Битовые коды (в виде строк '0'/'1') для каждого из 256 возможных символов.
    std::array<std::string, 256> codes_;

    /**
     * @brief Строит дерево Хаффмана по таблице частот символов.
     *
     * Использует очередь с приоритетом для последовательного объединения
     * двух узлов с наименьшей суммарной частотой. Обрабатывает граничный
     * случай, когда во входных данных встречается только один уникальный символ.
     *
     * @param freq Таблица частот встречаемости каждого из 256 байтовых значений.
     */
    void build_tree(const std::array<uint32_t,256>& freq) {
        nodes_.clear();
        using P = std::pair<uint32_t,int>;
        std::priority_queue<P, std::vector<P>, std::greater<P>> pq;

        for (int i = 0; i < 256; ++i) {
            if (freq[i] > 0) {
                int id = static_cast<int>(nodes_.size());
                nodes_.push_back({freq[i], i});
                pq.push({freq[i], id});
            }
        }

        if (pq.empty()) { root_ = -1; return; }

        if (pq.size() == 1) {
            auto [f, n] = pq.top(); pq.pop();
            int id = static_cast<int>(nodes_.size());
            nodes_.push_back({f, -1, n, -1});
            root_ = id; return;
        }

        while (pq.size() > 1) {
            auto [f1, n1] = pq.top(); pq.pop();
            auto [f2, n2] = pq.top(); pq.pop();
            int id = static_cast<int>(nodes_.size());
            nodes_.push_back({f1 + f2, -1, n1, n2});
            pq.push({f1 + f2, id});
        }
        root_ = pq.top().second;
    }

    /**
     * @brief Рекурсивно обходит дерево Хаффмана и заполняет коды символов.
     * @param nd   Индекс текущего узла дерева (начинать следует с корня).
     * @param path Накопленный путь от корня в виде строки из '0' и '1'
     *             (модифицируется и восстанавливается при обходе).
     */
    void gen_codes(int nd, std::string& path) {
        if (nd < 0) return;
        if (nodes_[nd].sym >= 0) {
            codes_[nodes_[nd].sym] = path.empty() ? "0" : path;
            return;
        }
        path.push_back('0'); gen_codes(nodes_[nd].left, path);  path.pop_back();
        path.push_back('1'); gen_codes(nodes_[nd].right, path); path.pop_back();
    }

    /**
     * @brief Восстанавливает канонические коды Хаффмана по таблице длин кодов.
     *
     * Реализует стандартный алгоритм построения канонических кодов
     * (аналогичный описанному в RFC 1951): для каждой длины кода вычисляется
     * начальное числовое значение, после чего символам одинаковой длины
     * присваиваются последовательные коды в порядке возрастания значения символа.
     *
     * @param lens Длины кодов (в битах) для каждого из 256 символов; 0 — символ не используется.
     */
    void rebuild_from_lengths(const std::array<uint8_t,256>& lens) {
        uint8_t max_len = *std::max_element(lens.begin(), lens.end());
        if (max_len == 0) { root_ = -1; return; }

        std::vector<int> bl_count(max_len + 1, 0);
        for (int i = 0; i < 256; ++i)
            if (lens[i] > 0) ++bl_count[lens[i]];

        std::vector<uint32_t> next_code(max_len + 2, 0);
        uint32_t code = 0;
        for (int b = 1; b <= max_len; ++b) {
            code = (code + bl_count[b-1]) << 1;
            next_code[b] = code;
        }

        for (int i = 0; i < 256; ++i) codes_[i].clear();
        for (int i = 0; i < 256; ++i) {
            if (lens[i] == 0) continue;
            uint32_t c = next_code[lens[i]]++;
            codes_[i].resize(lens[i]);
            for (int j = lens[i]-1; j >= 0; --j) {
                codes_[i][j] = (char)('0' + (c & 1));
                c >>= 1;
            }
        }
        rebuild_tree_from_codes();
    }

    /**
     * @brief Перестраивает дерево разбора (nodes_) на основе текущих кодов в codes_.
     *
     * Для каждого символа с непустым кодом прокладывает путь от корня,
     * создавая недостающие промежуточные узлы, и помечает конечный узел
     * соответствующим символом. Используется декодером для навигации
     * по битовому потоку.
     */
    void rebuild_tree_from_codes() {
        nodes_.clear();
        nodes_.reserve(512);
        nodes_.push_back({0, -1});
        root_ = 0;

        for (int sym = 0; sym < 256; ++sym) {
            if (codes_[sym].empty()) continue;
            int cur = root_;
            for (char c : codes_[sym]) {
                bool right = (c == '1');
                int child = right ? nodes_[cur].right : nodes_[cur].left;
                if (child < 0) {
                    child = static_cast<int>(nodes_.size());
                    if (right) nodes_[cur].right = child;
                    else       nodes_[cur].left  = child;
                    nodes_.push_back({0, -1});
                }
                cur = child;
            }
            nodes_[cur].sym = sym;
        }
    }

public:
    /**
     * @brief Сжимает произвольные байтовые данные кодированием Хаффмана.
     *
     * Подсчитывает частоты байтов, строит дерево Хаффмана, переводит коды
     * в канонический вид, после чего записывает таблицу длин, исходный
     * размер, число бит-заполнителей и сам закодированный битовый поток.
     *
     * @param in Исходные данные для сжатия.
     * @return Сжатый буфер в формате, описанном в @ref huffman.hpp.
     */
    std::vector<uint8_t> encode(const std::vector<uint8_t>& in) {
        std::array<uint32_t,256> freq{};
        for (uint8_t b : in) ++freq[b];

        build_tree(freq);
        std::string path;
        for (int i = 0; i < 256; ++i) codes_[i].clear();
        gen_codes(root_, path);

        std::array<uint8_t,256> lens{};
        for (int i = 0; i < 256; ++i) lens[i] = static_cast<uint8_t>(codes_[i].size());
        rebuild_from_lengths(lens);

        std::vector<uint8_t> out;
        out.reserve(in.size());

        for (int i = 0; i < 256; ++i)
            out.push_back(lens[i]);

        uint64_t orig = in.size();
        for (int i = 7; i >= 0; --i) out.push_back((orig >> (i*8)) & 0xFF);

        size_t pad_pos = out.size();
        out.push_back(0);

        BitWriter bw(out);
        for (uint8_t b : in)
            for (char c : codes_[b])
                bw.write_bit(c == '1');
        int pad = bw.flush();
        out[pad_pos] = static_cast<uint8_t>(pad);
        return out;
    }

    /**
     * @brief Восстанавливает исходные данные из буфера, сжатого encode().
     *
     * Читает таблицу длин кодов, исходный размер и число бит-заполнителей,
     * восстанавливает дерево по каноническим кодам и декодирует битовый
     * поток, проходя по дереву от корня для каждого символа.
     *
     * @param in Сжатый буфер в формате, описанном в @ref huffman.hpp.
     * @return Восстановленные исходные данные.
     * @throws std::runtime_error если заголовок повреждён, размер данных
     *         не совпадает с ожидаемым, или дерево повреждено.
     */
    std::vector<uint8_t> decode(const std::vector<uint8_t>& in) {
        if (in.size() < 256 + 9)
            throw std::runtime_error("Huffman: недостаточно данных в заголовке");

        std::array<uint8_t,256> lens{};
        for (int i = 0; i < 256; ++i) lens[i] = in[i];

        uint64_t orig = 0;
        for (int i = 0; i < 8; ++i) orig = (orig << 8) | in[256 + i];

        uint8_t pad = in[264];

        rebuild_from_lengths(lens);
        if (root_ < 0) return {};

        std::vector<uint8_t> out;
        out.reserve(static_cast<size_t>(orig));

        const uint8_t* data   = in.data() + 265;
        size_t         nbytes = in.size() - 265;

        if (nbytes == 0 && orig == 0) return out;
        size_t total_bits = nbytes * 8;
        if (total_bits < static_cast<size_t>(pad))
            throw std::runtime_error("Huffman: некорректный заголовок (pad > bits)");
        total_bits -= pad;

        int cur_bit  = 7;
        size_t cur_b = 0;
        int nd = root_;

        for (size_t bit = 0; bit < total_bits && out.size() < orig; ++bit) {
            bool go_right = (data[cur_b] >> cur_bit) & 1;
            if (--cur_bit < 0) { cur_bit = 7; ++cur_b; }

            nd = go_right ? nodes_[nd].right : nodes_[nd].left;
            if (nd < 0) throw std::runtime_error("Huffman: повреждённое дерево");

            if (nodes_[nd].sym >= 0) {
                out.push_back(static_cast<uint8_t>(nodes_[nd].sym));
                nd = root_;
            }
        }

        if (out.size() != orig)
            throw std::runtime_error("Huffman: несоответствие размера при декодировании");
        return out;
    }
};
