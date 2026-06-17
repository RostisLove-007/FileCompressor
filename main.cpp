#include <gtk/gtk.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <string>
#include <filesystem>
#include "compressor.hpp"

namespace fs = std::filesystem;

// ─────────────────────────────────────────────────────────────
//  Состояние приложения
// ─────────────────────────────────────────────────────────────
struct App {
    GtkWidget* window        = nullptr;
    GtkWidget* radio_comp    = nullptr;
    GtkWidget* radio_decomp  = nullptr;
    GtkWidget* radio_zip     = nullptr;
    GtkWidget* radio_rar     = nullptr;
    GtkWidget* radio_huff    = nullptr;
    GtkWidget* radio_lz77    = nullptr;
    GtkWidget* radio_rnc     = nullptr;
    GtkWidget* entry_input   = nullptr;
    GtkWidget* entry_output  = nullptr;
    GtkWidget* spin_window   = nullptr;
    GtkWidget* frame_lz77    = nullptr;
    GtkWidget* frame_method  = nullptr;
    GtkWidget* frame_format  = nullptr;
    GtkWidget* frame_opts    = nullptr;
    GtkWidget* check_resume  = nullptr;
    GtkWidget* progress_bar  = nullptr;
    GtkWidget* label_status  = nullptr;
    GtkWidget* btn_start     = nullptr;
    GtkWidget* btn_cancel    = nullptr;

    std::atomic<double>      progress_val{0.0};
    std::mutex               status_mutex;
    std::string              status_text{"Готов к работе"};
    std::atomic<bool>        working{false};
    std::atomic<bool>        had_error{false};
    std::string              error_text;
    std::string              friendly_error;   // сообщение для диалога
    std::unique_ptr<Compressor> comp;
} app;

// ─────────────────────────────────────────────────────────────
//  Таймер обновления UI (каждые 80 мс)
// ─────────────────────────────────────────────────────────────
static gboolean update_ui(gpointer) {
    double frac = app.progress_val.load();
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app.progress_bar), frac);

    // Процент прямо на полоске прогресса
    char pct[16];
    if      (frac <= 0.0) pct[0] = '\0';
    else if (frac >= 1.0) std::snprintf(pct, sizeof(pct), "100%%");
    else                  std::snprintf(pct, sizeof(pct), "%.0f%%", frac*100.0);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(app.progress_bar), pct);

    {
        std::lock_guard<std::mutex> lk(app.status_mutex);
        gtk_label_set_text(GTK_LABEL(app.label_status),
                           app.status_text.c_str());
    }

    if (!app.working.load()) {
        gtk_widget_set_sensitive(app.btn_start,  TRUE);
        gtk_widget_set_sensitive(app.btn_cancel, FALSE);

        if (app.had_error.load()) {
            std::string msg;
            {
                std::lock_guard<std::mutex> lk(app.status_mutex);
                // Показываем дружелюбное сообщение, не технические детали
                msg = app.friendly_error.empty()
                    ? app.error_text : app.friendly_error;
            }
            GtkWidget* dlg = gtk_message_dialog_new(
                GTK_WINDOW(app.window), GTK_DIALOG_MODAL,
                GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "%s", msg.c_str());
            gtk_window_set_title(GTK_WINDOW(dlg), "Ошибка");
            gtk_dialog_run(GTK_DIALOG(dlg));
            gtk_widget_destroy(dlg);
            app.had_error = false;
        }
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

// ─────────────────────────────────────────────────────────────
//  Рабочий поток
// ─────────────────────────────────────────────────────────────
static void worker_thread(std::string input, std::string output,
                          bool decomp, CompressOptions opts)
{
    auto set_status = [](const std::string& s) {
        std::lock_guard<std::mutex> lk(app.status_mutex);
        app.status_text = s;
    };

    app.comp = std::make_unique<Compressor>(
        [](double p){ app.progress_val.store(p); },
        set_status);

    try {
        if (decomp)
            app.comp->decompress(input, output);
        else
            app.comp->compress(input, output, opts);
    } catch (const std::exception& e) {
        app.had_error = true;
        std::lock_guard<std::mutex> lk(app.status_mutex);
        app.error_text = e.what();

        // Дружелюбное сообщение по контексту
        std::string err(e.what());
        if (err == "Отменено") {
            app.had_error = false;   // отмена — не ошибка, не показываем диалог
        } else if (decomp) {
            app.friendly_error = "Файл повреждён — восстановление прервано";
            app.status_text    = "⚠ Файл повреждён";
        } else {
            app.friendly_error = "Ошибка сжатия — операция прервана";
            app.status_text    = "⚠ Ошибка сжатия";
        }
    }
    app.working = false;
}

// ─────────────────────────────────────────────────────────────
//  update_ui_state — единая точка управления видимостью секций
// ─────────────────────────────────────────────────────────────
static void update_ui_state(GtkToggleButton* = nullptr, gpointer = nullptr) {
    bool decomp  = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app.radio_decomp));
    bool is_zip  = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app.radio_zip));
    bool is_lz77 = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app.radio_lz77));

    // При смене режима очищаем пути — они теряют смысл
    static bool last_decomp = false;
    if (decomp != last_decomp) {
        last_decomp = decomp;
        gtk_entry_set_text(GTK_ENTRY(app.entry_input),  "");
        gtk_entry_set_text(GTK_ENTRY(app.entry_output), "");
    }

    // Секции видны / скрыты в зависимости от режима
    gtk_widget_set_visible(app.frame_format, !decomp);
    gtk_widget_set_visible(app.frame_opts,   !decomp);
    gtk_widget_set_visible(app.frame_method, !decomp && is_zip);
    gtk_widget_set_visible(app.frame_lz77,   !decomp && is_zip && is_lz77);
}

