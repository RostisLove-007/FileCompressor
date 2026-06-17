#pragma once
#include <vector>
#include <cstdint>
#include <string>
#include <cstring>

// ═══════════════════════════════════════════════════════════════
//  FileDetector — определение типа файла по magic bytes
// ═══════════════════════════════════════════════════════════════
struct FileType {
    std::string description;
    bool        is_archive = false;   // уже сжатый формат
};

class FileDetector {
public:
    static FileType detect(const std::vector<uint8_t>& hdr) {
        auto has = [&](size_t off, const char* sig, size_t n) {
            return hdr.size() >= off + n &&
                   std::memcmp(hdr.data() + off, sig, n) == 0;
        };

        // Уже сжатые форматы
        if (has(0, "CZIP", 4) || has(0, "CRAR", 4))
            return {"Архив CZip",      true};
        if (has(0, "PK\x03\x04", 4))
            return {"ZIP-архив",       true};
        if (has(0, "Rar!", 4))
            return {"RAR-архив",       true};
        if (has(0, "\x1f\x8b", 2))
            return {"GZip-архив",      true};
        if (has(0, "7z\xbc\xaf\x27\x1c", 6))
            return {"7-Zip архив",     true};
        if (has(0, "BZh", 3))
            return {"BZip2-архив",     true};

        // Медиа (бессмысленно сжимать)
        if (has(0, "\xff\xd8\xff", 3))
            return {"JPEG-изображение", true};
        if (has(0, "\x89PNG\r\n\x1a\n", 8))
            return {"PNG-изображение",  true};
        if (has(0, "GIF8", 4))
            return {"GIF-изображение",  true};
        if (has(0, "ftyp", 4) || has(4, "ftyp", 4))
            return {"MP4/MOV-видео",    true};

        // Документы
        if (has(0, "%PDF", 4))
            return {"PDF-документ",    false};
        if (has(0, "\xd0\xcf\x11\xe0", 4))
            return {"Microsoft Office (старый формат)", false};
        if (has(0, "PK\x03\x04", 4))
            return {"Office Open XML (docx/xlsx)", true};

        // Исполняемые
        if (has(0, "\x7f" "ELF", 4))
            return {"ELF-бинарник",    false};
        if (has(0, "MZ", 2))
            return {"Windows PE",      false};

        // Текст — хорошо сжимается
        bool is_text = true;
        for (size_t i = 0; i < std::min(hdr.size(), size_t(16)); ++i)
            if (hdr[i] < 0x09 || (hdr[i] > 0x0d && hdr[i] < 0x20 && hdr[i] != 0x1b))
                { is_text = false; break; }
        if (is_text) return {"Текстовый файл", false};

        return {"Бинарный файл", false};
    }
};
