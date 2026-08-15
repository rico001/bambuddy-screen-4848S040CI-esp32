#include "smart_plugs_screen.h"

#include <Arduino.h>

#include "bambuddy_api.h"
#include "bambuddy_smart_plugs.h"
#include "ui_dialog.h"
#include "ui_layout.h"
#include "ui_util.h"
#include "ui_watch.h"
#include "ui_font.h"

static constexpr int PAD = 12;
static constexpr int HEADER_H = 54;
static constexpr int ROW_H = 88;
static constexpr uint32_t COL_OK = 0x2EAD62;
static constexpr uint32_t COL_ERR = 0xE53935;
static constexpr uint32_t COL_ACCENT = 0xFF6D00;
static constexpr uint32_t COL_MUTED = 0x9E9E9E;

static lv_obj_t *list_cont = nullptr;
static lv_obj_t *empty_lbl = nullptr;
static lv_obj_t *message_lbl = nullptr;
static lv_timer_t *ui_timer = nullptr;

static bambuddy_smart_plug_t shown[BB_SMART_PLUG_MAX_ITEMS];
static int shown_count = 0;
static bool list_loaded = false;
static int32_t switching_id = 0;
static uint32_t switching_ms = 0;

static void rebuild_list();

// Kennung und Schaltrichtung passen zusammen in user_data: die ID im
// unteren Teil, das Ziel im Vorzeichen. Damit braucht der Screen keine
// eigenen Merker fuer die offene Rueckfrage.
static void control_confirmed(void *user_data)
{
    const int32_t packed = (int32_t)(intptr_t)user_data;
    const int32_t id = packed < 0 ? -packed : packed;
    const bool turn_on = packed > 0;
    if (id == 0) return;

    switching_id = id;
    switching_ms = millis();
    rebuild_list();
    bambuddy_smart_plugs_request_control(id, turn_on);
}

static void control_cb(lv_event_t *event)
{
    const int index = (int)(intptr_t)lv_event_get_user_data(event);
    if (index < 0 || index >= shown_count || switching_id != 0) return;

    const bool turn_on = !shown[index].is_on;
    const int32_t packed = turn_on ? shown[index].id : -shown[index].id;

    char text[192];
    snprintf(text, sizeof(text),
             "Bist du sicher, dass \"%s\" wirklich %s werden soll?",
             shown[index].name, turn_on ? "eingeschaltet" : "ausgeschaltet");

    ui_confirm(turn_on ? "Smart Plug einschalten?" : "Smart Plug ausschalten?",
               text, "Abbrechen",
               turn_on ? "Einschalten" : "Ausschalten",
               turn_on ? COL_OK : COL_ERR,
               control_confirmed, (void *)(intptr_t)packed);
}

