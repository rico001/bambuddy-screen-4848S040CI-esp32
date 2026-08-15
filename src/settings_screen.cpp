#include "settings_screen.h"

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp32_smartdisplay.h>
#include <string.h>
#include <time.h>

#include "bambuddy_config.h"
#include "ota_service.h"
#include "screensaver.h"
#include "ui_layout.h"
#include "ui_theme.h"
#include "ui_font.h"

// ============================================================
// Layout
// ============================================================
static constexpr int PAD = 12;
static constexpr int HEADER_H = 48;
static constexpr int ROW_H = 66;        // Touch-Ziel inkl. Untertitel
static constexpr int SLIDER_ROW_H = 96; // Titel oben, Slider darunter
static constexpr int EDIT_KB_H = 220;


// ============================================================
// Auswahllisten
// ============================================================

// Abfrageintervall waehrend eines Drucks. Im Leerlauf wird das Fuenffache
// verwendet (gedeckelt), da sich dort minutenlang nichts aendert.
static const uint32_t poll_intervals_ms[] = {1000, 2000, 5000, 10000, 30000};
static constexpr int POLL_COUNT = 5;
static constexpr int POLL_DEFAULT = 1; // 2 Sekunden
static constexpr uint32_t POLL_IDLE_MAX_MS = 30000;
static const char *poll_options = "1 Sekunde\n2 Sekunden\n5 Sekunden\n10 Sekunden\n30 Sekunden";

struct timezone_entry_t {
    const char *label;
    const char *posix;
};

// POSIX-Zeitzonen inklusive Sommerzeitregel — die Umstellung passiert damit
// automatisch, ohne dass das Geraet etwas nachladen muss.
static const timezone_entry_t timezones[] = {
    {"Berlin", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"London", "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Athen", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"UTC", "UTC0"},
    {"New York", "EST5EDT,M3.2.0,M11.1.0"},
};
static constexpr int TZ_COUNT = 5;
static constexpr int TZ_DEFAULT = 0; // Berlin
static const char *tz_options = "Berlin\nLondon\nAthen\nUTC\nNew York";

// Bildschirmabschaltung. Standard sind fuenf Minuten: lange genug, dass das
// Display beim Vorbeigehen an bleibt, kurz genug, dass es nachts nicht die
// ganze Zeit leuchtet. 15 Sekunden vor dem Abschalten dunkelt es ab — so
// sieht man, dass gleich etwas passiert, und kann es antippen.
static const uint32_t screen_off_ms[] = {0, 30000, 60000, 300000, 600000};
static constexpr int SCREEN_OFF_COUNT = 5;
static constexpr int SCREEN_OFF_DEFAULT = 3; // 5 Minuten
static const char *screen_off_options =
    "Aus\n30 Sekunden\n1 Minute\n5 Minuten\n10 Minuten";

// Bildschirmschoner. "Aus" laesst das Display wie bisher dunkel werden,
// "Uhr" zeigt Uhrzeit und Druckerzustand, "Matrix" fallende Zeichen. Alles
// haengt an derselben
// Untaetigkeitsgrenze wie die Abschaltung — steht die auf "Aus", passiert
// gar nichts, und der Bildschirm bleibt dauerhaft an.
static constexpr int SAVER_COUNT = 3;
static constexpr int SAVER_DEFAULT = SCREENSAVER_OFF;
static const char *saver_options = "Aus\nUhr\nMatrix";

// Der Schoner leuchtet gedaempft: hell genug zum Ablesen aus dem Zimmer,
// dunkel genug, um nicht zu stoeren. Nachts noch einmal deutlich weniger —
// ein Wanddisplay, das um drei Uhr mit 30 Prozent ins Zimmer leuchtet, will
// niemand.
static constexpr float SAVER_BACKLIGHT = 0.30f;
static constexpr float SAVER_BACKLIGHT_NIGHT = 0.10f;
static constexpr int NIGHT_FROM_HOUR = 23;
static constexpr int NIGHT_TO_HOUR = 7;

// Zeit gilt als gueltig, wenn sie nach dem 15.11.2023 liegt —
// vorher steht die Uhr noch auf dem Startwert des Chips.
static constexpr time_t MIN_VALID_EPOCH = 1700000000;
static constexpr uint32_t NTP_RETRY_MS = 60000;

// ============================================================
// State
// ============================================================
static lv_obj_t *settings_list;
static lv_obj_t *dark_switch;
static lv_obj_t *tls_switch;
static lv_obj_t *guard_switch;
static lv_obj_t *log_start_switch;
static lv_obj_t *log_done_switch;
static lv_obj_t *log_error_switch;
static lv_obj_t *log_boot_switch;
static lv_obj_t *log_persist_switch;
static lv_obj_t *poll_dd;
static lv_obj_t *tz_dd;
static lv_obj_t *brightness_slider;
static lv_obj_t *brightness_value_lbl;
static lv_obj_t *time_row_lbl;
static lv_obj_t *ota_switch;
static lv_obj_t *ota_row_lbl;

// Dunkel ist Standard: Das Geraet haengt an der Wand, oft in einem Raum
// ohne Deckenlicht. Ein weisser 480x480-Bildschirm blendet dort.
static bool dark_mode = true;
static bool tls_verify = true;
// Standard: an. Ein versehentlicher Start am laufenden Drucker kostet im
// schlimmsten Fall ein Werkstueck und eine Duese — wer die Sperre nicht will,
// schaltet sie bewusst ab.
static bool start_guard = true;

// Protokollumfang. Alles an als Standard: Ein Protokoll, das man erst
// einschalten muss, ist beim ersten Bedarf leer.
static bool log_print_start = true;
static bool log_print_done = true;
static bool log_errors = true;
static bool log_boot = true;
static bool log_persist = true;

// Web-Update: aus als Standard. Eingeschaltet kann jeder im WLAN Firmware
// aufspielen — das soll eine bewusste Entscheidung sein, keine Werkseinstellung.
static bool ota_web = false;
static int brightness = 100;
static int poll_idx = POLL_DEFAULT;
static int tz_idx = TZ_DEFAULT;

