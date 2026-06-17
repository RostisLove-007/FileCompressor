// ══════════════════════════════════════════════════════════════
//  FileCompressor – графический интерфейс (GTK3)
//  Автор: C++17 / GTK3
// ══════════════════════════════════════════════════════════════
#include <gtk/gtk.h>
#include <thread>
#include <atomic>
#include <string>
#include <memory>
#include <filesystem>
#include "compressor.hpp"
#include "file_detector.hpp"

namespace fs = std::filesystem;

// ─────────────────────────────────────────────────────────────
//  Глобальное состояние приложения
// ─────────────────────────────────────────────────────────────
struct AppState {
    // Виджеты
    GtkWidget* window        = nullptr;
    GtkWidget* entry_input   = nullptr;
    GtkWidget* entry_output  = nullptr;
    GtkWidget* radio_zip     = nullptr;
    GtkWidget* radio_rar     = nullptr;
    GtkWidget* radio_huff    = nullptr;
    GtkWidget* radio_lz77    = nullptr;
    GtkWidget* spin_window   = nullptr;
    GtkWidget* check_resume  = nullptr;
    GtkWidget* radio_compress= nullptr;
    GtkWidget* radio_decomp  = nullptr;
    GtkWidget* btn_start     = nullptr;
    GtkWidget* btn_cancel    = nullptr;
    GtkWidget* progress_bar  = nullptr;
    GtkWidget* label_status  = nullptr;
    GtkWidget* label_ftype   = nullptr;
    GtkWidget* frame_lz77    = nullptr;
    GtkWidget* frame_format  = nullptr;   // секция «Формат архива»
    GtkWidget* frame_method  = nullptr;   // секция «Метод сжатия»
    GtkWidget* frame_opts    = nullptr;   // секция «Опции» (resume)

    // Рабочее состояние
    std::unique_ptr<Compressor> compressor;
    std::thread                 worker;
    std::atomic<bool>           running{false};

    // Данные для обновления UI из рабочего потока
    std::atomic<double>         progress_val{0.0};
    std::string                 status_text;
    std::atomic<bool>           finished{false};
    std::atomic<bool>           had_error{false};
    std::string                 error_text;
    guint                       timer_id = 0;
};

static AppState app;

// ─────────────────────────────────────────────────────────────
//  Вспомогательные функции GTK
// ─────────────────────────────────────────────────────────────
static void set_controls_sensitive(bool enabled) {
    gtk_widget_set_sensitive(app.entry_input,    enabled);
    gtk_widget_set_sensitive(app.entry_output,   enabled);
    gtk_widget_set_sensitive(app.radio_zip,      enabled);
    gtk_widget_set_sensitive(app.radio_rar,      enabled);
    gtk_widget_set_sensitive(app.radio_huff,     enabled);
    gtk_widget_set_sensitive(app.radio_lz77,     enabled);
    gtk_widget_set_sensitive(app.spin_window,    enabled);
    gtk_widget_set_sensitive(app.check_resume,   enabled);
    gtk_widget_set_sensitive(app.radio_compress, enabled);
    gtk_widget_set_sensitive(app.radio_decomp,   enabled);
    gtk_widget_set_sensitive(app.btn_start,      enabled);
    gtk_widget_set_sensitive(app.btn_cancel,    !enabled);
}

static void show_error(const std::string& msg) {
    GtkWidget* dlg = gtk_message_dialog_new(
        GTK_WINDOW(app.window),
        GTK_DIALOG_MODAL,
        GTK_MESSAGE_ERROR,
        GTK_BUTTONS_OK,
        "%s", msg.c_str());
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
}

static void show_info(const std::string& msg) {
    GtkWidget* dlg = gtk_message_dialog_new(
        GTK_WINDOW(app.window),
        GTK_DIALOG_MODAL,
        GTK_MESSAGE_INFO,
        GTK_BUTTONS_OK,
        "%s", msg.c_str());
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
}

