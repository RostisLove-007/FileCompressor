# CZip — File Compressor

Консольная утилита сжатия файлов на C++17.

## Сборка

```bash
make
```

## Использование

```bash
# Сжать
./filecompressor -c input.txt output.huf

# Распаковать
./filecompressor -d input.huf output.txt
```

## Алгоритмы

| Версия | Алгоритм |
|--------|----------|
| v1.0   | Huffman  |