static void build_row(int index)
{
    const bambuddy_smart_plug_t &plug = shown[index];

    lv_obj_t *row = lv_obj_create(list_cont);
    lv_obj_set_size(row, LV_PCT(100), ROW_H);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(row, 12, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 12, 0);

    lv_obj_t *icon_box = lv_obj_create(row);
    lv_obj_set_size(icon_box, 52, 52);
    lv_obj_align(icon_box, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_remove_flag(icon_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(icon_box, 10, 0);
    lv_obj_set_style_border_width(icon_box, 0, 0);
    lv_obj_set_style_bg_color(icon_box, lv_color_hex(0x5D4037), 0);

    lv_obj_t *icon = lv_label_create(icon_box);
    lv_label_set_text(icon, LV_SYMBOL_POWER);
    lv_obj_set_style_text_color(icon, lv_color_hex(COL_ACCENT), 0);
    lv_obj_center(icon);

    lv_obj_t *name = lv_label_create(row);
    lv_label_set_text(name, plug.name);
    lv_obj_set_width(name, SCREEN_W - 2 * PAD - 52 - 104 - 44);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(name, &bb_font_16, 0);
    lv_obj_align(name, LV_ALIGN_TOP_LEFT, 66, 10);

    lv_obj_t *status = lv_label_create(row);
    if (!plug.reachable) {
        lv_label_set_text(status, "Nicht erreichbar");
        lv_obj_set_style_text_color(status, lv_color_hex(COL_ERR), 0);
    } else if (!plug.state_known) {
        lv_label_set_text(status, "Status unbekannt");
        lv_obj_set_style_text_color(status, lv_color_hex(COL_MUTED), 0);
    } else {
        lv_label_set_text(status, plug.is_on ? LV_SYMBOL_WIFI "  ON" : "OFF");
        lv_obj_set_style_text_color(status, lv_color_hex(plug.is_on ? COL_OK : COL_MUTED), 0);
    }
    lv_obj_set_style_text_font(status, &bb_font_12, 0);
    lv_obj_align(status, LV_ALIGN_TOP_LEFT, 66, 40);

    lv_obj_t *button = lv_button_create(row);
    lv_obj_set_size(button, 84, 54);
    lv_obj_align(button, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_radius(button, 10, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(plug.is_on ? COL_ERR : COL_OK), 0);
    lv_obj_add_event_cb(button, control_cb, LV_EVENT_CLICKED, (void *)(intptr_t)index);
    if (!plug.reachable || switching_id == plug.id) {
        lv_obj_add_state(button, LV_STATE_DISABLED);
    }

    lv_obj_t *button_lbl = lv_label_create(button);
    lv_label_set_text(button_lbl, plug.is_on ? LV_SYMBOL_POWER "  AUS"
                                            : LV_SYMBOL_POWER "  EIN");
    lv_obj_center(button_lbl);
}

static void rebuild_list()
{
    lv_obj_clean(list_cont);
    for (int i = 0; i < shown_count; i++) build_row(i);

    if (shown_count == 0) {
        ui_set_text(empty_lbl, list_loaded ? "Keine Smart Plugs eingerichtet."
                                           : "Smart Plugs werden geladen ...");
        lv_obj_remove_flag(empty_lbl, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(empty_lbl, LV_OBJ_FLAG_HIDDEN);
    }
}

static void ui_tick_cb(lv_timer_t *)
{
    // Nicht umbauen, solange eine Rueckfrage offen ist: Der Umbau loescht
    // die Zeile samt dem Knopf, den der Finger gerade beruehrt hat. Ein
    // Objekt zu entfernen, waehrend LVGL die Beruehrung darauf noch
    // verarbeitet, fuehrt in einen haengenden Bildaufbau.
    if (ui_confirm_is_open()) return;

    if (bambuddy_smart_plugs_take_fresh()) {
        shown_count = bambuddy_smart_plugs_copy(shown, BB_SMART_PLUG_MAX_ITEMS);
        list_loaded = true;
        switching_id = 0;
        rebuild_list();
    }

    if (switching_id != 0 && millis() - switching_ms > 10000) {
        switching_id = 0;
        rebuild_list();
    }

    if (bambuddy_smart_plugs_message_age() < 6000) {
        ui_set_text(message_lbl, bambuddy_smart_plugs_message());
    } else {
        ui_set_text(message_lbl, "");
    }
}

void smart_plugs_screen_create(lv_obj_t *parent)
{
    shown_count = 0;
    list_loaded = false;
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    list_cont = lv_obj_create(parent);
    // Volle Bildschirmhoehe: Die Ansicht liegt ueber allem, Status- und
    // Navigationsleiste sind verdeckt.
    lv_obj_set_size(list_cont, SCREEN_W - 2 * PAD, SCREEN_H - HEADER_H - 34);
    lv_obj_align(list_cont, LV_ALIGN_TOP_MID, 0, HEADER_H);
    lv_obj_set_style_bg_opa(list_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list_cont, 0, 0);
    lv_obj_set_style_pad_all(list_cont, 0, 0);
    lv_obj_set_style_pad_row(list_cont, 10, 0);
    lv_obj_set_flex_flow(list_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list_cont, LV_DIR_VER);

    empty_lbl = lv_label_create(parent);
    lv_label_set_text(empty_lbl, "Smart Plugs werden geladen ...");
    lv_obj_set_style_text_color(empty_lbl, lv_color_hex(COL_MUTED), 0);
    lv_obj_align(empty_lbl, LV_ALIGN_CENTER, 0, -12);

    message_lbl = lv_label_create(parent);
    lv_label_set_text(message_lbl, "");
    lv_obj_set_width(message_lbl, SCREEN_W - 2 * PAD);
    lv_label_set_long_mode(message_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(message_lbl, &bb_font_12, 0);
    lv_obj_set_style_text_color(message_lbl, lv_color_hex(COL_ACCENT), 0);
    lv_obj_align(message_lbl, LV_ALIGN_BOTTOM_LEFT, PAD + 4, -5);

    ui_timer = lv_timer_create(ui_tick_cb, 500, nullptr);
    lv_timer_set_repeat_count(ui_timer, -1);

    // Beim erneuten Oeffnen nicht auf denselben API-Abruf warten: Der letzte
    // gueltige Stand wird sofort gezeigt und danach im Hintergrund erneuert.
    if (bambuddy_smart_plugs_has_data()) {
        shown_count = bambuddy_smart_plugs_copy(shown, BB_SMART_PLUG_MAX_ITEMS);
        list_loaded = true;
        rebuild_list();
    }
    bambuddy_smart_plugs_set_visible(true);
    bambuddy_api_refresh_smart_plugs();
}

void smart_plugs_screen_destroy()
{
    bambuddy_smart_plugs_set_visible(false);
    ui_confirm_close();
    if (ui_timer) {
        lv_timer_delete(ui_timer);
        ui_timer = nullptr;
    }
    list_cont = nullptr;
    empty_lbl = nullptr;
    message_lbl = nullptr;
    shown_count = 0;
    list_loaded = false;
    switching_id = 0;
}