// Vor dem Abschalten wird abgedunkelt: so sieht man es kommen und kann
// rechtzeitig tippen, statt ins Dunkle zu greifen.
static constexpr uint32_t DIM_LEAD_MS = 15000;
static constexpr float DIM_BACKLIGHT = 0.15f;

enum screen_level_t { SCREEN_ON, SCREEN_DIM, SCREEN_OFF };

static int screen_off_idx = SCREEN_OFF_DEFAULT;
static int saver_idx = SAVER_DEFAULT;
static screen_level_t screen_level = SCREEN_ON;
static lv_obj_t *sleep_catcher = nullptr;
static lv_timer_t *sleep_timer = nullptr;
static lv_obj_t *screen_off_dd;
static lv_obj_t *saver_dd;

static bool ntp_requested = false;
static uint32_t last_ntp_attempt_ms = 0;
static lv_timer_t *time_timer = nullptr;

static Preferences prefs;

// --- Textzeilen ---
#define MAX_TEXT_ROWS 16

struct text_row_t {
    const char *title;
    settings_get_fn get;
    settings_set_fn set;
    bool masked;
    bool numeric;
    lv_obj_t *value_lbl;
};

static text_row_t text_rows[MAX_TEXT_ROWS];
static int text_row_count = 0;

// --- Editor-Overlay ---
static lv_obj_t *edit_overlay = nullptr;
static lv_obj_t *edit_title;
static lv_obj_t *edit_ta;
static lv_obj_t *edit_kb;
static int edit_row_idx = -1;

// ============================================================
// Persistenz
// ============================================================

static void load_settings()
{
    prefs.begin("settings", true);
    dark_mode = prefs.getBool("dark", true);
    tls_verify = prefs.getBool("tls", true);
    start_guard = prefs.getBool("startguard", true);
    log_print_start = prefs.getBool("logstart", true);
    log_print_done = prefs.getBool("logdone", true);
    log_errors = prefs.getBool("logerr", true);
    log_boot = prefs.getBool("logboot", true);
    log_persist = prefs.getBool("logpersist", true);
    ota_web = prefs.getBool("otaweb", false);
    brightness = prefs.getInt("bright", 100);
    poll_idx = prefs.getInt("poll", POLL_DEFAULT);
    tz_idx = prefs.getInt("tz", TZ_DEFAULT);
    screen_off_idx = prefs.getInt("scroff", SCREEN_OFF_DEFAULT);
    saver_idx = prefs.getInt("saver", SAVER_DEFAULT);
    prefs.end();

    if (saver_idx < 0 || saver_idx >= SAVER_COUNT) saver_idx = SAVER_DEFAULT;

    if (screen_off_idx < 0 || screen_off_idx >= SCREEN_OFF_COUNT) {
        screen_off_idx = SCREEN_OFF_DEFAULT;
    }

    if (poll_idx < 0 || poll_idx >= POLL_COUNT) poll_idx = POLL_DEFAULT;
    if (tz_idx < 0 || tz_idx >= TZ_COUNT) tz_idx = TZ_DEFAULT;
    if (brightness < 0 || brightness > 100) brightness = 100;
}

static void save_settings()
{
    prefs.begin("settings", false);
    prefs.putBool("dark", dark_mode);
    prefs.putBool("tls", tls_verify);
    prefs.putBool("startguard", start_guard);
    prefs.putBool("logstart", log_print_start);
    prefs.putBool("logdone", log_print_done);
    prefs.putBool("logerr", log_errors);
    prefs.putBool("logboot", log_boot);
    prefs.putBool("logpersist", log_persist);
    prefs.putBool("otaweb", ota_web);
    prefs.putInt("bright", brightness);
    prefs.putInt("poll", poll_idx);
    prefs.putInt("tz", tz_idx);
    prefs.putInt("scroff", screen_off_idx);
    prefs.putInt("saver", saver_idx);
    prefs.end();
}

// ============================================================
// Theme / Helligkeit / Zeit
// ============================================================

// lv_theme_default_init() initialisiert die Theme-Styles an Ort und Stelle neu
// und ruft lv_obj_report_style_change(NULL) auf, sobald das Theme bereits am
// Display haengt. Dadurch faerben sich auch bestehende Objekte sofort um —
// ein Neustart ist nicht noetig. Ausnahme: fest gesetzte Farben in eigenem
// Code (Akzent-, Warn- und Fehlerfarben) bleiben wie sie sind.
static void apply_theme()
{
    lv_display_t *disp = lv_display_get_default();
    lv_theme_t *th = lv_theme_default_init(disp,
                                           lv_palette_main(LV_PALETTE_BLUE),
                                           lv_palette_main(LV_PALETTE_RED),
                                           dark_mode,
                                           LV_FONT_DEFAULT);
    lv_display_set_theme(disp, th);
}

// Regler 0-100 auf 20-100% echte Helligkeit abbilden: ganz dunkel waere ein
// scheinbar totes Geraet, aus dem man sich nicht mehr herausklicken kann.
static float brightness_to_backlight(int value)
{
    return (20.0f + value * 0.8f) / 100.0f;
}

// Setzt die Hintergrundbeleuchtung nur, wenn sie sich wirklich aendert. Der
// Schlafwaechter laeuft alle 500 ms und wuerde den Wert sonst zweimal pro
// Sekunde erneut an den Treiber schicken.
static float applied_backlight = -1.0f;

static void set_backlight(float value)
{
    if (value == applied_backlight) return;
    applied_backlight = value;
    smartdisplay_lcd_set_backlight(value);
}

static void apply_brightness()
{
    set_backlight(brightness_to_backlight(brightness));
}

// Helligkeit des Bildschirmschoners nach Tageszeit. Wird laufend nachgezogen,
// denn er laeuft womoeglich stundenlang und ueberschreitet dabei die Grenze.
static float saver_backlight()
{
    const time_t now = time(nullptr);
    struct tm tm_now;
    localtime_r(&now, &tm_now);

    const bool night = tm_now.tm_hour >= NIGHT_FROM_HOUR || tm_now.tm_hour < NIGHT_TO_HOUR;
    return night ? SAVER_BACKLIGHT_NIGHT : SAVER_BACKLIGHT;
}