// ─────────────────────────────────────────────────────────────
//  Таймер: обновляет UI из главного потока
// ─────────────────────────────────────────────────────────────
static gboolean update_ui(gpointer) {
    if (!app.running && !app.finished) return G_SOURCE_CONTINUE;

    // Обновляем прогресс-бар и статус
    double frac = app.progress_val.load();
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app.progress_bar), frac);
    // Процент прямо на полоске
    char pct[16];
    if (frac <= 0.0)      pct[0] = '\0';
    else if (frac >= 1.0) snprintf(pct, sizeof(pct), "100%%");
    else                  snprintf(pct, sizeof(pct), "%.0f%%", frac * 100.0);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(app.progress_bar), pct);
    gtk_label_set_text(GTK_LABEL(app.label_status), app.status_text.c_str());

    if (app.finished.exchange(false)) {
        app.running = false;
        set_controls_sensitive(true);
        g_source_remove(app.timer_id);
        app.timer_id = 0;

        if (app.had_error) {
            gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app.progress_bar), 0.0);
            // status_text уже содержит понятное сообщение об ошибке,
            // установленное в рабочем потоке — показываем его, а не e.what()
            show_error(app.status_text);
        } else {
            show_info("Операция успешно завершена!");
        }

        if (app.worker.joinable()) app.worker.join();
    }
    return G_SOURCE_CONTINUE;
}

// ─────────────────────────────────────────────────────────────
//  Диалог выбора файла
// ─────────────────────────────────────────────────────────────
static std::string browse_file(bool save_mode, const std::string& title) {
    GtkWidget* dlg = gtk_file_chooser_dialog_new(
        title.c_str(),
        GTK_WINDOW(app.window),
        save_mode ? GTK_FILE_CHOOSER_ACTION_SAVE
                  : GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Отмена", GTK_RESPONSE_CANCEL,
        save_mode ? "_Сохранить" : "_Открыть", GTK_RESPONSE_ACCEPT,
        nullptr);

    if (save_mode)
        gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dlg), TRUE);

    std::string result;
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char* fn = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        result = fn;
        g_free(fn);
    }
    gtk_widget_destroy(dlg);
    return result;
}

// ─────────────────────────────────────────────────────────────
//  Единая функция обновления состояния виджетов.
//  Вызывается при любом переключении действия / формата / метода.
// ─────────────────────────────────────────────────────────────
static void update_ui_state(GtkToggleButton* = nullptr, gpointer = nullptr) {
    bool decompress = gtk_toggle_button_get_active(
                          GTK_TOGGLE_BUTTON(app.radio_decomp));
    bool is_zip     = gtk_toggle_button_get_active(
                          GTK_TOGGLE_BUTTON(app.radio_zip));
    bool is_lz77    = gtk_toggle_button_get_active(
                          GTK_TOGGLE_BUTTON(app.radio_lz77));

    // При переключении режима очищаем пути — они теряют смысл
    static bool last_decompress = false;
    if (decompress != last_decompress) {
        last_decompress = decompress;
        gtk_entry_set_text(GTK_ENTRY(app.entry_input),  "");
        gtk_entry_set_text(GTK_ENTRY(app.entry_output), "");
    }

    // При восстановлении — скрываем формат, метод и опции сжатия
    gtk_widget_set_visible(app.frame_format, !decompress);
    gtk_widget_set_visible(app.frame_opts,   !decompress);

    // Метод скрываем если: восстановление ИЛИ выбран CRAR (метод фиксирован)
    gtk_widget_set_visible(app.frame_method, !decompress && is_zip);

    // Окно LZ77 только для: сжатие + CZIP + LZ77
    gtk_widget_set_visible(app.frame_lz77, !decompress && is_zip && is_lz77);
}

// ─────────────────────────────────────────────────────────────
//  Автозаполнение выходного файла
// ─────────────────────────────────────────────────────────────
static void on_input_changed(GtkEditable* ed, gpointer) {
    const gchar* inp = gtk_entry_get_text(GTK_ENTRY(app.entry_input));
    if (!inp || inp[0] == '\0') return;

    std::string out_path = inp;
    bool is_decompress =
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app.radio_decomp));

    if (is_decompress) {
        // Убираем расширение .czip/.crar
        if (out_path.size() > 5 &&
            (out_path.substr(out_path.size()-5) == ".czip" ||
             out_path.substr(out_path.size()-5) == ".crar"))
            out_path = out_path.substr(0, out_path.size()-5);
        else
            out_path += "_restored";
    } else {
        bool is_zip = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app.radio_zip));
        out_path += is_zip ? ".czip" : ".crar";
    }

    // Автоопределение типа файла
    auto ftype = FileDetector::detect_file(inp);
    gtk_label_set_text(GTK_LABEL(app.label_ftype),
                       ("Тип: " + ftype.description).c_str());

    gtk_entry_set_text(GTK_ENTRY(app.entry_output), out_path.c_str());
}

