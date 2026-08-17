#include "general_screen.h"

#include <Arduino.h>

#include "bambuddy_hms.h"
#include "bambuddy_version.h"
#include "build_stamp.h"
#include "jog_screen.h"
#include "settings_screen.h"
#include "smart_plugs_screen.h"
#include "ui_dialog.h"
#include "ui_fullscreen.h"
#include "ui_layout.h"
#include "ui_theme.h"
#include "wifi_screen.h"
#include "ui_font.h"

static constexpr int PAD = 12;
static constexpr uint32_t COL_WIFI = 0x00897B;
static constexpr uint32_t COL_SETTINGS = COL_NEUTRAL; // Kachel "Einstellungen"

static lv_obj_t *root = nullptr;

// Die Unteransichten liegen als Vollbild ueber allem (siehe ui_fullscreen.h).
// Diese Kachel muss deshalb nichts mehr verwalten: kein Umbauen des eigenen
// Inhalts, keine Merker, wo man herkam. Der Zurueck-Knopf blendet die
// Ansicht aus, und darunter liegt genau die Kachel, von der man kam.
static void open_wifi_cb(lv_event_t *)
{
    ui_fullscreen_open("System / WLAN", COL_WIFI, wifi_screen_create,
                       wifi_screen_destroy);
}

static void open_settings_cb(lv_event_t *)
{
    ui_fullscreen_open("System / Settings", COL_SETTINGS, settings_screen_create,
                       settings_screen_destroy);
}

// --- Versionsabgleich ----------------------------------------------------
//
// Gruen: die Instanz laeuft mit genau der Fassung, gegen die dieses Display
// geprueft wurde. Rot: sie laeuft mit einer anderen — dann koennen einzelne
// Anzeigen still auf Null stehen, weil ein Antwortfeld anders heisst.
static lv_obj_t *version_badge = nullptr;
static lv_obj_t *version_badge_label = nullptr;
static lv_timer_t *version_timer = nullptr;

static void version_info_cb(lv_event_t *)
{
    if (ui_confirm_is_open()) return;

    // Umlaute und Akzente koennen die Schnitte seit ui_font.h; Gedankenstrich
    // und typografische Anfuehrungszeichen liegen jenseits von Latin-1 und
    // wuerden weiterhin als leeres Rechteck erscheinen.
    char text[192];

    if (!bambuddy_version_known()) {
        const char *err = bambuddy_version_error();
        snprintf(text, sizeof(text),
                 "%s\nDieses Display ist gegen die REST-API von "
                 BB_TESTED_VERSION " gebaut.",
                 err[0] ? err : "Noch keine Antwort von Bambuddy.");
        ui_info("Version unbekannt", text, "OK");
        return;
    }

    if (bambuddy_version_matches_tested()) {
        int n = snprintf(text, sizeof(text),
                         "Bambuddy v%s läuft. Die REST-API entspricht der "
                         "Fassung, gegen die dieses Display gebaut ist.",
                         bambuddy_version_current());
        if (n > 0 && n < (int)sizeof(text) &&
            bambuddy_version_update_available() && bambuddy_version_latest()[0]) {
            snprintf(text + n, sizeof(text) - n, "\nAuf GitHub steht %s.",
                     bambuddy_version_latest());
        }
        ui_info("Version passt", text, "OK");
        return;
    }

    snprintf(text, sizeof(text),
             "Bambuddy läuft mit v%s, gebaut ist dieses Display gegen die "
             "REST-API von " BB_TESTED_VERSION ".",
             bambuddy_version_current());
    ui_info("Version weicht ab", text, "OK");
}

static void version_badge_delete_cb(lv_event_t *)
{
    version_badge = nullptr;
    version_badge_label = nullptr;
}

static void refresh_version_badge()
{
    if (!version_badge) return;

    uint32_t color = COL_MUTED;
    const char *symbol = "?";

    if (bambuddy_version_known()) {
        const bool ok = bambuddy_version_matches_tested();
        color = ok ? COL_OK : COL_ERR;
        symbol = "!";
    }

    lv_obj_set_style_bg_color(version_badge, lv_color_hex(color), 0);
    lv_label_set_text(version_badge_label, symbol);
}

// Die Abfrage laeuft im Netzwerk-Task; die Kachel erfaehrt erst hier davon.
static void version_timer_cb(lv_timer_t *)
{
    if (bambuddy_version_take_fresh()) refresh_version_badge();
}

