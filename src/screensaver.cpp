#include "screensaver.h"

#include <Arduino.h>
#include <lvgl.h>
#include <string.h>
#include <time.h>

#include "bambuddy_api.h"
#include "settings_screen.h"
#include "ui_layout.h"
#include "ui_theme.h"
#include "ui_util.h"

static lv_obj_t *overlay = nullptr;
static lv_timer_t *tick = nullptr;
static screensaver_mode_t current_mode = SCREENSAVER_OFF;

// ============================================================
// Uhr
// ============================================================

static lv_obj_t *time_lbl = nullptr;
static lv_obj_t *date_lbl = nullptr;
static lv_obj_t *printer_lbl = nullptr;
static int shown_minute = -1;

// Wochentage ausgeschrieben statt der englischen Kuerzel von strftime: Das
// Geraet spricht sonst ueberall Deutsch.
static const char *const WEEKDAYS[] = {"Sonntag",    "Montag",  "Dienstag", "Mittwoch",
                                       "Donnerstag", "Freitag", "Samstag"};

// Was macht der Drucker gerade? Genau die Zeile, fuer die man sonst hingeht
// und antippt. Die Daten liegen ohnehin im Speicher — kein zusaetzlicher
// Abruf, nur eine andere Darstellung.
//
// bambuddy_api_copy_status() statt _take(): Das Frisch-Merkmal gehoert dem
// Status-Screen. Wuerde der Bildschirmschoner es verbrauchen, uebersaehe
// jener beim Aufwachen seine erste Aktualisierung.
static void printer_summary(uint32_t *color_out, char *out, size_t out_len)
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
        snprintf(out, out_len, "%s",
                 strcasecmp(st.state, "IDLE") == 0 ? "Bereit" : st.state);
    } else {
        out[0] = '\0';
    }
}

static void clock_refresh_printer()
{
    if (!printer_lbl) return;

    char text[64];
    uint32_t color;
    printer_summary(&color, text, sizeof(text));

    ui_set_text(printer_lbl, text);
    ui_set_text_color(printer_lbl, color);
}

static void clock_refresh(bool force)
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

static void clock_build()
{
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

    shown_minute = -1;
    clock_refresh(true);
    clock_refresh_printer();
}

// ============================================================
// Matrix
// ============================================================
//
// Teuer ist hier nicht das Rechnen, sondern das Neuzeichnen: Der Bildpuffer
// liegt im PSRAM, aus dem das Panel gleichzeitig liest. Deshalb zeichnet
// dieser Schoner nicht Bild fuer Bild die ganze Flaeche, sondern haelt je
// Spalte zwei Beschriftungen und verschiebt nur diese — LVGL macht dann pro
// Schritt bloss die betroffenen Streifen schmutzig.
//
// Dazu laeuft jede Spalte in ihrem eigenen Takt: Bei jedem Tick ruehrt sich
// nur ein Teil von ihnen. Ein gleichzeitiger Schritt aller Spalten waere ein
// voller Bildaufbau — genau das, was auf diesem Board sichtbar zuckt.

static constexpr int MATRIX_COLS = 20;
static constexpr int MATRIX_COL_W = SCREEN_W / MATRIX_COLS; // 24
static constexpr int MATRIX_LINE_H = 28;                    // montserrat_24
static constexpr int MATRIX_ROWS = SCREEN_H / MATRIX_LINE_H + 1;
static constexpr int MATRIX_TRAIL_MIN = 5;
static constexpr int MATRIX_TRAIL_MAX = 12;
static constexpr uint32_t MATRIX_TICK_MS = 110;

// Katakana waere das Original, der eingebaute Zeichensatz kennt aber nur
// ASCII. Ziffern und Grossbuchstaben kommen dem Bild am naechsten.
static const char MATRIX_CHARS[] = "0123456789ABCDEFGHJKLMNPQRSTUVWXYZ<>*+=";
static constexpr int MATRIX_CHAR_COUNT = sizeof(MATRIX_CHARS) - 1;

struct matrix_col_t {
    lv_obj_t *trail;
    lv_obj_t *head;
    int16_t row;     // Zeile des hellen Kopfes, darf negativ sein
    uint8_t length;  // Laenge des Schweifs in Zeichen
    uint8_t period;  // alle wie viel Ticks ein Schritt?
    uint8_t counter;
    char text[2 * MATRIX_TRAIL_MAX]; // je Zeichen ein Zeilenumbruch
    char head_text[2];
};

// Die Beschriftungen zeigen auf diese Puffer, statt den Text zu kopieren.
//
// lv_label_set_text() ruft intern lv_realloc — bei zwanzig Spalten waeren
// das rund sechzig Neuanforderungen je Sekunde, und der Schoner laeuft
// womoeglich die ganze Nacht. lv_label_set_text_static() zeigt stattdessen
// auf einen Puffer, der uns gehoert: keine Allokation, keine Zerstueckelung
// des internen Speichers.
//
// Bedingung dafuer: Der Puffer muss leben, solange die Beschriftung lebt,
// und nach jeder Aenderung muss set_text_static() erneut gerufen werden —
// sonst weiss LVGL nicht, dass es neu messen und zeichnen soll.