// ─────────────────────────────────────────────────────────────
//  Кнопка «Старт»
// ─────────────────────────────────────────────────────────────
static void on_btn_start(GtkButton*, gpointer) {
    std::string input  = gtk_entry_get_text(GTK_ENTRY(app.entry_input));
    std::string output = gtk_entry_get_text(GTK_ENTRY(app.entry_output));

    if (input.empty() || output.empty()) {
        show_error("Укажите исходный файл и файл-результат");
        return;
    }
    if (!fs::exists(input)) {
        show_error("Исходный файл не найден:\n" + input);
        return;
    }

    bool compress   = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app.radio_compress));
    bool is_zip     = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app.radio_zip));
    bool is_huff    = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app.radio_huff));
    bool resume     = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app.check_resume));
    uint32_t win    = static_cast<uint32_t>(
        gtk_spin_button_get_value(GTK_SPIN_BUTTON(app.spin_window))) * 1024;

    CompressOptions opts;
    opts.format   = is_zip ? ArchiveFormat::CZIP : ArchiveFormat::CRAR;
    opts.method   = !is_zip ? CompressMethod::PPM_LZSS_RNC :
                    (is_huff ? CompressMethod::HUFFMAN : CompressMethod::LZ77);
    opts.win_size = win;
    opts.resume   = resume;

    app.running = true;
    app.finished = false;
    app.had_error = false;
    app.progress_val = 0.0;
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app.progress_bar), 0.0);
    set_controls_sensitive(false);

    // Запускаем таймер обновления UI
    app.timer_id = g_timeout_add(50, update_ui, nullptr);  // ~20 FPS

    // Создаём компрессор с callbacks
    app.compressor = std::make_unique<Compressor>(
        [](double p) { app.progress_val = p; },
        [](const std::string& s) { app.status_text = s; }
    );

    // Запускаем рабочий поток
    if (app.worker.joinable()) app.worker.join();
    app.worker = std::thread([compress, input, output, opts]() {
        try {
            if (compress)
                app.compressor->compress(input, output, opts);
            else
                app.compressor->decompress(input, output);
            app.had_error = false;
        } catch (const std::exception& e) {
            app.had_error  = true;
            app.error_text = e.what();
            // Техническое сообщение скрываем — показываем понятный текст
            if (!compress)
                app.status_text = "⚠ Файл повреждён — восстановление прервано";
            else
                app.status_text = "⚠ Ошибка сжатия — операция прервана";
            app.progress_val = 0.0;
        }
        app.finished = true;
    });
}

// ─────────────────────────────────────────────────────────────
//  Кнопка «Отмена»
// ─────────────────────────────────────────────────────────────
static void on_btn_cancel(GtkButton*, gpointer) {
    if (app.compressor) {
        app.compressor->cancel();
        app.status_text = "Отмена... (будет сохранена контрольная точка)";
    }
}

