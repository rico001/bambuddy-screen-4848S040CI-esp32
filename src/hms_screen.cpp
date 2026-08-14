#include "hms_screen.h"

#include <stdio.h>
#include <time.h>

#include "bambuddy_hms.h"
#include "ui_dialog.h"
#include "ui_layout.h"
#include "ui_theme.h"
#include "ui_util.h"

static constexpr int PAD = 12;
static constexpr int HEADER_H = 54;
// Zwei Zeilen fuer den Text, darunter eine schmale Zeile fuer Code und
// Zeitpunkt. Vorher passte alles in eine Zeile — Meldungen wie "AMS-Motor
// ueberlastet - Filament verheddert oder Spule klemmt" wurden dabei
// abgeschnitten, und der Zeitstempel rutschte in den Text hinein.
static constexpr int TITLE_H = 40; // zwei Zeilen in der Standardschrift
static constexpr int META_H = 16;
static constexpr int ROW_H = 8 + TITLE_H + 4 + META_H + 8;

static lv_obj_t *list_cont = nullptr;
static lv_obj_t *empty_lbl = nullptr;
static lv_obj_t *hint_lbl = nullptr;
static lv_timer_t *ui_timer = nullptr;

// Schwere in Farbe: Man soll die eine Zeile finden, die zaehlt, ohne alle
// zehn zu lesen.
static uint32_t severity_color(int32_t severity)
{
    switch (severity) {
    case BB_HMS_SEVERITY_OK: return COL_OK;
    case 1: return COL_ERR;
    case 2: return COL_ERR;
    case 3: return COL_WARN;
    case 4: return COL_MUTED;
    default: return COL_WARN;
    }
}

// Wann war das? Mit gestellter Uhr Datum und Zeit, sonst die Laufzeit beim
// Auftreten — eine Uhrzeit aus dem Startwert des Chips waere schlechter als
// eine ehrliche Naeherung.
static void format_when(const bambuddy_hms_entry_t &e, char *out, size_t out_len)
{
    // Aus dem Speicher geladen und ohne Zeitstempel: Die Laufzeitangabe
    // stammt aus einem frueheren Lauf und waere jetzt irrefuehrend.
    if (e.when == 0 && e.restored) {
        snprintf(out, out_len, "frueherer Lauf");
        return;
    }

    // In der ersten Minute nach dem Start waere "nach 0 min Laufzeit" keine
    // Angabe, sondern ein Platzhalter. Der Starteintrag selbst faellt immer
    // hierunter.
    if (e.when == 0 && e.uptime_s < 60) {
        snprintf(out, out_len, "beim Start");
        return;
    }

    if (e.when == 0) {
        const uint32_t minutes = e.uptime_s / 60;
        if (minutes < 60) {
            snprintf(out, out_len, "nach %u min Laufzeit", (unsigned)minutes);
        } else {
            snprintf(out, out_len, "nach %u h %02u min Laufzeit",
                     (unsigned)(minutes / 60), (unsigned)(minutes % 60));
        }
        return;
    }

    struct tm tm_when;
    localtime_r(&e.when, &tm_when);
    snprintf(out, out_len, "%02d.%02d.  %02d:%02d", tm_when.tm_mday % 100,
             (tm_when.tm_mon + 1) % 100, tm_when.tm_hour, tm_when.tm_min);
}