static matrix_col_t columns[MATRIX_COLS];

static char matrix_random_char()
{
    return MATRIX_CHARS[random(MATRIX_CHAR_COUNT)];
}

// Spalte neu auswuerfeln. Sie startet oberhalb des Bildes, damit sie
// hereinlaeuft, statt aus dem Nichts zu erscheinen.
static void matrix_reset(matrix_col_t &c)
{
    c.length = MATRIX_TRAIL_MIN + random(MATRIX_TRAIL_MAX - MATRIX_TRAIL_MIN + 1);
    c.period = 1 + random(4);
    c.counter = random(c.period + 1);
    c.row = -(int16_t)random(MATRIX_ROWS);
}

static void matrix_write_trail(matrix_col_t &c)
{
    size_t n = 0;
    for (uint8_t i = 0; i < c.length && n + 2 < sizeof(c.text); i++) {
        if (i > 0) c.text[n++] = '\n';
        c.text[n++] = matrix_random_char();
    }
    c.text[n] = '\0';
    lv_label_set_text_static(c.trail, c.text);
}

static void matrix_step(matrix_col_t &c)
{
    c.row++;
    if (c.row - (int16_t)c.length > MATRIX_ROWS) matrix_reset(c);

    // Zeichen bei jedem Schritt neu wuerfeln — ein starrer Schweif sieht aus
    // wie ein herunterfallendes Wort, nicht wie Matrix.
    matrix_write_trail(c);

    // Der Schweif endet eine Zeile ueber dem Kopf.
    lv_obj_set_y(c.trail, (c.row - (int16_t)c.length) * MATRIX_LINE_H);
    lv_obj_set_y(c.head, c.row * MATRIX_LINE_H);

    c.head_text[0] = matrix_random_char();
    c.head_text[1] = '\0';
    lv_label_set_text_static(c.head, c.head_text);
}

static void matrix_build()
{
    for (int i = 0; i < MATRIX_COLS; i++) {
        matrix_col_t &c = columns[i];
        c = {};
        matrix_reset(c);

        c.trail = lv_label_create(overlay);
        lv_obj_set_style_text_font(c.trail, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(c.trail, lv_color_hex(0x1F8B3A), 0);
        lv_obj_set_style_text_line_space(c.trail, MATRIX_LINE_H - 24, 0);
        lv_obj_set_x(c.trail, i * MATRIX_COL_W + 4);

        // Der Kopf ist heller als der Schweif — das ist das, was die
        // Bewegungsrichtung ueberhaupt erkennbar macht.
        c.head = lv_label_create(overlay);
        lv_obj_set_style_text_font(c.head, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(c.head, lv_color_hex(0xD8FFD8), 0);
        lv_obj_set_x(c.head, i * MATRIX_COL_W + 4);

        matrix_step(c);
    }
}

static void matrix_tick()
{
    for (int i = 0; i < MATRIX_COLS; i++) {
        matrix_col_t &c = columns[i];
        if (++c.counter < c.period) continue;

        c.counter = 0;
        matrix_step(c);
    }
}

// ============================================================
// Gemeinsam
// ============================================================

static void tick_cb(lv_timer_t *)
{
    if (current_mode == SCREENSAVER_CLOCK) {
        clock_refresh(false);
        clock_refresh_printer();
    } else if (current_mode == SCREENSAVER_MATRIX) {
        matrix_tick();
    }
}

static void build(screensaver_mode_t mode)
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

    if (mode == SCREENSAVER_MATRIX) {
        matrix_build();
        tick = lv_timer_create(tick_cb, MATRIX_TICK_MS, nullptr);
    } else {
        clock_build();
        tick = lv_timer_create(tick_cb, 1000, nullptr);
    }
    lv_timer_set_repeat_count(tick, -1);
}

void screensaver_show(screensaver_mode_t mode)
{
    if (mode == current_mode) return;

    if (tick) {
        lv_timer_delete(tick);
        tick = nullptr;
    }
    if (overlay) {
        // Asynchron: Der Aufruf kommt aus dem Weck-Callback einer Flaeche,
        // die ueber diesem Overlay liegt.
        lv_obj_delete_async(overlay);
        overlay = nullptr;
    }

    time_lbl = nullptr;
    date_lbl = nullptr;
    printer_lbl = nullptr;
    memset(columns, 0, sizeof(columns));
    current_mode = mode;

    if (mode == SCREENSAVER_OFF) return;

    build(mode);
}

bool screensaver_visible()
{
    return overlay != nullptr;
}

bool screensaver_mode_available(screensaver_mode_t mode)
{
    if (mode == SCREENSAVER_CLOCK) return settings_time_synced();
    return mode != SCREENSAVER_OFF;
}
