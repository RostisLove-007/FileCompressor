#pragma once
#include <vector>
#include <array>
#include <string>
#include <queue>
#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include "bit_io.hpp"

// ═══════════════════════════════════════════════════════════════
//  Huffman — канонический кодировщик/декодировщик
//
//  Формат закодированных данных:
//   [256 байт] длины кодов для символов 0..255 (0 = символ не используется)
//   [8 байт]   размер оригинала (big-endian uint64)
//   [1 байт]   кол-во padding-бит в последнем байте данных
//   [N байт]   закодированные биты
// ═══════════════════════════════════════════════════════════════
class HuffmanCoder {
    struct Node {
        uint32_t freq;
        int      sym;    // -1 для внутренних узлов
        int      left  = -1;
        int      right = -1;
    };

    std::vector<Node>           nodes_;
    std::array<std::string,256> codes_;
    int                         root_ = -1;

    // ── Построение дерева по частотам ────────────────────────
    void build_tree(const std::array<uint32_t,256>& freq) {
        nodes_.clear();
        // Минимальная куча по freq
        using P = std::pair<uint32_t,int>;
        std::priority_queue<P, std::vector<P>, std::greater<P>> pq;

        for (int i = 0; i < 256; ++i) {
            if (freq[i] == 0) continue;
            int id = static_cast<int>(nodes_.size());
            nodes_.push_back({freq[i], i});
            pq.push({freq[i], id});
        }

        // Особый случай: один уникальный символ
        if (pq.size() == 1) {
            int n  = static_cast<int>(nodes_.size());
            int ch = pq.top().second; pq.pop();
            nodes_.push_back({nodes_[ch].freq, -1, ch, -1});
            root_ = n;
            return;
        }

        while (pq.size() > 1) {
            auto [fa, a] = pq.top(); pq.pop();
            auto [fb, b] = pq.top(); pq.pop();
            int n = static_cast<int>(nodes_.size());
            nodes_.push_back({fa + fb, -1, a, b});
            pq.push({fa + fb, n});
        }
        root_ = pq.top().second;
    }

    // ── Обход дерева → codes_ (non-canonical) ─────────────────
    void gen_codes(int node, std::string& path) {
        if (node < 0) return;
        if (nodes_[node].sym >= 0) {
            codes_[nodes_[node].sym] = path.empty() ? "0" : path;
            return;
        }
        path.push_back('0'); gen_codes(nodes_[node].left,  path); path.pop_back();
        path.push_back('1'); gen_codes(nodes_[node].right, path); path.pop_back();
    }

    // ── Канонические коды из длин ─────────────────────────────
    void rebuild_from_lengths(const std::array<uint8_t,256>& lens) {
        // Сортируем символы: сначала по длине, потом по номеру
        std::vector<std::pair<uint8_t,int>> syms;
        for (int i = 0; i < 256; ++i)
            if (lens[i]) syms.push_back({lens[i], i});
        std::sort(syms.begin(), syms.end());

        uint32_t code = 0; uint8_t prev_len = 0;
        for (auto [len, sym] : syms) {
            code <<= (len - prev_len);
            std::string s;
            for (int b = len - 1; b >= 0; --b)
                s.push_back((code >> b) & 1 ? '1' : '0');
            codes_[sym] = s;
            ++code; prev_len = len;
        }
        rebuild_tree_from_codes();
    }

    // ── Дерево из канонических кодов (для декодирования) ──────
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
    // ── КОДИРОВАНИЕ ──────────────────────────────────────────
    std::vector<uint8_t> encode(const std::vector<uint8_t>& in) {
        std::array<uint32_t,256> freq{};
        for (uint8_t b : in) ++freq[b];

        build_tree(freq);
        std::string path;
        for (int i = 0; i < 256; ++i) codes_[i].clear();
        gen_codes(root_, path);

        // Канонические коды
        std::array<uint8_t,256> lens{};
        for (int i = 0; i < 256; ++i)
            lens[i] = static_cast<uint8_t>(codes_[i].size());
        rebuild_from_lengths(lens);

        std::vector<uint8_t> out;
        out.reserve(in.size());

        // Таблица длин
        for (int i = 0; i < 256; ++i) out.push_back(lens[i]);

        // Размер оригинала (8 байт BE)
        uint64_t sz = in.size();
        for (int i = 7; i >= 0; --i) out.push_back((sz >> (i*8)) & 0xFF);

        // Биты
        size_t pad_pos = out.size();
        out.push_back(0);   // padding placeholder
        BitWriter bw(out);
        for (uint8_t b : in)
            for (char c : codes_[b]) bw.write_bit(c == '1');
        int pad = bw.flush();
        out[pad_pos] = static_cast<uint8_t>(pad);

        return out;
    }

    // ── ДЕКОДИРОВАНИЕ ────────────────────────────────────────
    std::vector<uint8_t> decode(const std::vector<uint8_t>& in) {
        if (in.size() < 256 + 8 + 1)
            throw std::runtime_error("Huffman: слишком короткие данные");

        std::array<uint8_t,256> lens{};
        for (int i = 0; i < 256; ++i) lens[i] = in[i];

        uint64_t orig = 0;
        for (int i = 0; i < 8; ++i)
            orig = (orig << 8) | in[256 + i];

        int pad = in[264];

        for (int i = 0; i < 256; ++i) codes_[i].clear();
        rebuild_from_lengths(lens);

        std::vector<uint8_t> out;
        out.reserve(static_cast<size_t>(orig));

        size_t data_start = 265;
        size_t data_size  = in.size() - data_start;
        if (data_size == 0 && orig == 0) return out;

        const uint8_t* data = in.data() + data_start;
        size_t cur_b = 0; int cur_bit = 7;
        int cur = root_;

        size_t total_bits = data_size * 8 - static_cast<size_t>(pad);

        for (size_t bit_idx = 0; bit_idx < total_bits && out.size() < orig; ++bit_idx) {
            bool b = (data[cur_b] >> cur_bit) & 1;
            if (--cur_bit < 0) { ++cur_b; cur_bit = 7; }

            cur = b ? nodes_[cur].right : nodes_[cur].left;
            if (cur < 0) throw std::runtime_error("Huffman: повреждённое дерево");

            if (nodes_[cur].sym >= 0) {
                out.push_back(static_cast<uint8_t>(nodes_[cur].sym));
                cur = root_;
            }
        }

        if (out.size() != orig)
            throw std::runtime_error("Huffman: несоответствие размера при декодировании");
        return out;
    }
};
