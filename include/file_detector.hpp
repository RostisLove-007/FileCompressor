#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include <array>

/**
 * @file file_detector.hpp
 * @brief Автоматическое определение типа файла по магическим байтам (сигнатурам).
 */

/**
 * @brief Определяет тип содержимого по сигнатуре (магическим байтам) в начале файла.
 *
 * Поддерживает распознавание популярных архивов, изображений, аудио/видео,
 * документов и исполняемых файлов, а также собственных форматов CZIP/CRAR.
 * Если ни одна известная сигнатура не подошла, выполняется грубая проверка
 * на «текстовость» содержимого; в противном случае файл считается бинарным
 * неизвестного формата.
 */
class FileDetector {
    /**
     * @brief Описание одной магической сигнатуры файла.
     */
    struct Magic {
        std::vector<uint8_t> bytes;       ///< Байтовая последовательность сигнатуры.
        size_t               offset;      ///< Смещение от начала файла, с которого сигнатура должна начинаться.
        std::string          mime;        ///< MIME-тип, соответствующий сигнатуре.
        std::string          description; ///< Человекочитаемое описание типа файла (на русском языке).
        std::string          extension;   ///< Типичное расширение файла данного типа.
    };

    /**
     * @brief Возвращает таблицу известных сигнатур файлов.
     *
     * Таблица формируется один раз (статическая локальная переменная) и
     * используется методом detect() для последовательного сопоставления
     * с начальными байтами проверяемых данных.
     *
     * @return Константная ссылка на статический список известных сигнатур.
     */
    static const std::vector<Magic>& signatures() {
        static std::vector<Magic> sigs = {
            {{0x50,0x4B,0x03,0x04}, 0, "application/zip",  "ZIP-архив",          ".zip"},
            {{0x52,0x61,0x72,0x21,0x1A,0x07}, 0, "application/x-rar",  "RAR-архив", ".rar"},
            {{0x1F,0x8B},           0, "application/gzip", "Gzip-архив",         ".gz"},
            {{0x42,0x5A,0x68},      0, "application/x-bzip2","BZip2-архив",      ".bz2"},
            {{0xFD,0x37,0x7A,0x58,0x5A,0x00}, 0, "application/x-xz","XZ-архив", ".xz"},
            {{0x37,0x7A,0xBC,0xAF,0x27,0x1C}, 0, "application/x-7z",  "7-Zip архив",".7z"},

            {{0x43,0x5A,0x49,0x50}, 0, "application/x-czip","CZIP-архив (Huffman/LZ77)",".czip"},
            {{0x43,0x52,0x41,0x52}, 0, "application/x-crar","CRAR-архив (PPM/LZSS/RNC)", ".crar"},

            {{0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A}, 0, "image/png",  "PNG-изображение", ".png"},
            {{0xFF,0xD8,0xFF},      0, "image/jpeg", "JPEG-изображение",       ".jpg"},
            {{0x47,0x49,0x46,0x38}, 0, "image/gif",  "GIF-изображение",        ".gif"},
            {{0x42,0x4D},           0, "image/bmp",  "BMP-изображение",        ".bmp"},
            {{0x52,0x49,0x46,0x46}, 0, "image/webp", "WEBP-изображение",       ".webp"},

            {{0x49,0x44,0x33},      0, "audio/mpeg",  "MP3 (ID3)",             ".mp3"},
            {{0x66,0x74,0x79,0x70}, 4, "video/mp4",   "MP4-видео",             ".mp4"},
            {{0x00,0x00,0x00,0x18,0x66,0x74,0x79,0x70}, 0, "video/mp4","MP4",  ".mp4"},

            {{0x25,0x50,0x44,0x46}, 0, "application/pdf",  "PDF-документ",      ".pdf"},
            {{0xD0,0xCF,0x11,0xE0,0xA1,0xB1,0x1A,0xE1}, 0,
             "application/msword", "Microsoft Office (старый формат)",          ".doc"},
            {{0x3C,0x3F,0x78,0x6D,0x6C}, 0, "text/xml",   "XML-документ",      ".xml"},
            {{0x3C,0x68,0x74,0x6D,0x6C}, 0, "text/html",  "HTML-документ",     ".html"},

            {{0x7F,0x45,0x4C,0x46}, 0, "application/x-elf", "ELF-исполняемый",  ".elf"},
            {{0x4D,0x5A},           0, "application/x-msdownload","PE-исполняемый (Windows)",".exe"},

            {{0xEF,0xBB,0xBF},      0, "text/plain",  "UTF-8 текст (BOM)",     ".txt"},
        };
        return sigs;
    }

