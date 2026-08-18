#include "status_bar.h"

#include <Arduino.h>
#include <WiFi.h>
#include <time.h>

#include "bambuddy_api.h"
#include "bambuddy_config.h"
#include "settings_screen.h"
#include "ui_kit.h"
#include "ui_layout.h"
#include "ui_theme.h"
#include "ui_util.h"
#include "ui_font.h"


// Meldet der Netzwerk-Task laenger nichts, haengt er — das darf nicht als
// "alles gut" durchgehen.
static constexpr uint32_t HEARTBEAT_TIMEOUT_MS = 30000;

static lv_obj_t *wifi_lbl;
static lv_obj_t *source_lbl;
static lv_obj_t *clock_lbl;
static lv_timer_t *tick_timer = nullptr;

// Verbunden heisst: unauffaellig.
//
// Die Leiste zeigt Dauerzustaende. Ein gruenes "alles gut", das Tag und Nacht
// leuchtet, gewoehnt man sich in einer Woche ab — und uebersieht dann auch
// das Rot daneben. Farbe bekommt hier nur, was von der Regel abweicht; im
// Normalfall steht die Zeile so zurueckhaltend da wie die Uhr rechts.
static void update_wifi()
{
    if (WiFi.status() == WL_CONNECTED) {
        ui_set_text_fmt(wifi_lbl, LV_SYMBOL_WIFI " %s", WiFi.SSID().c_str());
        ui_set_text_color(wifi_lbl, COL_MUTED);
    } else {
        ui_set_text(wifi_lbl, LV_SYMBOL_WIFI " kein WLAN");
        ui_set_text_color(wifi_lbl, COL_ERR);
    }
}

static void update_source()
{
    const char *source = bambuddy_source_mqtt() ? "MQTT" : "HTTP";
    const uint32_t beat = bambuddy_api_heartbeat();

    // Erst der Dienst selbst, dann sein Ergebnis: ein haengender Task hat
    // sonst denselben Anblick wie ein nicht erreichbarer Server.
    if (beat == 0) {
        ui_set_text_fmt(source_lbl, "%s startet", source);
        ui_set_text_color(source_lbl, COL_MUTED);
        return;
    }
    if (millis() - beat > HEARTBEAT_TIMEOUT_MS) {
        ui_set_text_fmt(source_lbl, "%s hängt", source);
        ui_set_text_color(source_lbl, COL_ERR);
        return;
    }

    uint32_t color = COL_ERR;
    const char *suffix = "";

    switch (bambuddy_api_link()) {
    case BB_LINK_OK:           color = COL_MUTED; suffix = "";              break;
    case BB_LINK_STARTING:     color = COL_MUTED; suffix = " startet";      break;
    case BB_LINK_NO_WIFI:      color = COL_ERR;   suffix = " wartet";       break;
    case BB_LINK_NO_CONFIG:    color = COL_WARN;  suffix = " unkonfiguriert"; break;
    case BB_LINK_UNAUTHORIZED: color = COL_ERR;   suffix = " Key?";        break;
    case BB_LINK_NO_SERVER:
    default:                   color = COL_ERR;   suffix = " getrennt";     break;
    }

    ui_set_text_fmt(source_lbl, "%s%s", source, suffix);
    ui_set_text_color(source_lbl, color);
}

static void update_clock()
{
    if (!settings_time_synced()) {
        ui_set_text(clock_lbl, "--:--");
        return;
    }

    const time_t now = time(nullptr);
    struct tm local_tm;
    localtime_r(&now, &local_tm);

    char buf[6];
    strftime(buf, sizeof(buf), "%H:%M", &local_tm);
    ui_set_text(clock_lbl, buf);
}

static void tick_cb(lv_timer_t *)
{
    update_wifi();
    update_source();
    update_clock();
}

void status_bar_create(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_size(bar, SCREEN_W, STATUS_BAR_H);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    ui_screen_surface(bar);
    lv_obj_set_style_pad_hor(bar, GAP_L, 0);

    // Die Leiste sitzt auf demselben Grund wie die Kacheln darunter. Ihre
    // Grenze zieht eine Haarlinie statt einer eigenen Flaeche — eine zweite
    // Farbe fuer 26 Pixel Hoehe zerschneidet den Bildschirm optisch mehr,
    // als sie ordnet.
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(COL_LINE), 0);
    lv_obj_set_style_border_opa(bar, LV_OPA_COVER, 0);

    wifi_lbl = lv_label_create(bar);
    lv_obj_set_style_text_font(wifi_lbl, &bb_font_12, 0);
    lv_obj_set_width(wifi_lbl, 210);
    lv_label_set_long_mode(wifi_lbl, LV_LABEL_LONG_DOT);
    lv_obj_align(wifi_lbl, LV_ALIGN_LEFT_MID, 0, 0);

    source_lbl = lv_label_create(bar);
    lv_obj_set_style_text_font(source_lbl, &bb_font_12, 0);
    lv_obj_align(source_lbl, LV_ALIGN_CENTER, 40, 0);

    clock_lbl = lv_label_create(bar);
    lv_obj_set_style_text_font(clock_lbl, &bb_font_12, 0);
    lv_obj_set_style_text_color(clock_lbl, lv_color_hex(COL_MUTED), 0);
    lv_obj_align(clock_lbl, LV_ALIGN_RIGHT_MID, 0, 0);

    tick_cb(nullptr);

    if (!tick_timer) {
        tick_timer = lv_timer_create(tick_cb, 1000, nullptr);
        lv_timer_set_repeat_count(tick_timer, -1);
    }
}