// ─────────────────────────────────────────────────────────────
//  Построение интерфейса
// ─────────────────────────────────────────────────────────────
static void build_ui(GtkApplication* gapp, gpointer) {
    // ── Главное окно ─────────────────────────────────────────
    app.window = gtk_application_window_new(gapp);
    gtk_window_set_title(GTK_WINDOW(app.window), "FileCompressor – Архиватор");
    gtk_window_set_default_size(GTK_WINDOW(app.window), 620, 520);
    gtk_window_set_resizable(GTK_WINDOW(app.window), FALSE);

    // CSS стили
    GtkCssProvider* css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css, R"(
        window { background-color: #1e1e2e; }
        frame  { border-radius: 8px; }
        frame > label { color: #cdd6f4; font-weight: bold; }
        entry  { background: #313244; color: #cdd6f4; border-color: #45475a;
                 border-radius: 6px; padding: 4px 8px; }
        entry:focus { border-color: #89b4fa; }
        button { background: #313244; color: #cdd6f4; border-radius: 6px;
                 border-color: #45475a; padding: 4px 12px; }
        button:hover { background: #45475a; }
        button#btn_start { background: #89b4fa; color: #1e1e2e; font-weight: bold; }
        button#btn_start:hover { background: #b4befe; }
        button#btn_cancel { background: #f38ba8; color: #1e1e2e; font-weight: bold; }
        button#btn_cancel:hover { background: #fab387; }
        label  { color: #cdd6f4; }
        radiobutton label, checkbutton label { color: #cdd6f4; }
        progressbar trough { background: #313244; border-radius: 4px; min-height: 18px; }
        progressbar progress { background: #89b4fa; border-radius: 4px; }
        spinbutton { background: #313244; color: #cdd6f4; border-radius: 6px; }
    )", -1, nullptr);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    // ── Основной контейнер ───────────────────────────────────
    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 14);
    gtk_container_add(GTK_CONTAINER(app.window), vbox);

    // ── Заголовок ────────────────────────────────────────────
    GtkWidget* title = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(title),
        "<span font='16' weight='bold' color='#cba6f7'>🗜 FileCompressor</span>");
    gtk_box_pack_start(GTK_BOX(vbox), title, FALSE, FALSE, 0);

    // ── Секция: файлы ─────────────────────────────────────────
    GtkWidget* ffile = gtk_frame_new(" Файлы ");
    gtk_box_pack_start(GTK_BOX(vbox), ffile, FALSE, FALSE, 0);
    GtkWidget* grid_files = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid_files), 6);
    gtk_grid_set_row_spacing(GTK_GRID(grid_files), 6);
    gtk_container_set_border_width(GTK_CONTAINER(grid_files), 8);
    gtk_container_add(GTK_CONTAINER(ffile), grid_files);

    // Исходный файл
    gtk_grid_attach(GTK_GRID(grid_files),
                    gtk_label_new("Исходный файл:"), 0, 0, 1, 1);
    app.entry_input = gtk_entry_new();
    gtk_widget_set_hexpand(app.entry_input, TRUE);
    gtk_grid_attach(GTK_GRID(grid_files), app.entry_input, 1, 0, 1, 1);
    GtkWidget* btn_in = gtk_button_new_with_label("📁 Обзор");
    gtk_grid_attach(GTK_GRID(grid_files), btn_in, 2, 0, 1, 1);

    // Результат
    gtk_grid_attach(GTK_GRID(grid_files),
                    gtk_label_new("Файл-результат:"), 0, 1, 1, 1);
    app.entry_output = gtk_entry_new();
    gtk_widget_set_hexpand(app.entry_output, TRUE);
    gtk_grid_attach(GTK_GRID(grid_files), app.entry_output, 1, 1, 1, 1);
    GtkWidget* btn_out = gtk_button_new_with_label("📁 Обзор");
    gtk_grid_attach(GTK_GRID(grid_files), btn_out, 2, 1, 1, 1);

    // Тип файла
    app.label_ftype = gtk_label_new("Тип: не определён");
    gtk_widget_set_halign(app.label_ftype, GTK_ALIGN_START);
    GtkStyleContext* sc = gtk_widget_get_style_context(app.label_ftype);
    gtk_style_context_add_class(sc, "dim-label");
    gtk_grid_attach(GTK_GRID(grid_files), app.label_ftype, 1, 2, 2, 1);

    // ── Секция: действие ─────────────────────────────────────
    GtkWidget* faction = gtk_frame_new(" Действие ");
    gtk_box_pack_start(GTK_BOX(vbox), faction, FALSE, FALSE, 0);
    GtkWidget* hact = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 20);
    gtk_container_set_border_width(GTK_CONTAINER(hact), 8);
    gtk_container_add(GTK_CONTAINER(faction), hact);

    app.radio_compress = gtk_radio_button_new_with_label(nullptr, "⬇ Сжать");
    app.radio_decomp   = gtk_radio_button_new_with_label_from_widget(
                            GTK_RADIO_BUTTON(app.radio_compress), "⬆ Восстановить");
    gtk_box_pack_start(GTK_BOX(hact), app.radio_compress, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hact), app.radio_decomp,   FALSE, FALSE, 0);

    // ── Секция: формат ───────────────────────────────────────
    app.frame_format = gtk_frame_new(" Формат архива ");
    GtkWidget* fformat = app.frame_format;
    gtk_box_pack_start(GTK_BOX(vbox), fformat, FALSE, FALSE, 0);
    GtkWidget* hfmt = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 20);
    gtk_container_set_border_width(GTK_CONTAINER(hfmt), 8);
    gtk_container_add(GTK_CONTAINER(fformat), hfmt);

    app.radio_zip = gtk_radio_button_new_with_label(nullptr,
        "🗂 CZIP (ZIP-стиль)");
    app.radio_rar = gtk_radio_button_new_with_label_from_widget(
        GTK_RADIO_BUTTON(app.radio_zip), "📦 CRAR (RAR-стиль)");
    gtk_box_pack_start(GTK_BOX(hfmt), app.radio_zip, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hfmt), app.radio_rar, FALSE, FALSE, 0);

    // ── Секция: метод (ZIP) ───────────────────────────────────
    app.frame_method = gtk_frame_new(" Метод сжатия (CZIP) ");
    GtkWidget* fmethod = app.frame_method;
    gtk_box_pack_start(GTK_BOX(vbox), fmethod, FALSE, FALSE, 0);
    GtkWidget* hmeth = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 20);
    gtk_container_set_border_width(GTK_CONTAINER(hmeth), 8);
    gtk_container_add(GTK_CONTAINER(fmethod), hmeth);

    app.radio_huff = gtk_radio_button_new_with_label(nullptr,
        "🌳 Хаффман (быстро)");
    app.radio_lz77 = gtk_radio_button_new_with_label_from_widget(
        GTK_RADIO_BUTTON(app.radio_huff),
        "🔄 LZ77 + Хаффман (лучший коэффициент)");
    gtk_box_pack_start(GTK_BOX(hmeth), app.radio_huff, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hmeth), app.radio_lz77, FALSE, FALSE, 0);

    GtkWidget* hint_rar = gtk_label_new("CRAR всегда использует PPM → LZSS → RNC");
    gtk_widget_set_halign(hint_rar, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(hmeth), hint_rar, FALSE, FALSE, 0);

    // ── Секция: размер окна LZ77 ──────────────────────────────
    app.frame_lz77 = gtk_frame_new(" Размер окна LZ77 ");
    gtk_box_pack_start(GTK_BOX(vbox), app.frame_lz77, FALSE, FALSE, 0);
    GtkWidget* hlz = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(hlz), 8);
    gtk_container_add(GTK_CONTAINER(app.frame_lz77), hlz);

    GtkWidget* lbl_win = gtk_label_new("Окно:");
    app.spin_window = gtk_spin_button_new_with_range(1, 64, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(app.spin_window), 32);
    GtkWidget* lbl_kb = gtk_label_new("КБ");

    // Шкала (слайдер)
    GtkWidget* scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,
                                                 1, 64, 1);
    gtk_range_set_value(GTK_RANGE(scale), 32);
    gtk_scale_set_draw_value(GTK_SCALE(scale), FALSE);
    gtk_widget_set_hexpand(scale, TRUE);

    // Явная двусторонняя синхронизация через value-changed.
    // Флаг updating предотвращает бесконечный цикл: spin→scale→spin→...
    static bool updating = false;

    g_signal_connect(app.spin_window, "value-changed",
        G_CALLBACK(+[](GtkSpinButton* spin, gpointer scale_ptr) {
            if (updating) return;
            updating = true;
            gtk_range_set_value(GTK_RANGE(scale_ptr),
                                gtk_spin_button_get_value(spin));
            updating = false;
        }), scale);

    g_signal_connect(scale, "value-changed",
        G_CALLBACK(+[](GtkRange* range, gpointer spin_ptr) {
            if (updating) return;
            updating = true;
            gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin_ptr),
                                      gtk_range_get_value(range));
            updating = false;
        }), app.spin_window);

    gtk_box_pack_start(GTK_BOX(hlz), lbl_win,        FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hlz), app.spin_window, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hlz), lbl_kb,          FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hlz), scale,            TRUE, TRUE, 8);

    // ── Опции ────────────────────────────────────────────────
    app.frame_opts = gtk_frame_new(" Опции ");
    GtkWidget* fopts = app.frame_opts;
    gtk_box_pack_start(GTK_BOX(vbox), fopts, FALSE, FALSE, 0);
    GtkWidget* hopts = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    gtk_container_set_border_width(GTK_CONTAINER(hopts), 8);
    gtk_container_add(GTK_CONTAINER(fopts), hopts);

    app.check_resume = gtk_check_button_new_with_label(
        "🔁 Продолжить прерванное сжатие (если есть контрольная точка)");
    gtk_box_pack_start(GTK_BOX(hopts), app.check_resume, FALSE, FALSE, 0);

    // ── Прогресс ─────────────────────────────────────────────
    GtkWidget* fpr = gtk_frame_new(" Прогресс ");
    gtk_box_pack_start(GTK_BOX(vbox), fpr, FALSE, FALSE, 0);
    GtkWidget* vpr = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(vpr), 8);
    gtk_container_add(GTK_CONTAINER(fpr), vpr);

    app.progress_bar = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(app.progress_bar), TRUE);
    gtk_box_pack_start(GTK_BOX(vpr), app.progress_bar, FALSE, FALSE, 0);

    app.label_status = gtk_label_new("Готов к работе");
    gtk_widget_set_halign(app.label_status, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(app.label_status), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(vpr), app.label_status, FALSE, FALSE, 0);

    // ── Кнопки действий ──────────────────────────────────────
    GtkWidget* hbtn = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_halign(hbtn, GTK_ALIGN_END);
    gtk_box_pack_start(GTK_BOX(vbox), hbtn, FALSE, FALSE, 0);

    app.btn_start  = gtk_button_new_with_label("▶ Выполнить");
    app.btn_cancel = gtk_button_new_with_label("✖ Отмена");
    gtk_widget_set_name(app.btn_start,  "btn_start");
    gtk_widget_set_name(app.btn_cancel, "btn_cancel");
    gtk_widget_set_sensitive(app.btn_cancel, FALSE);

    gtk_box_pack_start(GTK_BOX(hbtn), app.btn_start,  FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbtn), app.btn_cancel, FALSE, FALSE, 0);

    // ── Подключение сигналов ──────────────────────────────────
    g_signal_connect(btn_in, "clicked", G_CALLBACK(+[](GtkButton*, gpointer) {
        auto p = browse_file(false, "Выберите исходный файл");
        if (!p.empty()) gtk_entry_set_text(GTK_ENTRY(app.entry_input), p.c_str());
    }), nullptr);

    g_signal_connect(btn_out, "clicked", G_CALLBACK(+[](GtkButton*, gpointer) {
        auto p = browse_file(true, "Укажите файл-результат");
        if (!p.empty()) gtk_entry_set_text(GTK_ENTRY(app.entry_output), p.c_str());
    }), nullptr);

    g_signal_connect(app.entry_input,    "changed", G_CALLBACK(on_input_changed), nullptr);

    // Все переключатели ведут к одной функции обновления состояния UI
    g_signal_connect(app.radio_compress, "toggled", G_CALLBACK(update_ui_state), nullptr);
    g_signal_connect(app.radio_decomp,   "toggled", G_CALLBACK(update_ui_state), nullptr);
    g_signal_connect(app.radio_zip,      "toggled", G_CALLBACK(update_ui_state), nullptr);
    g_signal_connect(app.radio_rar,      "toggled", G_CALLBACK(update_ui_state), nullptr);
    g_signal_connect(app.radio_lz77,     "toggled", G_CALLBACK(update_ui_state), nullptr);
    g_signal_connect(app.radio_huff,     "toggled", G_CALLBACK(update_ui_state), nullptr);

    g_signal_connect(app.btn_start,  "clicked", G_CALLBACK(on_btn_start),  nullptr);
    g_signal_connect(app.btn_cancel, "clicked", G_CALLBACK(on_btn_cancel), nullptr);

    // Начальное состояние
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app.radio_compress), TRUE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app.radio_zip),      TRUE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app.radio_huff),     TRUE);

    // hint_rar больше не нужен — метод секция целиком блокируется при CRAR
    gtk_widget_set_visible(hint_rar, FALSE);

    // Применяем начальное состояние (вызовет update_ui_state через signals выше)
    update_ui_state();

    gtk_widget_show_all(app.window);
    gtk_widget_set_visible(app.frame_lz77, FALSE);
    gtk_widget_set_visible(hint_rar, FALSE);
}

// ─────────────────────────────────────────────────────────────
//  main
// ─────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    GtkApplication* gapp = gtk_application_new(
        "ru.compressor.file", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(gapp, "activate", G_CALLBACK(build_ui), nullptr);
    int status = g_application_run(G_APPLICATION(gapp), argc, argv);
    g_object_unref(gapp);
    return status;
}