    /**
     * @brief Грубо оценивает, является ли содержимое текстовым.
     *
     * Проверяет первые до 512 байт данных: наличие нулевого байта считается
     * признаком бинарного содержимого, а доля управляющих символов вне
     * привычного для текста диапазона используется как мера «подозрительности».
     *
     * @param data Проверяемые данные.
     * @return @c true, если доля подозрительных байт менее 5% и не встретился нулевой байт.
     */
    static bool looks_like_text(const std::vector<uint8_t>& data) {
        size_t check = std::min(data.size(), size_t(512));
        size_t non_text = 0;
        for (size_t i = 0; i < check; ++i) {
            uint8_t b = data[i];
            if (b == 0) return false;
            if (b < 0x09 && b != 0x00) ++non_text;
            else if (b > 0x0D && b < 0x20) ++non_text;
        }
        return non_text < check / 20;
    }

public:
    /**
     * @brief Результат определения типа файла.
     */
    struct Result {
        std::string mime;             ///< Определённый MIME-тип содержимого.
        std::string description;      ///< Человекочитаемое описание типа (на русском языке).
        std::string extension;        ///< Типичное расширение для данного типа.
        bool        is_text    = false;  ///< @c true, если содержимое распознано как текстовое.
        bool        is_archive = false;  ///< @c true, если содержимое распознано как архив.
    };

    /**
     * @brief Определяет тип данных по их содержимому (магическим байтам).
     *
     * Последовательно сравнивает начало данных с таблицей известных
     * сигнатур signatures(); если совпадение не найдено, выполняет
     * проверку looks_like_text() как запасной вариант классификации.
     *
     * @param data Буфер данных (или их начальный фрагмент) для анализа.
     * @return Результат определения типа содержимого.
     */
    static Result detect(const std::vector<uint8_t>& data) {
        if (data.empty())
            return {"application/octet-stream", "Пустой файл", ".bin"};

        for (const auto& sig : signatures()) {
            if (data.size() < sig.offset + sig.bytes.size()) continue;
            bool match = true;
            for (size_t i = 0; i < sig.bytes.size(); ++i) {
                if (data[sig.offset + i] != sig.bytes[i]) { match = false; break; }
            }
            if (match) {
                bool is_arc = (sig.mime.find("zip") != std::string::npos ||
                               sig.mime.find("rar") != std::string::npos ||
                               sig.mime.find("gzip")!= std::string::npos ||
                               sig.mime.find("bzip2")!= std::string::npos||
                               sig.mime.find("czip")!= std::string::npos ||
                               sig.mime.find("crar")!= std::string::npos ||
                               sig.mime.find("7z")  != std::string::npos ||
                               sig.mime.find("xz")  != std::string::npos);
                return {sig.mime, sig.description, sig.extension, false, is_arc};
            }
        }

        if (looks_like_text(data))
            return {"text/plain", "Текстовый файл", ".txt", true, false};

        return {"application/octet-stream", "Бинарный файл (неизвестный формат)", ".bin"};
    }

    /**
     * @brief Определяет тип файла на диске, читая лишь его начальный фрагмент.
     *
     * Открывает файл, считывает не более 512 байт заголовка и передаёт их
     * в detect(). Используется, когда загружать весь файл в память
     * избыточно.
     *
     * @param path Путь к файлу на диске.
     * @return Результат определения типа содержимого; в случае ошибки
     *         открытия файла возвращается признак ошибки открытия.
     */
    static Result detect_file(const std::string& path) {
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) return {"application/octet-stream", "Ошибка открытия файла", ".bin"};

        std::vector<uint8_t> hdr(512);
        size_t n = fread(hdr.data(), 1, 512, f);
        fclose(f);
        hdr.resize(n);
        return detect(hdr);
    }
};