static void add_version_badge()
{
    version_badge = lv_button_create(root);
    lv_obj_set_size(version_badge, 40, 40);
    lv_obj_align(version_badge, LV_ALIGN_TOP_RIGHT, -PAD - 48, 6);
    lv_obj_set_style_radius(version_badge, 20, 0);
    lv_obj_add_event_cb(version_badge, version_info_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(version_badge, version_badge_delete_cb, LV_EVENT_DELETE, nullptr);

    version_badge_label = lv_label_create(version_badge);
    lv_obj_set_style_text_font(version_badge_label, &bb_font_24, 0);
    lv_obj_center(version_badge_label);

    refresh_version_badge();
}

// Neustart erst im naechsten Durchlauf, damit LVGL das Antippen und das
// Aufraeumen des Dialogs noch zu Ende bringt — sonst startet das Geraet
// mitten im Ereignis neu.
static void restart_async(void *)
{
    // Ausstehende Protokolleintraege noch sichern. Der uebliche Weg wartet
    // auf einen ruhigen Moment — den gibt es hier nicht mehr, und ein kurzer
    // Streifen unmittelbar vor dem Neustart faellt niemandem auf.
    bambuddy_hms_flush_now();

    // 500 statt der frueheren 200 ms. Noetig waere es nicht — putBytes und
    // end() schreiben synchron und kehren erst zurueck, wenn das NVS
    // festgeschrieben ist. Aber ein Neustart ist nicht rueckgaengig zu
    // machen, und drei Zehntelsekunden mehr merkt niemand.
    delay(500);
    ESP.restart();
}

static void restart_confirmed(void *)
{
    lv_async_call(restart_async, nullptr);
}

static void restart_cb(lv_event_t *)
{
    if (ui_confirm_is_open()) return;
    ui_confirm("Bambuddy-Display neu starten?", "Das Display startet sofort neu.",
               "Abbrechen", "Neustart", COL_ERR, restart_confirmed, nullptr);
}

static void add_restart_button()
{
    lv_obj_t *btn = lv_button_create(root);
    lv_obj_set_size(btn, 40, 40);
    lv_obj_align(btn, LV_ALIGN_TOP_RIGHT, -PAD, 6);
    lv_obj_set_style_radius(btn, 20, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(COL_ERR), 0);
    lv_obj_add_event_cb(btn, restart_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, LV_SYMBOL_REFRESH);
    lv_obj_center(label);
}

static lv_obj_t *add_launcher(const char *icon, const char *title, const char *subtitle,
                              uint32_t color, int y, lv_event_cb_t callback)
{
    lv_obj_t *btn = lv_button_create(root);
    lv_obj_set_size(btn, SCREEN_W - 2 * PAD, 82);
    lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_radius(btn, 14, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(color), 0);
    lv_obj_set_style_pad_all(btn, 18, 0);
    lv_obj_add_event_cb(btn, callback, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *icon_lbl = lv_label_create(btn);
    lv_label_set_text(icon_lbl, icon);
    lv_obj_set_style_text_font(icon_lbl, &bb_font_24, 0);
    lv_obj_align(icon_lbl, LV_ALIGN_LEFT_MID, 4, 0);

    lv_obj_t *title_lbl = lv_label_create(btn);
    lv_label_set_text(title_lbl, title);
    lv_obj_set_style_text_font(title_lbl, &bb_font_16, 0);
    lv_obj_align(title_lbl, LV_ALIGN_TOP_LEFT, 54, 6);

    lv_obj_t *sub_lbl = lv_label_create(btn);
    lv_label_set_text(sub_lbl, subtitle);
    lv_obj_set_width(sub_lbl, SCREEN_W - 2 * PAD - 100);
    lv_label_set_long_mode(sub_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(sub_lbl, &bb_font_12, 0);
    lv_obj_set_style_text_opa(sub_lbl, LV_OPA_70, 0);
    lv_obj_align(sub_lbl, LV_ALIGN_TOP_LEFT, 54, 34);

    lv_obj_t *arrow = lv_label_create(btn);
    lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -4, 0);
    return btn;
}

// Fusszeile: welche Fassung laeuft hier gerade?
//
// Nach einem Web-Update ist das die erste Frage, und die Update-Seite
// beantwortet sie nur dem, der einen Browser offen hat. Wer vor dem Geraet
// steht, sieht es hier — zusammen mit dem Bauzeitpunkt, denn zwei Staende
// derselben Versionsnummer gibt es beim Entwickeln staendig.
//
// Das Ausrufezeichen oben rechts meint etwas anderes: die Fassung der
// Bambuddy-Instanz. Hier steht die Firmware des Displays selbst.
static void add_footer()
{
    lv_obj_t *lbl = lv_label_create(root);
    lv_label_set_text_fmt(lbl, "v%s, %s", build_stamp_version(),
                          build_stamp_datetime());
    lv_obj_set_style_text_font(lbl, &bb_font_12, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(COL_MUTED), 0);
    lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, -PAD);
}

static void build_home()
{
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(root);
    lv_label_set_text(title, LV_SYMBOL_SETTINGS "  System");
    lv_obj_set_style_text_font(title, &bb_font_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, PAD + 4, 14);

    lv_obj_t *hint = lv_label_create(root);
    lv_label_set_text(hint, "Verbindung und Display verwalten");
    lv_obj_set_style_text_font(hint, &bb_font_12, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(COL_MUTED), 0);
    lv_obj_align(hint, LV_ALIGN_TOP_LEFT, PAD + 4, 42);

    add_version_badge();
    add_restart_button();

    add_launcher(LV_SYMBOL_WIFI, "WLAN", "Netzwerk suchen und Verbindung verwalten",
                 COL_WIFI, 68, open_wifi_cb);
    add_launcher(LV_SYMBOL_SETTINGS, "Einstellungen", "Bambuddy, MQTT und Darstellung",
                 COL_SETTINGS, 158, open_settings_cb);

    add_footer();
}

void general_screen_create(lv_obj_t *parent)
{
    root = parent;
    build_home();

    if (!version_timer) version_timer = lv_timer_create(version_timer_cb, 1000, nullptr);
}

void general_screen_show_smart_plugs()
{
    ui_fullscreen_open("System / Smart Plugs", COL_PLUG, smart_plugs_screen_create,
                       smart_plugs_screen_destroy);
}

void general_screen_show_jog()
{
    ui_fullscreen_open("System / Steuerung", COL_JOG, jog_screen_create,
                       jog_screen_destroy);
}

void general_screen_set_visible(bool visible)
{
    // Beim Aufschlagen der Kachel neu abfragen — nach einem Update der
    // Instanz soll das Ausrufezeichen nicht stundenlang die alte Lage zeigen.
    if (visible) {
        bambuddy_version_request_refresh();
        refresh_version_badge();
    }
}