// ============================================================
// Bildschirmabschaltung
// ============================================================

static void wake_cb(lv_event_t *);

// Im Schlaf legt sich eine unsichtbare Flaeche ueber alles. Sie verschluckt
// die Weckberuehrung — ohne sie wuerde der erste Tipp auf den dunklen
// Bildschirm blind auf dem darunterliegenden Screen landen, und das kann im
// schlimmsten Fall einen Druck stoppen. Beim blossen Abdunkeln bleibt sie
// weg: da ist noch alles lesbar, und Tippen soll normal wirken.
static void set_catcher(bool active)
{
    if (active && !sleep_catcher) {
        sleep_catcher = lv_obj_create(lv_layer_top());
        lv_obj_set_size(sleep_catcher, SCREEN_W, SCREEN_H);
        lv_obj_align(sleep_catcher, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_set_style_bg_opa(sleep_catcher, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(sleep_catcher, 0, 0);
        lv_obj_set_style_radius(sleep_catcher, 0, 0);
        lv_obj_remove_flag(sleep_catcher, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(sleep_catcher, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(sleep_catcher, wake_cb, LV_EVENT_PRESSED, nullptr);
    }
    if (!sleep_catcher) return;

    if (active) {
        lv_obj_remove_flag(sleep_catcher, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(sleep_catcher);
    } else {
        lv_obj_add_flag(sleep_catcher, LV_OBJ_FLAG_HIDDEN);
    }
}

static void set_screen_level(screen_level_t level)
{
    if (level == screen_level) return;
    screen_level = level;

    switch (level) {
    case SCREEN_OFF:
        // Mit Bildschirmschoner wird nicht abgeschaltet, sondern umgeschaltet.
        // Ohne gestellte Uhr bleibt es bei der Abschaltung — eine erfundene
        // Uhrzeit gross ins Zimmer zu leuchten waere schlimmer als ein
        // dunkler Bildschirm.
        if (screensaver_mode_available((screensaver_mode_t)saver_idx)) {
            screensaver_show((screensaver_mode_t)saver_idx);
            set_catcher(true); // Weckflaeche ueber den Schoner legen
            set_backlight(saver_backlight());
            break;
        }
        screensaver_show(SCREENSAVER_OFF);
        set_catcher(true);
        set_backlight(0.0f);
        break;
    case SCREEN_DIM:
        screensaver_show(SCREENSAVER_OFF);
        set_catcher(false);
        set_backlight(DIM_BACKLIGHT);
        break;
    case SCREEN_ON:
    default:
        screensaver_show(SCREENSAVER_OFF);
        set_catcher(false);
        set_backlight(brightness_to_backlight(brightness));
        break;
    }
}

static void wake_cb(lv_event_t *)
{
    set_screen_level(SCREEN_ON);
}

void settings_screen_wake()
{
    // Nicht nur einschalten, sondern auch die Untaetigkeitsuhr anstossen:
    // Ohne das haette der Schlafwaechter 500 ms spaeter wieder abgedunkelt,
    // weil er allein die Zeit seit der letzten Beruehrung kennt. Zugleich
    // laeuft die Abschaltzeit dadurch neu — der Bildschirm bleibt so lange
    // an, wie er es nach einer Beruehrung auch bliebe.
    lv_display_trigger_activity(nullptr);
    set_screen_level(SCREEN_ON);
}

static void sleep_check_cb(lv_timer_t *)
{
    const uint32_t timeout = screen_off_ms[screen_off_idx];
    if (timeout == 0) {
        set_screen_level(SCREEN_ON); // Einstellung im Schlaf abgeschaltet
        return;
    }

    // LVGL zaehlt die Zeit seit der letzten Eingabe selbst mit — dadurch
    // zaehlt jede Beruehrung auf jedem Screen, ohne dass die Screens etwas
    // davon wissen muessen.
    const uint32_t idle = lv_display_get_inactive_time(nullptr);
    const uint32_t dim_at = timeout > DIM_LEAD_MS ? timeout - DIM_LEAD_MS : timeout / 2;

    if (idle >= timeout) {
        set_screen_level(SCREEN_OFF);
        // Der Schoner laeuft womoeglich stundenlang. set_screen_level()
        // greift nur beim Wechsel, die Nachtabsenkung braucht aber einen
        // Blick auf die Uhrzeit — der gehoert hierher.
        if (screensaver_visible()) set_backlight(saver_backlight());
    } else if (idle >= dim_at) {
        set_screen_level(SCREEN_DIM);
    } else {
        set_screen_level(SCREEN_ON);
    }
}

static void apply_timezone()
{
    setenv("TZ", timezones[tz_idx].posix, 1);
    tzset();
    ntp_requested = false; // Sync mit neuer Zone erneut anstossen
}

// ============================================================
// Zeilen-Bausteine
// ============================================================

lv_obj_t *settings_add_section(const char *title)
{
    lv_obj_t *lbl = lv_label_create(settings_list);
    lv_label_set_text(lbl, title);
    lv_obj_set_width(lbl, LV_PCT(100));
    lv_obj_set_style_text_font(lbl, &bb_font_12, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(COL_MUTED), 0);
    lv_obj_set_style_pad_top(lbl, 6, 0);
    lv_obj_set_style_pad_left(lbl, 4, 0);
    return lbl;
}

static lv_obj_t *row_base(int height)
{
    lv_obj_t *row = lv_obj_create(settings_list);
    lv_obj_set_size(row, LV_PCT(100), height);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(row, 10, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_hor(row, 14, 0);
    lv_obj_set_style_pad_ver(row, 0, 0);
    return row;
}

// Gemeinsamer Zeilenaufbau. sub_lbl_out liefert das Untertitel-Label zurueck,
// damit Zeilen ihren Wert spaeter aktualisieren koennen.
static lv_obj_t *row_create(const char *icon, const char *title, const char *subtitle,
                            lv_obj_t **sub_lbl_out)
{
    lv_obj_t *row = row_base(ROW_H);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    if (icon) {
        lv_obj_t *ic = lv_label_create(row);
        lv_label_set_text(ic, icon);
        lv_obj_set_style_pad_right(ic, 14, 0);
    }

    // Textspalte: waechst, damit das Bedienelement rechts buendig sitzt
    lv_obj_t *text_col = lv_obj_create(row);
    lv_obj_set_height(text_col, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(text_col, 1);
    lv_obj_remove_flag(text_col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(text_col, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(text_col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(text_col, 0, 0);
    lv_obj_set_style_pad_all(text_col, 0, 0);
    lv_obj_set_flex_flow(text_col, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *title_lbl = lv_label_create(text_col);
    lv_label_set_text(title_lbl, title);
    lv_obj_set_width(title_lbl, LV_PCT(100));
    lv_label_set_long_mode(title_lbl, LV_LABEL_LONG_DOT);

    lv_obj_t *sub_lbl = nullptr;
    if (subtitle) {
        sub_lbl = lv_label_create(text_col);
        lv_label_set_text(sub_lbl, subtitle);
        lv_obj_set_width(sub_lbl, LV_PCT(100));
        lv_label_set_long_mode(sub_lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(sub_lbl, &bb_font_12, 0);
        lv_obj_set_style_text_color(sub_lbl, lv_color_hex(COL_MUTED), 0);
        lv_obj_set_style_pad_top(sub_lbl, 2, 0);
    }

    if (sub_lbl_out) *sub_lbl_out = sub_lbl;
    return row;
}

lv_obj_t *settings_add_row(const char *icon, const char *title, const char *subtitle)
{
    return row_create(icon, title, subtitle, nullptr);
}

lv_obj_t *settings_add_slider_row(const char *icon, const char *title,
                                  int32_t min, int32_t max, int32_t value,
                                  lv_event_cb_t cb, lv_obj_t **value_lbl_out)
{
    lv_obj_t *row = row_base(SLIDER_ROW_H);

    lv_obj_t *icon_lbl = nullptr;
    if (icon) {
        icon_lbl = lv_label_create(row);
        lv_label_set_text(icon_lbl, icon);
        lv_obj_align(icon_lbl, LV_ALIGN_TOP_LEFT, 0, 16);
    }

    lv_obj_t *title_lbl = lv_label_create(row);
    lv_label_set_text(title_lbl, title);
    lv_obj_align(title_lbl, LV_ALIGN_TOP_LEFT, icon ? 34 : 0, 16);

    lv_obj_t *value_lbl = lv_label_create(row);
    lv_obj_set_style_text_color(value_lbl, lv_color_hex(COL_MUTED), 0);
    lv_obj_align(value_lbl, LV_ALIGN_TOP_RIGHT, 0, 16);

    // Slider ueber die volle Zeilenbreite — auf einem Touchscreen ist ein
    // schmaler Regler neben dem Text kaum zu treffen.
    lv_obj_t *slider = lv_slider_create(row);
    lv_slider_set_range(slider, min, max);
    lv_slider_set_value(slider, value, LV_ANIM_OFF);
    lv_obj_set_width(slider, LV_PCT(100));
    lv_obj_align(slider, LV_ALIGN_BOTTOM_MID, 0, -18);
    lv_obj_add_event_cb(slider, cb, LV_EVENT_VALUE_CHANGED, nullptr);

    if (value_lbl_out) *value_lbl_out = value_lbl;
    return slider;
}

// ============================================================
// Textzeilen + Editor
// ============================================================

// Keys und Tokens nur angedeutet anzeigen — lang genug zum Wiedererkennen,
// kurz genug, dass niemand sie vom Display abschreibt.
static void format_value(const text_row_t &row, char *out, size_t out_len)
{
    const char *value = row.get ? row.get() : "";

    if (!value || value[0] == '\0') {
        strncpy(out, "nicht gesetzt", out_len - 1);
        out[out_len - 1] = '\0';
        return;
    }

    const size_t len = strlen(value);
    if (row.masked && len > 14) {
        snprintf(out, out_len, "%.6s ... %s", value, value + len - 4);
    } else {
        strncpy(out, value, out_len - 1);
        out[out_len - 1] = '\0';
    }
}

static void refresh_value_label(int idx)
{
    if (idx < 0 || idx >= text_row_count) return;

    text_row_t &row = text_rows[idx];
    char buf[64];
    format_value(row, buf, sizeof(buf));
    lv_label_set_text(row.value_lbl, buf);

    const char *value = row.get ? row.get() : "";
    const bool empty = (value == nullptr || value[0] == '\0');
    lv_obj_set_style_text_color(row.value_lbl,
                                lv_color_hex(empty ? COL_WARN : COL_MUTED), 0);
}

static void edit_close()
{
    edit_row_idx = -1;
    lv_obj_add_flag(edit_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void edit_save_cb(lv_event_t *)
{
    if (edit_row_idx < 0) return;

    const int idx = edit_row_idx;
    text_row_t &row = text_rows[idx];
    if (row.set) row.set(lv_textarea_get_text(edit_ta));

    edit_close();
    refresh_value_label(idx);
}

static void edit_cancel_cb(lv_event_t *)
{
    edit_close();
}

static void edit_ta_cb(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {
        edit_save_cb(e); // Haken auf der Tastatur = speichern
    } else if (code == LV_EVENT_CANCEL) {
        edit_close();
    }
}

// Overlay liegt auf lv_layer_top: immer ueber allen Screens, unabhaengig
// davon, welche Tileview-Kachel gerade sichtbar ist.
static void edit_overlay_create()
{
    edit_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(edit_overlay, SCREEN_W, SCREEN_H);
    lv_obj_align(edit_overlay, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_remove_flag(edit_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(edit_overlay, 0, 0);
    lv_obj_set_style_border_width(edit_overlay, 0, 0);
    lv_obj_set_style_pad_all(edit_overlay, 0, 0);
    lv_obj_add_flag(edit_overlay, LV_OBJ_FLAG_HIDDEN);

    edit_title = lv_label_create(edit_overlay);
    lv_obj_set_style_text_font(edit_title, &bb_font_16, 0);
    lv_obj_set_width(edit_title, SCREEN_W - 2 * PAD);
    lv_label_set_long_mode(edit_title, LV_LABEL_LONG_DOT);
    lv_obj_align(edit_title, LV_ALIGN_TOP_LEFT, PAD + 4, 18);

    edit_ta = lv_textarea_create(edit_overlay);
    lv_textarea_set_one_line(edit_ta, true);
    lv_obj_set_size(edit_ta, SCREEN_W - 2 * PAD, 52);
    lv_obj_align(edit_ta, LV_ALIGN_TOP_MID, 0, 56);
    lv_obj_add_event_cb(edit_ta, edit_ta_cb, LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(edit_ta, edit_ta_cb, LV_EVENT_CANCEL, nullptr);

    lv_obj_t *hint = lv_label_create(edit_overlay);
    lv_label_set_text(hint, "Wird sofort gespeichert.");
    lv_obj_set_style_text_font(hint, &bb_font_12, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(COL_MUTED), 0);
    lv_obj_align(hint, LV_ALIGN_TOP_LEFT, PAD + 4, 116);

    lv_obj_t *cancel_btn = lv_button_create(edit_overlay);
    lv_obj_set_size(cancel_btn, 150, 46);
    lv_obj_align(cancel_btn, LV_ALIGN_TOP_LEFT, PAD, 146);
    lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(COL_NEUTRAL), 0);
    lv_obj_add_event_cb(cancel_btn, edit_cancel_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *cancel_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_lbl, LV_SYMBOL_CLOSE "  Abbrechen");
    lv_obj_center(cancel_lbl);

    lv_obj_t *save_btn = lv_button_create(edit_overlay);
    lv_obj_set_size(save_btn, SCREEN_W - 2 * PAD - 160, 46);
    lv_obj_align(save_btn, LV_ALIGN_TOP_RIGHT, -PAD, 146);
    lv_obj_set_style_bg_color(save_btn, lv_color_hex(COL_ACCENT), 0);
    lv_obj_add_event_cb(save_btn, edit_save_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *save_lbl = lv_label_create(save_btn);
    lv_label_set_text(save_lbl, LV_SYMBOL_SAVE "  Speichern");
    lv_obj_center(save_lbl);

    edit_kb = lv_keyboard_create(edit_overlay);
    lv_obj_set_size(edit_kb, SCREEN_W, EDIT_KB_H);
    lv_obj_align(edit_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(edit_kb, edit_ta);
}

static void edit_open(int idx)
{
    if (idx < 0 || idx >= text_row_count) return;
    if (!edit_overlay) edit_overlay_create();

    edit_row_idx = idx;
    const text_row_t &row = text_rows[idx];

    lv_label_set_text(edit_title, row.title);
    lv_keyboard_set_mode(edit_kb, row.numeric ? LV_KEYBOARD_MODE_NUMBER
                                              : LV_KEYBOARD_MODE_TEXT_LOWER);

    // Beim Bearbeiten immer im Klartext — sonst kann niemand pruefen,
    // ob der Key richtig angekommen ist.
    lv_textarea_set_text(edit_ta, row.get ? row.get() : "");

    lv_obj_remove_flag(edit_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(edit_overlay);
}

static void text_row_cb(lv_event_t *e)
{
    edit_open((int)(intptr_t)lv_event_get_user_data(e));
}

lv_obj_t *settings_add_text_row(const char *icon, const char *title,
                                settings_get_fn get, settings_set_fn set,
                                bool masked, bool numeric)
{
    if (text_row_count >= MAX_TEXT_ROWS) return nullptr;

    const int idx = text_row_count++;
    text_row_t &entry = text_rows[idx];
    entry.title = title;
    entry.get = get;
    entry.set = set;
    entry.masked = masked;
    entry.numeric = numeric;

    // Der Wert steht als Untertitel unter dem Namen — mehr Platz als eine
    // rechtsbuendige Spalte, und lange URLs bleiben lesbar.
    lv_obj_t *row = row_create(icon, title, "", &entry.value_lbl);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(row, lv_color_hex(COL_ACCENT), LV_STATE_PRESSED);
    lv_obj_add_event_cb(row, text_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);

    // Chevron signalisiert: hier oeffnet sich etwas
    lv_obj_t *chevron = lv_label_create(row);
    lv_label_set_text(chevron, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(chevron, lv_color_hex(COL_MUTED), 0);
    lv_obj_set_style_pad_left(chevron, 10, 0);

    refresh_value_label(idx);
    return row;
}

// ============================================================
// Web-Update
// ============================================================

// Die Zeile sagt, wo der Dienst zu erreichen ist — eine Adresse, die man erst
// im Router suchen muss, waere die halbe Auskunft. Waehrend eines Uploads
// steht stattdessen der Fortschritt dort: Der Browser zeigt zwar seinen
// eigenen Balken, aber wer vor dem Geraet steht, sieht sonst nur ein
// flackerndes Display und weiss nicht, ob das gewollt ist.
static void refresh_ota_row()
{
    if (!ota_row_lbl) return;

    const int pct = ota_service_progress();
    if (pct >= 0) {
        lv_label_set_text_fmt(ota_row_lbl, "Update läuft: %d %%", pct);
        lv_obj_set_style_text_color(ota_row_lbl, lv_color_hex(COL_ACCENT), 0);
        return;
    }

    if (!ota_service_enabled()) {
        lv_label_set_text(ota_row_lbl, "Firmware im Browser aufspielen");
        lv_obj_set_style_text_color(ota_row_lbl, lv_color_hex(COL_MUTED), 0);
        return;
    }

    if (ota_service_online()) {
        lv_label_set_text(ota_row_lbl, ota_service_address());
        lv_obj_set_style_text_color(ota_row_lbl, lv_color_hex(COL_OK), 0);
        return;
    }

    const char *msg = ota_service_message();
    lv_label_set_text(ota_row_lbl, msg[0] ? msg : "Wartet auf WLAN");
    lv_obj_set_style_text_color(ota_row_lbl, lv_color_hex(COL_MUTED), 0);
}

// ============================================================
// NTP
// ============================================================

static void time_tick_cb(lv_timer_t *)
{
    refresh_ota_row();

    const bool online = (WiFi.status() == WL_CONNECTED);
    const time_t now = time(nullptr);
    const bool valid = now >= MIN_VALID_EPOCH;

    if (online && !valid) {
        const uint32_t ms = millis();
        if (!ntp_requested || (ms - last_ntp_attempt_ms) >= NTP_RETRY_MS) {
            configTzTime(timezones[tz_idx].posix,
                         "de.pool.ntp.org", "pool.ntp.org", "time.nist.gov");
            ntp_requested = true;
            last_ntp_attempt_ms = ms;
            Serial.println("[Settings] NTP-Sync angefordert");
        }
    }

    if (!time_row_lbl) return;

    if (valid) {
        struct tm local_tm;
        localtime_r(&now, &local_tm);
        char buf[48];
        strftime(buf, sizeof(buf), "%d.%m.%Y  %H:%M:%S", &local_tm);
        lv_label_set_text(time_row_lbl, buf);
        lv_obj_set_style_text_color(time_row_lbl, lv_color_hex(COL_OK), 0);
    } else {
        lv_label_set_text(time_row_lbl, online ? "Synchronisiere ..." : "Wartet auf WLAN");
        lv_obj_set_style_text_color(time_row_lbl, lv_color_hex(COL_MUTED), 0);
    }
}

// Uhr und Bildschirmabschaltung laufen unabhaengig davon, ob der
// Einstellungs-Screen jemals gebaut wurde — beide gehoeren zum Geraet,
// nicht zu einer Kachel.
static void start_background_services()
{
    if (!sleep_timer) {
        sleep_timer = lv_timer_create(sleep_check_cb, 500, nullptr);
        lv_timer_set_repeat_count(sleep_timer, -1);
    }

    if (!time_timer) {
        time_timer = lv_timer_create(time_tick_cb, 1000, nullptr);
        lv_timer_set_repeat_count(time_timer, -1);
    }
    time_tick_cb(nullptr);
}

// ============================================================
// Callbacks
// ============================================================

static void dark_switch_cb(lv_event_t *)
{
    dark_mode = lv_obj_has_state(dark_switch, LV_STATE_CHECKED);
    apply_theme();
    save_settings();
}

static void tls_switch_cb(lv_event_t *)
{
    tls_verify = lv_obj_has_state(tls_switch, LV_STATE_CHECKED);
    save_settings();
    Serial.printf("[Settings] TLS-Pruefung: %s\n", tls_verify ? "an" : "AUS");
}

static void brightness_cb(lv_event_t *)
{
    brightness = lv_slider_get_value(brightness_slider);
    screen_level = SCREEN_ON; // Regler bedienen heisst: Bildschirm ist wach
    apply_brightness();
    lv_label_set_text_fmt(brightness_value_lbl, "%d%%", 20 + brightness * 80 / 100);
    save_settings();
}

static void poll_dd_cb(lv_event_t *)
{
    poll_idx = lv_dropdown_get_selected(poll_dd);
    save_settings();
}

static void saver_dd_cb(lv_event_t *)
{
    saver_idx = (int)lv_dropdown_get_selected(saver_dd);
    save_settings();

    // Laeuft gerade ein Schoner und die Wahl aendert sich, sofort umstellen —
    // sonst bliebe der alte stehen, bis das naechste Mal geweckt wird.
    if (screensaver_visible()) {
        screensaver_show(screensaver_mode_available((screensaver_mode_t)saver_idx)
                             ? (screensaver_mode_t)saver_idx
                             : SCREENSAVER_OFF);
    }
}

static void guard_switch_cb(lv_event_t *)
{
    start_guard = lv_obj_has_state(guard_switch, LV_STATE_CHECKED);
    save_settings();
}

// Ein Rueckruf fuer alle vier: Welches Ziel gemeint ist, steht in den
// Nutzdaten. Vier fast gleiche Funktionen waeren vier Gelegenheiten, das
// Speichern in einer davon zu vergessen.
static void log_switch_cb(lv_event_t *e)
{
    bool *target = (bool *)lv_event_get_user_data(e);
    lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
    if (!target || !sw) return;

    *target = lv_obj_has_state(sw, LV_STATE_CHECKED);
    save_settings();
}

static void ota_switch_cb(lv_event_t *)
{
    ota_web = lv_obj_has_state(ota_switch, LV_STATE_CHECKED);
    save_settings();
    ota_service_set_enabled(ota_web);
    refresh_ota_row(); // nicht bis zum naechsten Sekundentakt warten
}

static void screen_off_dd_cb(lv_event_t *)
{
    screen_off_idx = lv_dropdown_get_selected(screen_off_dd);
    set_screen_level(SCREEN_ON);
    save_settings();
}

static void tz_dd_cb(lv_event_t *)
{
    tz_idx = lv_dropdown_get_selected(tz_dd);
    apply_timezone();
    save_settings();
    time_tick_cb(nullptr); // Anzeige sofort aktualisieren
}

// Zahlen als Text fuer die Textzeilen
static const char *printer_id_get()
{
    static char buf[8];
    snprintf(buf, sizeof(buf), "%d", bambuddy_printer_id());
    return buf;
}

static const char *mqtt_port_get()
{
    static char buf[8];
    snprintf(buf, sizeof(buf), "%d", bambuddy_mqtt_port());
    return buf;
}

static void source_switch_cb(lv_event_t *e)
{
    lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
    const bool use_mqtt = lv_obj_has_state(sw, LV_STATE_CHECKED);
    bambuddy_set_source_mqtt(use_mqtt);
    Serial.printf("[Settings] Statusquelle: %s\n", use_mqtt ? "MQTT" : "HTTP");
}

// ============================================================
// Public API
// ============================================================

void settings_apply_saved()
{
    load_settings();
    apply_theme();
    apply_brightness();
    apply_timezone();
    // Uhr und Bildschirmabschaltung gehoeren zum Geraet und duerfen nicht
    // vom spaeter nur bei Bedarf erzeugten Einstellungs-Screen abhaengen.
    start_background_services();

    // Ebenso das Web-Update: Es muss auch dann laufen, wenn der
    // Einstellungs-Screen seit dem Start nie geoeffnet wurde — sonst waere der
    // Schalter nach jedem Neustart wirkungslos, bis jemand nachsieht.
    ota_service_set_enabled(ota_web);
}

void settings_screen_create(lv_obj_t *parent)
{
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    // Scrollbare Liste — neue Einstellungen haengen sich unten an,
    // ohne dass Positionen angepasst werden muessen.
    settings_list = lv_obj_create(parent);
    lv_obj_set_size(settings_list, SCREEN_W - 2 * PAD, SCREEN_H - HEADER_H - PAD);
    lv_obj_align(settings_list, LV_ALIGN_TOP_MID, 0, HEADER_H);
    lv_obj_set_style_bg_opa(settings_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(settings_list, 0, 0);
    lv_obj_set_style_pad_all(settings_list, 0, 0);
    lv_obj_set_style_pad_row(settings_list, 8, 0);
    lv_obj_set_flex_flow(settings_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(settings_list, LV_DIR_VER);

    // --- Darstellung ---
    //
    // Steht bewusst ganz oben: Helligkeit, Dark Mode und Bildschirmschoner
    // fasst man im Alltag am haeufigsten an. Server-URL und Schluessel
    // dagegen einmal beim Einrichten und danach nie wieder.
    settings_add_section("DARSTELLUNG");

    lv_obj_t *dark_row = settings_add_row(LV_SYMBOL_IMAGE, "Dark Mode",
                                          "Dunkles Farbschema für alle Screens");
    dark_switch = lv_switch_create(dark_row);
    lv_obj_set_size(dark_switch, 58, 32);
    if (dark_mode) lv_obj_add_state(dark_switch, LV_STATE_CHECKED);
    lv_obj_add_event_cb(dark_switch, dark_switch_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    lv_obj_t *off_row = settings_add_row(LV_SYMBOL_POWER, "Bildschirm aus",
                                         "Nach Untätigkeit abschalten");
    screen_off_dd = lv_dropdown_create(off_row);
    lv_dropdown_set_options(screen_off_dd, screen_off_options);
    lv_dropdown_set_selected(screen_off_dd, screen_off_idx);
    lv_obj_set_width(screen_off_dd, 150);
    lv_obj_add_event_cb(screen_off_dd, screen_off_dd_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    lv_obj_t *saver_row = settings_add_row(LV_SYMBOL_EYE_OPEN, "Bildschirmschoner",
                                           "Statt abschalten die Uhr zeigen");
    saver_dd = lv_dropdown_create(saver_row);
    lv_dropdown_set_options(saver_dd, saver_options);
    lv_dropdown_set_selected(saver_dd, saver_idx);
    lv_obj_set_width(saver_dd, 150);
    lv_obj_add_event_cb(saver_dd, saver_dd_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    brightness_slider = settings_add_slider_row(LV_SYMBOL_SETTINGS, "Helligkeit",
                                                0, 100, brightness,
                                                brightness_cb, &brightness_value_lbl);
    lv_label_set_text_fmt(brightness_value_lbl, "%d%%", 20 + brightness * 80 / 100);

    // --- Bambuddy-Verbindung ---
    settings_add_section("BAMBUDDY");

    settings_add_text_row(LV_SYMBOL_HOME, "Server-URL",
                          bambuddy_configured_base_url, bambuddy_set_base_url, false,
                          false);

    settings_add_text_row(LV_SYMBOL_EYE_CLOSE, "API-Key",
                          bambuddy_api_key, bambuddy_set_api_key, true, false);

    settings_add_text_row(LV_SYMBOL_LIST, "Drucker-ID",
                          printer_id_get, bambuddy_set_printer_id, false, true);

    settings_add_text_row(LV_SYMBOL_VIDEO, "Kamera-Token",
                          bambuddy_cam_token, bambuddy_set_cam_token, true, false);

    lv_obj_t *poll_row = settings_add_row(LV_SYMBOL_REFRESH, "Aktualisierung",
                                          "Im Leerlauf automatisch seltener");
    poll_dd = lv_dropdown_create(poll_row);
    lv_dropdown_set_options(poll_dd, poll_options);
    lv_dropdown_set_selected(poll_dd, poll_idx);
    lv_obj_set_width(poll_dd, 150);
    lv_obj_add_event_cb(poll_dd, poll_dd_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    lv_obj_t *tls_row = settings_add_row(LV_SYMBOL_WARNING, "Zertifikat prüfen",
                                         "Nur zum Debuggen abschalten");
    tls_switch = lv_switch_create(tls_row);
    lv_obj_set_size(tls_switch, 58, 32);
    if (tls_verify) lv_obj_add_state(tls_switch, LV_STATE_CHECKED);
    lv_obj_add_event_cb(tls_switch, tls_switch_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    lv_obj_t *guard_row =
        settings_add_row(LV_SYMBOL_WARNING, "Druckstart blockieren",
                         "Start des Drucks blockieren, wenn Drucker beschäftigt.");
    guard_switch = lv_switch_create(guard_row);
    lv_obj_set_size(guard_switch, 58, 32);
    if (start_guard) lv_obj_add_state(guard_switch, LV_STATE_CHECKED);
    lv_obj_add_event_cb(guard_switch, guard_switch_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    // --- MQTT ---
    settings_add_section("MQTT");

    lv_obj_t *source_row = settings_add_row(LV_SYMBOL_SHUFFLE, "Status per MQTT",
                                            "Aus: Status wird per HTTP abgefragt");
    lv_obj_t *source_switch = lv_switch_create(source_row);
    lv_obj_set_size(source_switch, 58, 32);
    if (bambuddy_source_mqtt()) lv_obj_add_state(source_switch, LV_STATE_CHECKED);
    lv_obj_add_event_cb(source_switch, source_switch_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    settings_add_text_row(LV_SYMBOL_WIFI, "Broker",
                          bambuddy_mqtt_host, bambuddy_set_mqtt_host, false, false);

    settings_add_text_row(LV_SYMBOL_DOWNLOAD, "Port",
                          mqtt_port_get, bambuddy_set_mqtt_port, false, true);

    settings_add_text_row(LV_SYMBOL_USB, "Benutzer",
                          bambuddy_mqtt_user, bambuddy_set_mqtt_user, false, false);

    settings_add_text_row(LV_SYMBOL_EYE_CLOSE, "Passwort",
                          bambuddy_mqtt_pass, bambuddy_set_mqtt_pass, true, false);

    settings_add_text_row(LV_SYMBOL_LIST, "Status-Topic",
                          bambuddy_mqtt_topic, bambuddy_set_mqtt_topic, false, false);

    // --- Protokoll ---
    settings_add_section("MELDUNGEN");

    struct log_row_t {
        const char *icon;
        const char *title;
        const char *subtitle;
        bool *value;
        lv_obj_t **handle;
    };

    const log_row_t log_rows[] = {
        {LV_SYMBOL_PLAY, "Druckstart", "Beginn eines Auftrags vermerken",
         &log_print_start, &log_start_switch},
        {LV_SYMBOL_OK, "Druck fertig", "Fertige Drucke vermerken", &log_print_done,
         &log_done_switch},
        {LV_SYMBOL_WARNING, "Fehler", "Druckerfehler und Abbrüche vermerken",
         &log_errors, &log_error_switch},
        {LV_SYMBOL_POWER, "Systemstart", "Neustarts des Displays vermerken", &log_boot,
         &log_boot_switch},
        {LV_SYMBOL_SAVE, "Dauerhaft speichern", "Aus: kein Flimmern, aber weg nach Neustart",
         &log_persist, &log_persist_switch},
    };

    for (const log_row_t &row : log_rows) {
        lv_obj_t *parent_row = settings_add_row(row.icon, row.title, row.subtitle);
        lv_obj_t *sw = lv_switch_create(parent_row);
        lv_obj_set_size(sw, 58, 32);
        if (*row.value) lv_obj_add_state(sw, LV_STATE_CHECKED);
        lv_obj_add_event_cb(sw, log_switch_cb, LV_EVENT_VALUE_CHANGED, row.value);
        *row.handle = sw;
    }

    // --- Zeit ---
    settings_add_section("ZEIT");

    lv_obj_t *tz_row = settings_add_row(LV_SYMBOL_GPS, "Zeitzone",
                                        "Sommerzeit wird automatisch umgestellt");
    tz_dd = lv_dropdown_create(tz_row);
    lv_dropdown_set_options(tz_dd, tz_options);
    lv_dropdown_set_selected(tz_dd, tz_idx);
    lv_obj_set_width(tz_dd, 150);
    lv_obj_add_event_cb(tz_dd, tz_dd_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    // Untertitel dieser Zeile dient als Sync-Statusanzeige
    row_create(LV_SYMBOL_BELL, "Uhrzeit", "", &time_row_lbl);

    // --- Firmware ---
    //
    // Ganz unten: Ein Schalter, der einen Upload-Weg ins Geraet oeffnet, soll
    // nicht neben der Helligkeit liegen, wo man ihn im Vorbeiwischen trifft.
    settings_add_section("FIRMWARE");

    lv_obj_t *ota_row = row_create(LV_SYMBOL_DOWNLOAD, "Web-Update", "", &ota_row_lbl);
    ota_switch = lv_switch_create(ota_row);
    lv_obj_set_size(ota_switch, 58, 32);
    if (ota_web) lv_obj_add_state(ota_switch, LV_STATE_CHECKED);
    lv_obj_add_event_cb(ota_switch, ota_switch_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    time_tick_cb(nullptr);
}

void settings_screen_destroy()
{
    if (edit_overlay) {
        lv_obj_delete(edit_overlay);
        edit_overlay = nullptr;
    }

    settings_list = nullptr;
    dark_switch = nullptr;
    tls_switch = nullptr;
    guard_switch = nullptr;
    log_start_switch = nullptr;
    log_done_switch = nullptr;
    log_error_switch = nullptr;
    log_boot_switch = nullptr;
    log_persist_switch = nullptr;
    poll_dd = nullptr;
    tz_dd = nullptr;
    screen_off_dd = nullptr;
    saver_dd = nullptr;
    brightness_slider = nullptr;
    brightness_value_lbl = nullptr;
    time_row_lbl = nullptr;
    ota_switch = nullptr;
    ota_row_lbl = nullptr;
    edit_title = nullptr;
    edit_ta = nullptr;
    edit_kb = nullptr;
    edit_row_idx = -1;
    memset(text_rows, 0, sizeof(text_rows));
    text_row_count = 0;
}


uint32_t settings_poll_interval_ms() { return poll_intervals_ms[poll_idx]; }

uint32_t settings_poll_interval_idle_ms()
{
    const uint32_t idle = poll_intervals_ms[poll_idx] * 5;
    return idle > POLL_IDLE_MAX_MS ? POLL_IDLE_MAX_MS : idle;
}


bool settings_start_guard()
{
    return start_guard;
}

bool settings_log_print_start() { return log_print_start; }
bool settings_log_print_done() { return log_print_done; }
bool settings_log_errors() { return log_errors; }
bool settings_log_boot() { return log_boot; }
bool settings_log_persist() { return log_persist; }
bool settings_ota_web() { return ota_web; }

uint32_t settings_display_idle_ms()
{
    return lv_display_get_inactive_time(nullptr);
}

bool settings_log_any()
{
    return log_print_start || log_print_done || log_errors || log_boot;
}

bool settings_tls_verify() { return tls_verify; }

const char *settings_timezone() { return timezones[tz_idx].posix; }

bool settings_time_synced() { return time(nullptr) >= MIN_VALID_EPOCH; }
