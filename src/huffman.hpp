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

// ═══════════════════════════════════════════════════════════════
//  Кодирование Хаффмана (классическое)
//
//  Формат сжатых данных:
//   [256 байт] длины кодов (0 = символ не встречается)
//   [8 байт]   исходный размер (big-endian uint64)
//   [1 байт]   число бит-заполнителей в последнем байте
//   [данные]   закодированные биты
// ═══════════════════════════════════════════════════════════════
class HuffmanCoder {
    // ── узел дерева ──────────────────────────────────────────
    struct Node {
        uint32_t freq;
        int      sym;         // ≥0 лист, -1 внутренний
        int      left  = -1;
        int      right = -1;
    };

    std::vector<Node>            nodes_;
    int                          root_   = -1;
    std::array<std::string, 256> codes_;  // коды для символов

    // ── построение дерева из таблицы частот ──────────────────
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

        // Граничный случай: один символ
        // Создаём внутренний узел с одним левым потомком (код = "0")
        if (pq.size() == 1) {
            auto [f, n] = pq.top(); pq.pop();
            int id = static_cast<int>(nodes_.size());
            nodes_.push_back({f, -1, n, -1});   // right=-1: никогда не посещается
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

    // ── рекурсивная генерация кодов ───────────────────────────
    void gen_codes(int nd, std::string& path) {
        if (nd < 0) return;
        if (nodes_[nd].sym >= 0) {
            codes_[nodes_[nd].sym] = path.empty() ? "0" : path;
            return;
        }
        path.push_back('0'); gen_codes(nodes_[nd].left, path);  path.pop_back();
        path.push_back('1'); gen_codes(nodes_[nd].right, path); path.pop_back();
    }

    // ── восстановление дерева из длин кодов ──────────────────
    void rebuild_from_lengths(const std::array<uint8_t,256>& lens) {
        // Канонические коды по RFC 1951
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

    void rebuild_tree_from_codes() {
        nodes_.clear();
        nodes_.reserve(512);   // предотвращаем перевыделение при росте дерева
        nodes_.push_back({0, -1});  // root = 0
        root_ = 0;

        for (int sym = 0; sym < 256; ++sym) {
            if (codes_[sym].empty()) continue;
            int cur = root_;
            for (char c : codes_[sym]) {
                bool right = (c == '1');
                // Читаем индекс потомка ДО возможного push_back
                int child = right ? nodes_[cur].right : nodes_[cur].left;
                if (child < 0) {
                    child = static_cast<int>(nodes_.size());
                    // Пишем индекс до push_back (не держим ссылку через него)
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
    // ── КОДИРОВАНИЕ ──────────────────────────────────────────
    std::vector<uint8_t> encode(const std::vector<uint8_t>& in) {
        std::array<uint32_t,256> freq{};
        for (uint8_t b : in) ++freq[b];

        build_tree(freq);
        std::string path;
        for (int i = 0; i < 256; ++i) codes_[i].clear();
        gen_codes(root_, path);

        // Переводим в канонические коды — те же длины, но стандартные паттерны.
        // Декодировщик делает то же самое, поэтому коды совпадут.
        std::array<uint8_t,256> lens{};
        for (int i = 0; i < 256; ++i) lens[i] = static_cast<uint8_t>(codes_[i].size());
        rebuild_from_lengths(lens);  // codes_ теперь канонические

        std::vector<uint8_t> out;
        out.reserve(in.size());

        // Таблица длин (одновременно — описание канонических кодов)
        for (int i = 0; i < 256; ++i)
            out.push_back(lens[i]);

        // Исходный размер (8 байт, big-endian)
        uint64_t orig = in.size();
        for (int i = 7; i >= 0; --i) out.push_back((orig >> (i*8)) & 0xFF);

        // Резервируем место под число бит-заполнителей
        size_t pad_pos = out.size();
        out.push_back(0);

        // Закодированные данные
        BitWriter bw(out);
        for (uint8_t b : in)
            for (char c : codes_[b])
                bw.write_bit(c == '1');
        int pad = bw.flush();
        out[pad_pos] = static_cast<uint8_t>(pad);
        return out;
    }

    // ── ДЕКОДИРОВАНИЕ ────────────────────────────────────────
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

        // Вычислим реальное число бит
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