// ─────────────────────────────────────────────────────────────
//  Callbacks
// ─────────────────────────────────────────────────────────────
static void on_input_changed(GtkEditable*, gpointer) {
    const char* in = gtk_entry_get_text(GTK_ENTRY(app.entry_input));
    if (!in || !*in) return;
    bool decomp = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app.radio_decomp));
    gtk_entry_set_text(GTK_ENTRY(app.entry_output),
                       (std::string(in) + (decomp ? ".out" : ".czip")).c_str());
}

static void choose_file(GtkEntry* entry, bool save) {
    GtkWidget* dlg = gtk_file_chooser_dialog_new(
        save ? "Файл-результат" : "Исходный файл",
        GTK_WINDOW(app.window),
        save ? GTK_FILE_CHOOSER_ACTION_SAVE : GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Отмена", GTK_RESPONSE_CANCEL,
        save ? "_Сохранить" : "_Открыть", GTK_RESPONSE_ACCEPT,
        nullptr);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char* path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        gtk_entry_set_text(entry, path);
        g_free(path);
    }
    gtk_widget_destroy(dlg);
}

static void on_btn_start(GtkButton*, gpointer) {
    std::string input  = gtk_entry_get_text(GTK_ENTRY(app.entry_input));
    std::string output = gtk_entry_get_text(GTK_ENTRY(app.entry_output));

    if (input.empty() || output.empty()) {
        GtkWidget* dlg = gtk_message_dialog_new(
            GTK_WINDOW(app.window), GTK_DIALOG_MODAL,
            GTK_MESSAGE_WARNING, GTK_BUTTONS_OK,
            "Укажите исходный файл и файл-результат");
        gtk_dialog_run(GTK_DIALOG(dlg));
        gtk_widget_destroy(dlg);
        return;
    }

    bool decomp = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app.radio_decomp));

    CompressOptions opts;
    opts.format = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app.radio_zip))
                ? ArchiveFormat::CZIP : ArchiveFormat::CRAR;
    opts.method = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app.radio_lz77))
                ? CompressMethod::LZ77
                : gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(app.radio_rnc))
                ? CompressMethod::LZSS_RNC : CompressMethod::HUFFMAN;
    opts.win_size = static_cast<uint32_t>(
        gtk_spin_button_get_value(GTK_SPIN_BUTTON(app.spin_window))) * 1024;
    opts.resume = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON(app.check_resume));

    app.working   = true;
    app.had_error = false;
    app.friendly_error.clear();
    app.progress_val.store(0.0);

    gtk_widget_set_sensitive(app.btn_start,  FALSE);
    gtk_widget_set_sensitive(app.btn_cancel, TRUE);
    g_timeout_add(80, update_ui, nullptr);

    std::thread(worker_thread, input, output, decomp, opts).detach();
}

static void on_btn_cancel(GtkButton*, gpointer) {
    if (app.comp) app.comp->cancel();
}

// ─────────────────────────────────────────────────────────────
//  Построение UI
// ─────────────────────────────────────────────────────────────
static GtkWidget* make_frame(GtkWidget* vbox, const char* title) {
    GtkWidget* f = gtk_frame_new(title);
    gtk_box_pack_start(GTK_BOX(vbox), f, FALSE, FALSE, 0);
    return f;
}

static GtkWidget* make_hbox_in_frame(GtkWidget* frame, int spacing = 12) {
    GtkWidget* hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, spacing);
    gtk_widget_set_margin_start(hb, 8);  gtk_widget_set_margin_end(hb, 8);
    gtk_widget_set_margin_top(hb, 6);    gtk_widget_set_margin_bottom(hb, 6);
    gtk_container_add(GTK_CONTAINER(frame), hb);
    return hb;
}