static void build_row(const bambuddy_hms_entry_t &e)
{
    lv_obj_t *row = lv_obj_create(list_cont);
    lv_obj_set_size(row, LV_PCT(100), ROW_H);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(row, 10, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_border_width(row, 0, 0);

    // Schmaler Streifen links statt eines farbigen Hintergrunds: Zehn bunte
    // Flaechen uebereinander liest niemand mehr.
    lv_obj_t *bar = lv_obj_create(row);
    lv_obj_set_size(bar, 5, ROW_H - 16);
    lv_obj_align(bar, LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(bar, 3, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(severity_color(e.severity)), 0);

    // Oben der Klartext, soweit bekannt — sonst der Code, damit die Zeile
    // nicht leer bleibt. Feste Breite UND Hoehe: nur dann kuerzt LVGL
    // mehrzeilig statt einzeilig.
    lv_obj_t *title = lv_label_create(row);
    lv_label_set_text(title, e.text[0] ? e.text : e.code);
    lv_obj_set_size(title, SCREEN_W - 2 * PAD - 26 - 14, TITLE_H);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 26, 8);

    // Untere Zeile: links der Code zum Nachschlagen, rechts der Zeitpunkt.
    //
    // Rechtsbuendig, damit die Zeitangaben aller Eintraege untereinander
    // stehen — so liest man den Verlauf, ohne den Blick springen zu lassen.
    // Zustandsmeldungen haben keinen Code; dort bleibt links nur die Schwere.
    lv_obj_t *meta = lv_label_create(row);
    if (e.code[0]) {
        lv_label_set_text_fmt(meta, "%s   %s", bambuddy_hms_severity_text(e.severity),
                              e.code);
    } else {
        lv_label_set_text(meta, bambuddy_hms_severity_text(e.severity));
    }
    lv_obj_set_style_text_font(meta, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(meta, lv_color_hex(COL_MUTED), 0);
    lv_obj_set_width(meta, SCREEN_W - 2 * PAD - 26 - 130);
    lv_label_set_long_mode(meta, LV_LABEL_LONG_DOT);
    lv_obj_align(meta, LV_ALIGN_BOTTOM_LEFT, 26, -8);

    char when[36];
    format_when(e, when, sizeof(when));

    lv_obj_t *when_lbl = lv_label_create(row);
    lv_label_set_text(when_lbl, when);
    lv_obj_set_style_text_font(when_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(when_lbl, lv_color_hex(COL_MUTED), 0);
    lv_obj_align(when_lbl, LV_ALIGN_BOTTOM_RIGHT, -14, -8);
}

static void rebuild();

static void clear_confirmed(void *)
{
    bambuddy_hms_clear();

    // Sofort neu zeichnen statt auf den Sekundentakt zu warten. Sicher, weil
    // die Liste nicht zum Dialog gehoert, aus dessen Rueckruf wir kommen.
    rebuild();
}

static void clear_cb(lv_event_t *)
{
    if (ui_confirm_is_open()) return;

    ui_confirm("Protokoll leeren?",
               "Alle Eintraege werden geloescht, auch die gespeicherten.",
               "Abbrechen", "Leeren", COL_ERR, clear_confirmed, nullptr);
}

static void rebuild()
{
    lv_obj_clean(list_cont);

    const int count = bambuddy_hms_count();
    if (count == 0) {
        lv_obj_remove_flag(empty_lbl, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_add_flag(empty_lbl, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < count; i++) {
        bambuddy_hms_entry_t e;
        if (bambuddy_hms_get(i, &e)) build_row(e);
    }
}

static void ui_tick_cb(lv_timer_t *)
{
    if (bambuddy_hms_take_fresh()) rebuild();
}

void hms_screen_create(lv_obj_t *parent)
{
    list_cont = lv_obj_create(parent);
    lv_obj_set_size(list_cont, SCREEN_W - 2 * PAD, SCREEN_H - HEADER_H - 40);
    lv_obj_align(list_cont, LV_ALIGN_TOP_MID, 0, HEADER_H);
    lv_obj_set_style_bg_opa(list_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list_cont, 0, 0);
    lv_obj_set_style_pad_all(list_cont, 0, 0);
    lv_obj_set_style_pad_row(list_cont, 8, 0);
    lv_obj_set_flex_flow(list_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list_cont, LV_DIR_VER);

    // Eine Aussage, keine Luecke: "keine Meldungen" ist die gute Nachricht.
    empty_lbl = lv_label_create(parent);
    lv_label_set_text(empty_lbl, "Seit dem Start nichts protokolliert.");
    lv_obj_set_style_text_color(empty_lbl, lv_color_hex(COL_MUTED), 0);
    lv_obj_align(empty_lbl, LV_ALIGN_CENTER, 0, -20);

    hint_lbl = lv_label_create(parent);
    lv_label_set_text(hint_lbl, "Codes nachschlagen: wiki.bambulab.com/en/hms/home");
    lv_obj_set_style_text_font(hint_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(hint_lbl, lv_color_hex(COL_MUTED), 0);
    lv_obj_align(hint_lbl, LV_ALIGN_BOTTOM_LEFT, PAD + 2, -12);

    // Oben rechts, auf Hoehe des Zurueck-Knopfes. Unten waere er neben dem
    // Wiki-Hinweis gelandet, wo man ihn beim Scrollen versehentlich trifft.
    lv_obj_t *clear_btn = lv_button_create(parent);
    lv_obj_set_size(clear_btn, 96, 38);
    lv_obj_align(clear_btn, LV_ALIGN_TOP_RIGHT, -PAD, 7);
    lv_obj_set_style_radius(clear_btn, 10, 0);
    lv_obj_set_style_bg_color(clear_btn, lv_color_hex(COL_ERR), 0);
    lv_obj_add_event_cb(clear_btn, clear_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *clear_lbl = lv_label_create(clear_btn);
    lv_label_set_text(clear_lbl, LV_SYMBOL_TRASH "  Leeren");
    lv_obj_center(clear_lbl);

    ui_timer = lv_timer_create(ui_tick_cb, 1000, nullptr);
    lv_timer_set_repeat_count(ui_timer, -1);

    // Das Frisch-Merkmal koennte von einem frueheren Besuch verbraucht sein.
    rebuild();
}

void hms_screen_destroy()
{
    // Eine offene Rueckfrage wuerde sonst ueber der naechsten Ansicht
    // stehen bleiben und auf geloeschte Objekte zeigen.
    ui_confirm_close();

    if (ui_timer) {
        lv_timer_delete(ui_timer);
        ui_timer = nullptr;
    }
    list_cont = nullptr;
    empty_lbl = nullptr;
    hint_lbl = nullptr;
}
