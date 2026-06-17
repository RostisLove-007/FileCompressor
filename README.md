# CZip v2.0 — File Compressor

Архиватор файлов на C++17 с GTK3-интерфейсом.

## Возможности

- Три алгоритма: **Huffman**, **LZ77 + Huffman**, **LZSS + RNC**
- Два формата: **CZIP** (ZIP-стиль) и **CRAR** (RAR-стиль)
- Чанковая обработка (512 КБ/чанк) — файлы любого размера
- Проверка целостности CRC32 при распаковке
- Продолжение прерванного сжатия (`.ckpt`-файлы)
- Автоопределение типа файла по magic bytes

## Сборка

```bash
# macOS
brew install gtk+3
make

# Linux
sudo apt install libgtk-3-dev
make
```

## История веток

```
main  v1.0 ──────────────────────────────────────── v2.0
              \                                    /
dev            ●──lz77──lzss-rnc──archive──gui──chunked──resume──polish──●
```

## Алгоритмы

| Метод       | Формат | Описание                              |
|-------------|--------|---------------------------------------|
| Huffman     | CZIP   | Канонический Huffman, без зависимостей |
| LZ77+Huffman| CZIP   | Скользящее окно + энтропийное сжатие  |
| LZSS+RNC    | CRAR   | LZSS с чанками 4KB + CRC16           |

## Формат архива

```
[4B]  магия CZIP / CRAR
[1B]  версия 0x01
[1B]  метод
[1B]  флаги
[4B]  win_size (BE)
[8B]  orig_size (BE)
[4B]  CRC32 (BE)
[2B]  filename_len
[N B] filename
[4B]  n_chunks
[чанки: 4B csz + 4B orig_sz + data]
```
