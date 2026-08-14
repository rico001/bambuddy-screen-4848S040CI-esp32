#include "screensaver.h"

#include <lvgl.h>
#include <string.h>
#include <time.h>

#include "bambuddy_api.h"
#include "settings_screen.h"
#include "ui_layout.h"
#include "ui_theme.h"
#include "ui_util.h"

static lv_obj_t *overlay = nullptr;
static lv_obj_t *time_lbl = nullptr;
static lv_obj_t *date_lbl = nullptr;
static lv_obj_t *printer_lbl = nullptr;
static lv_timer_t *tick = nullptr;
static int shown_minute = -1;

// Wochentage ausgeschrieben statt der englischen Kuerzel von strftime: Das
// Geraet spricht sonst ueberall Deutsch.
static const char *const WEEKDAYS[] = {"Sonntag", "Montag",     "Dienstag", "Mittwoch",
                                       "Donnerstag", "Freitag", "Samstag"};

// Was macht der Drucker gerade? Genau die Zeile, fuer die man sonst
// hingeht und antippt. Die Daten liegen ohnehin im Speicher — kein
// zusaetzlicher Abruf, nur eine andere Darstellung.
//
// bambuddy_api_copy_status() statt _take(): Das Frisch-Merkmal gehoert dem
// Status-Screen. Wuerde der Bildschirmschoner es verbrauchen, uebersaehe
// jener beim Aufwachen seine erste Aktualisierung.
static void refresh_printer(uint32_t *color_out, char *out, size_t out_len)
{
    *color_out = 0x9AA5AD;

    bambuddy_status_t st;
    if (!bambuddy_api_copy_status(&st)) {
        out[0] = '\0';
        return;
    }

    if (!st.printer_connected) {
        snprintf(out, out_len, "Drucker offline");
        return;
    }

    char rest[24];
    ui_format_duration(st.remaining_min * 60, rest, sizeof(rest));

    if (strcasecmp(st.state, "RUNNING") == 0) {
        *color_out = COL_OK;
        if (rest[0]) {
            snprintf(out, out_len, "Druckt  %d %%  noch %s", (int)(st.progress + 0.5f),
                     rest);
        } else {
            snprintf(out, out_len, "Druckt  %d %%", (int)(st.progress + 0.5f));
        }
    } else if (strcasecmp(st.state, "PREPARE") == 0) {
        *color_out = COL_ACCENT;
        snprintf(out, out_len, "Bereitet vor");
    } else if (strcasecmp(st.state, "PAUSE") == 0) {
        *color_out = COL_WARN;
        snprintf(out, out_len, "Pausiert  %d %%", (int)(st.progress + 0.5f));
    } else if (strcasecmp(st.state, "FINISH") == 0) {
        *color_out = COL_OK;
        snprintf(out, out_len, "Druck fertig");
    } else if (strcasecmp(st.state, "FAILED") == 0) {
        *color_out = COL_ERR;
        snprintf(out, out_len, "Druck fehlgeschlagen");
    } else if (st.state[0]) {
        // state ist ein freier String vom Drucker. Unbekanntes durchreichen
        // statt zu "Bereit" zu machen — sonst steht dort etwas Falsches.
        snprintf(out, out_len, "%s", strcasecmp(st.state, "IDLE") == 0 ? "Bereit"
                                                                      : st.state);
    } else {
        out[0] = '\0';
    }
}

static void refresh(bool force)
{
    if (!time_lbl || !date_lbl) return;

    const time_t now = time(nullptr);
    struct tm tm_now;
    localtime_r(&now, &tm_now);

    // Nur bei Minutenwechsel neu zeichnen. Jede Sekunde neu zu setzen wuerde
    // LVGL sechzigmal so oft schmutzig machen — und jede Neuzeichnung
    // konkurriert mit dem Panel um die Speicherbandbreite.
    if (!force && tm_now.tm_min == shown_minute) return;
    shown_minute = tm_now.tm_min;

    ui_set_text_fmt(time_lbl, "%02d:%02d", tm_now.tm_hour, tm_now.tm_min);
    ui_set_text_fmt(date_lbl, "%s, %d.%d.%d", WEEKDAYS[tm_now.tm_wday % 7],
                    tm_now.tm_mday, tm_now.tm_mon + 1, tm_now.tm_year + 1900);
}

// Getrennt von der Uhr und oefter: Ein fehlgeschlagener Druck soll nicht bis
// zum Minutenwechsel warten. Neu gezeichnet wird trotzdem nur, wenn sich der
// Text wirklich aendert — Prozent und Restzeit springen hoechstens ein paar
// Mal pro Minute.
static void refresh_printer_line()
{
    if (!printer_lbl) return;

    char text[64];
    uint32_t color;
    refresh_printer(&color, text, sizeof(text));

    ui_set_text(printer_lbl, text);
    ui_set_text_color(printer_lbl, color);
}

static void tick_cb(lv_timer_t *)
{
    refresh(false);
    refresh_printer_line();
}

static void build()
{
    overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(overlay, SCREEN_W, SCREEN_H);
    lv_obj_align(overlay, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(overlay, 0, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_pad_all(overlay, 0, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);

    // Bewusst nicht anklickbar: Die Weckflaeche der Abschaltung liegt
    // darueber und faengt die Beruehrung ab. Zwei Empfaenger fuer denselben
    // Tipp waeren zwei Gelegenheiten, ihn unterschiedlich zu behandeln.
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_CLICKABLE);

    time_lbl = lv_label_create(overlay);
    lv_obj_set_style_text_font(time_lbl, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(time_lbl, lv_color_hex(0xE0E0E0), 0);
    lv_obj_align(time_lbl, LV_ALIGN_CENTER, 0, -30);

    date_lbl = lv_label_create(overlay);
    lv_obj_set_style_text_font(date_lbl, &lv_font_montserrat_24, 0);
    // Heller als sonst: Aus zwei Metern Abstand und bei 30 %
    // Hintergrundbeleuchtung waere das uebliche Grau fuer Nebeninformation
    // kaum noch zu lesen.
    lv_obj_set_style_text_color(date_lbl, lv_color_hex(0x9AA5AD), 0);
    lv_obj_align(date_lbl, LV_ALIGN_CENTER, 0, 34);

    printer_lbl = lv_label_create(overlay);
    lv_obj_set_style_text_font(printer_lbl, &lv_font_montserrat_24, 0);
    lv_obj_set_width(printer_lbl, SCREEN_W - 40);
    lv_label_set_long_mode(printer_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(printer_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(printer_lbl, LV_ALIGN_CENTER, 0, 92);

    tick = lv_timer_create(tick_cb, 1000, nullptr);
    lv_timer_set_repeat_count(tick, -1);
}

void screensaver_show(bool visible)
{
    if (visible == (overlay != nullptr)) return;

    if (!visible) {
        if (tick) {
            lv_timer_delete(tick);
            tick = nullptr;
        }
        // Asynchron: Der Aufruf kommt aus dem Weck-Callback einer Flaeche,
        // die ueber diesem Overlay liegt.
        lv_obj_delete_async(overlay);
        overlay = nullptr;
        time_lbl = nullptr;
        date_lbl = nullptr;
        printer_lbl = nullptr;
        shown_minute = -1;
        return;
    }

    build();
    refresh(true);
    refresh_printer_line();
}

bool screensaver_visible()
{
    return overlay != nullptr;
}

bool screensaver_clock_available()
{
    return settings_time_synced();
}