static void build_ui(GtkApplication* gapp) {
    app.window = gtk_application_window_new(gapp);
    gtk_window_set_title(GTK_WINDOW(app.window), "CZip v2.0 — Архиватор");
    gtk_window_set_default_size(GTK_WINDOW(app.window), 500, -1);
    gtk_window_set_resizable(GTK_WINDOW(app.window), FALSE);
    gtk_container_set_border_width(GTK_CONTAINER(app.window), 12);

    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_add(GTK_CONTAINER(app.window), vbox);

    // ── Режим ────────────────────────────────────────────────
    GtkWidget* hmode = make_hbox_in_frame(make_frame(vbox, " Режим "));
    app.radio_comp   = gtk_radio_button_new_with_label(nullptr, "Сжатие");
    app.radio_decomp = gtk_radio_button_new_with_label_from_widget(
        GTK_RADIO_BUTTON(app.radio_comp), "Восстановление");
    gtk_box_pack_start(GTK_BOX(hmode), app.radio_comp,   FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hmode), app.radio_decomp, FALSE, FALSE, 0);

    // ── Файлы ────────────────────────────────────────────────
    GtkWidget* ffiles = make_frame(vbox, " Файлы ");
    GtkWidget* gfiles = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(gfiles), 6);
    gtk_grid_set_row_spacing(GTK_GRID(gfiles), 6);
    gtk_widget_set_margin_start(gfiles, 8); gtk_widget_set_margin_end(gfiles, 8);
    gtk_widget_set_margin_top(gfiles, 6);   gtk_widget_set_margin_bottom(gfiles, 6);
    gtk_container_add(GTK_CONTAINER(ffiles), gfiles);

    struct BtnData { GtkEntry* entry; bool save; };
    auto make_row = [&](int row, const char* lbl,
                         GtkWidget*& entry, bool save) {
        GtkWidget* l = gtk_label_new(lbl);
        gtk_widget_set_halign(l, GTK_ALIGN_END);
        gtk_grid_attach(GTK_GRID(gfiles), l, 0, row, 1, 1);
        entry = gtk_entry_new();
        gtk_widget_set_hexpand(entry, TRUE);
        gtk_grid_attach(GTK_GRID(gfiles), entry, 1, row, 1, 1);
        GtkWidget* btn = gtk_button_new_with_label("…");
        gtk_grid_attach(GTK_GRID(gfiles), btn, 2, row, 1, 1);
        auto* bd = new BtnData{GTK_ENTRY(entry), save};
        g_signal_connect(btn, "clicked",
            G_CALLBACK(+[](GtkButton*, gpointer d) {
                auto* p = static_cast<BtnData*>(d);
                choose_file(p->entry, p->save);
            }), bd);
    };
    make_row(0, "Исходный:",  app.entry_input,  false);
    make_row(1, "Результат:", app.entry_output, true);

    // ── Формат архива ─────────────────────────────────────────
    app.frame_format = make_frame(vbox, " Формат архива ");
    GtkWidget* hfmt  = make_hbox_in_frame(app.frame_format);
    app.radio_zip    = gtk_radio_button_new_with_label(nullptr, "CZIP");
    app.radio_rar    = gtk_radio_button_new_with_label_from_widget(
        GTK_RADIO_BUTTON(app.radio_zip), "CRAR");
    gtk_box_pack_start(GTK_BOX(hfmt), app.radio_zip, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hfmt), app.radio_rar, FALSE, FALSE, 0);

    // ── Метод сжатия ─────────────────────────────────────────
    app.frame_method = make_frame(vbox, " Метод сжатия ");
    GtkWidget* hmeth = make_hbox_in_frame(app.frame_method);
    app.radio_huff   = gtk_radio_button_new_with_label(nullptr, "Huffman");
    app.radio_lz77   = gtk_radio_button_new_with_label_from_widget(
        GTK_RADIO_BUTTON(app.radio_huff), "LZ77");
    app.radio_rnc    = gtk_radio_button_new_with_label_from_widget(
        GTK_RADIO_BUTTON(app.radio_huff), "LZSS + RNC");
    gtk_box_pack_start(GTK_BOX(hmeth), app.radio_huff, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hmeth), app.radio_lz77, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hmeth), app.radio_rnc,  FALSE, FALSE, 0);

    // ── Опции ────────────────────────────────────────────────
    app.frame_opts   = make_frame(vbox, " Опции ");
    GtkWidget* hopts = make_hbox_in_frame(app.frame_opts, 8);
    app.check_resume = gtk_check_button_new_with_label(
        "Продолжить прерванное сжатие");
    gtk_box_pack_start(GTK_BOX(hopts), app.check_resume, FALSE, FALSE, 0);

    // ── Окно LZ77 ────────────────────────────────────────────
    app.frame_lz77   = make_frame(vbox, " Размер окна LZ77 ");
    GtkWidget* hlz   = make_hbox_in_frame(app.frame_lz77, 8);

    gtk_box_pack_start(GTK_BOX(hlz), gtk_label_new("Окно:"), FALSE, FALSE, 0);
    app.spin_window = gtk_spin_button_new_with_range(1, 64, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(app.spin_window), 32);
    gtk_box_pack_start(GTK_BOX(hlz), app.spin_window, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hlz), gtk_label_new("КБ"), FALSE, FALSE, 0);

    GtkWidget* scale = gtk_scale_new_with_range(
        GTK_ORIENTATION_HORIZONTAL, 1, 64, 1);
    gtk_range_set_value(GTK_RANGE(scale), 32);
    gtk_scale_set_draw_value(GTK_SCALE(scale), FALSE);
    gtk_widget_set_hexpand(scale, TRUE);
    gtk_box_pack_start(GTK_BOX(hlz), scale, TRUE, TRUE, 0);

    // Двусторонняя синхронизация слайдер ↔ спиннер
    static bool updating = false;
    g_signal_connect(app.spin_window, "value-changed",
        G_CALLBACK(+[](GtkSpinButton* s, gpointer sc) {
            if (updating) return;
            updating = true;
            gtk_range_set_value(GTK_RANGE(sc),
                gtk_spin_button_get_value(s));
            updating = false;
        }), scale);
    g_signal_connect(scale, "value-changed",
        G_CALLBACK(+[](GtkRange* r, gpointer sp) {
            if (updating) return;
            updating = true;
            gtk_spin_button_set_value(GTK_SPIN_BUTTON(sp),
                gtk_range_get_value(r));
            updating = false;
        }), app.spin_window);

    // ── Прогресс ─────────────────────────────────────────────
    app.progress_bar = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(app.progress_bar), TRUE);
    gtk_box_pack_start(GTK_BOX(vbox), app.progress_bar, FALSE, FALSE, 4);

    app.label_status = gtk_label_new("Готов к работе");
    gtk_widget_set_halign(app.label_status, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(app.label_status), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(vbox), app.label_status, FALSE, FALSE, 0);

    // ── Кнопки ───────────────────────────────────────────────
    GtkWidget* hbtn = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(vbox), hbtn, FALSE, FALSE, 4);
    gtk_widget_set_halign(hbtn, GTK_ALIGN_END);

    app.btn_cancel = gtk_button_new_with_label("Отмена");
    app.btn_start  = gtk_button_new_with_label("▶  Старт");
    gtk_widget_set_sensitive(app.btn_cancel, FALSE);

    GtkStyleContext* sc_start =
        gtk_widget_get_style_context(app.btn_start);
    gtk_style_context_add_class(sc_start, "suggested-action");

    gtk_box_pack_start(GTK_BOX(hbtn), app.btn_cancel, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbtn), app.btn_start,  FALSE, FALSE, 0);

    // ── Сигналы ──────────────────────────────────────────────
    g_signal_connect(app.entry_input,  "changed", G_CALLBACK(on_input_changed), nullptr);
    g_signal_connect(app.radio_comp,   "toggled", G_CALLBACK(update_ui_state),  nullptr);
    g_signal_connect(app.radio_decomp, "toggled", G_CALLBACK(update_ui_state),  nullptr);
    g_signal_connect(app.radio_zip,    "toggled", G_CALLBACK(update_ui_state),  nullptr);
    g_signal_connect(app.radio_rar,    "toggled", G_CALLBACK(update_ui_state),  nullptr);
    g_signal_connect(app.radio_lz77,   "toggled", G_CALLBACK(update_ui_state),  nullptr);
    g_signal_connect(app.radio_huff,   "toggled", G_CALLBACK(update_ui_state),  nullptr);
    g_signal_connect(app.radio_rnc,    "toggled", G_CALLBACK(update_ui_state),  nullptr);
    g_signal_connect(app.btn_start,    "clicked", G_CALLBACK(on_btn_start),     nullptr);
    g_signal_connect(app.btn_cancel,   "clicked", G_CALLBACK(on_btn_cancel),    nullptr);

    // Начальное состояние
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app.radio_comp), TRUE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app.radio_zip),  TRUE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app.radio_huff), TRUE);
    update_ui_state();

    gtk_widget_show_all(app.window);
    update_ui_state();   // второй вызов скрывает нужные секции после show_all
}

static void on_activate(GtkApplication* gapp, gpointer) { build_ui(gapp); }

int main(int argc, char* argv[]) {
    auto* gapp = gtk_application_new("local.czip",
                                     G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(gapp, "activate", G_CALLBACK(on_activate), nullptr);
    int rc = g_application_run(G_APPLICATION(gapp), argc, argv);
    g_object_unref(gapp);
    return rc;
}
