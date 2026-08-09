#include "ams_screen.h"

#include <Arduino.h>
#include <string.h>

#include "bambuddy_api.h"
#include "bambuddy_filament.h"
#include "filament_config_view.h"
#include "ui_layout.h"
#include "ui_util.h"

static constexpr int PAD = 12;
static constexpr int HEADER_H = 48;
static constexpr int UNIT_H = 164;
static constexpr int SLOT_GAP = 8;
static constexpr int SLOT_W = (SCREEN_W - 4 * PAD - 3 * SLOT_GAP) / 4;

static constexpr uint32_t COL_OK = 0x2EAD62;
static constexpr uint32_t COL_WARN = 0xFFB300;
static constexpr uint32_t COL_ERR = 0xE53935;
static constexpr uint32_t COL_ACCENT = 0x2196F3;
static constexpr uint32_t COL_MUTED = 0x9E9E9E;
static constexpr uint32_t COL_EMPTY = 0x30363D;

static lv_obj_t *root = nullptr;
static lv_obj_t *list_cont = nullptr;
static lv_obj_t *state_lbl = nullptr;
static lv_obj_t *reload_btn = nullptr;
// Bis wann bleibt der Knopf gesperrt? Ohne Sperre laesst sich schneller
// tippen, als der Drucker antworten kann — jeder Tipp reiht dann einen
// weiteren Abruf ein, und der Netzwerk-Task arbeitet minutenlang Rueckstand
// ab.
static uint32_t reload_block_until_ms = 0;
static lv_timer_t *ui_timer = nullptr;
static bool screen_visible = false;
static bool have_status = false;
static bambuddy_status_t shown;
// Welches Fach wartet gerade auf frische Daten? Der Wert kommt nicht aus
// dem Druckerstatus, also muss er getrennt beobachtet werden — sonst
// bemerkt der Screen weder das Erscheinen noch das Verschwinden des
// Ladekreises.
static uint32_t shown_pending_token = 0;

static uint32_t humidity_color(int humidity)
{
    if (humidity < 0) return COL_MUTED;
    if (humidity <= 40) return COL_OK;
    if (humidity <= 60) return COL_WARN;
    return COL_ERR;
}

static uint32_t contrast_color(uint32_t color)
{
    const uint32_t r = (color >> 16) & 0xFF;
    const uint32_t g = (color >> 8) & 0xFF;
    const uint32_t b = color & 0xFF;
    return (r * 299 + g * 587 + b * 114) > 150000 ? 0x111111 : 0xFFFFFF;
}

static bool tray_is_active(const bambuddy_ams_unit_t &unit,
                           const bambuddy_ams_tray_t &tray)
{
    if (shown.tray_now == 255 || !tray.exists) return false;
    if (unit.id >= 0 && unit.id < 4) {
        return shown.tray_now == unit.id * BB_AMS_MAX_TRAYS + tray.id;
    }
    return shown.tray_now == tray.id;
}

// Was steckt im Fach? Der Drucker entscheidet — siehe
// bambuddy_filament_tray_name(). Solange die Profilliste noch nicht geladen
// ist, bleibt das blosse Material stehen statt gar nichts.
static void tray_label(int32_t ams_id, const bambuddy_ams_tray_t &tray, char *out,
                       size_t out_len)
{
    if (out_len) out[0] = '\0';
    if (!tray.exists) return;

    const char *known = bambuddy_filament_tray_name(ams_id, tray.id, tray.info_idx);
    const char *text = (known && known[0]) ? known : tray.type;
    strncpy(out, text, out_len - 1);
    out[out_len - 1] = '\0';
}

static void unit_name(const bambuddy_ams_unit_t &unit, char *out, size_t out_len)
{
    if (unit.is_ht) {
        snprintf(out, out_len, "AMS-HT");
    } else if (unit.id >= 0 && unit.id < 26) {
        snprintf(out, out_len, "AMS-%c", (char)('A' + unit.id));
    } else {
        snprintf(out, out_len, "AMS %d", (int)unit.id);
    }
}

// Antippen oeffnet die Filamentkonfiguration fuer genau diesen Slot. Welcher
// gemeint ist, steckt in den Nutzdaten des Knopfes: AMS oben, Fach unten.
static void reload_cb(lv_event_t *)
{
    if (reload_block_until_ms && millis() < reload_block_until_ms) return;

    bambuddy_api_send_command(BB_CMD_REFRESH_AMS);
    reload_block_until_ms = millis() + 3000;
    if (reload_btn) lv_obj_add_state(reload_btn, LV_STATE_DISABLED);
}

static void slot_clicked_cb(lv_event_t *e)
{
    const uint32_t packed = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    const int32_t ams_id = (int32_t)(packed >> 16);
    const int32_t tray_id = (int32_t)(packed & 0xFFFF);

    // Farbe und Beschriftung aus dem zuletzt angezeigten Stand holen — der
    // Knopf selbst traegt sie nicht, und ein zweiter Zeiger darauf waere
    // genau der Zeiger, der beim naechsten Neuaufbau ins Leere zeigt.
    char type[16] = "";
    char current[48] = "";
    char label[48] = "AMS";

    filament_slot_info_t slot = {};
    slot.ams_id = ams_id;
    slot.tray_id = tray_id;
    slot.color = 0xFFFFFF;

    for (int i = 0; i < shown.ams_count; i++) {
        if (shown.ams[i].id != ams_id) continue;

        char name[20];
        unit_name(shown.ams[i], name, sizeof(name));
        snprintf(label, sizeof(label), "%s - Fach %d", name, (int)tray_id + 1);

        if (tray_id >= 0 && tray_id < BB_AMS_MAX_TRAYS) {
            const bambuddy_ams_tray_t &tray = shown.ams[i].trays[tray_id];
            if (tray.exists) {
                slot.color = tray.color;
                slot.temp_min = tray.temp_min;
                slot.temp_max = tray.temp_max;
                strncpy(type, tray.type, sizeof(type) - 1);
                type[sizeof(type) - 1] = '\0';
                tray_label(ams_id, tray, current, sizeof(current));
            }
        }
        break;
    }

    slot.label = label;
    slot.type = type;
    slot.name = current;

    Serial.printf("[AMS] Slot angetippt: AMS %d Fach %d\n", (int)ams_id,
                  (int)tray_id + 1);
    filament_config_open(slot);
}

static void build_slot(lv_obj_t *card, const bambuddy_ams_unit_t &unit, int slot_index)
{
    const bambuddy_ams_tray_t &tray = unit.trays[slot_index];
    const bool active = tray_is_active(unit, tray);

    lv_obj_t *slot = lv_obj_create(card);
    lv_obj_set_size(slot, SLOT_W, 94);
    lv_obj_align(slot, LV_ALIGN_TOP_LEFT, PAD + slot_index * (SLOT_W + SLOT_GAP), 58);
    lv_obj_remove_flag(slot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(slot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(slot, slot_clicked_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)(((uint32_t)unit.id << 16) |
                                            ((uint32_t)slot_index & 0xFFFF)));
    lv_obj_set_style_radius(slot, 12, 0);
    lv_obj_set_style_pad_all(slot, 0, 0);
    lv_obj_set_style_border_width(slot, active ? 2 : 0, 0);
    lv_obj_set_style_border_color(slot, lv_color_hex(COL_ACCENT), 0);

    const uint32_t tray_color = tray.exists ? tray.color : COL_EMPTY;
    // Kinder duerfen den Tipp nicht abfangen: lv_obj_create() macht jeden
    // Behaelter anklickbar, und Punkt, Kern und Balken decken fast die
    // gesamte Slot-Flaeche ab. Ohne das reagiert nur ein schmaler Streifen.
    lv_obj_t *dot = lv_obj_create(slot);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(dot, 36, 36);
    lv_obj_align(dot, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(dot, 18, 0);
    lv_obj_set_style_border_width(dot, 3, 0);
    lv_obj_set_style_border_color(dot, lv_color_hex(contrast_color(tray_color)), 0);
    lv_obj_set_style_border_opa(dot, LV_OPA_30, 0);
    lv_obj_set_style_pad_all(dot, 0, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(tray_color), 0);

    // Farbiger Rollenkörper plus kontrastierender Kern: bleibt auch bei sehr
    // hellen oder dunklen Filamentfarben klar als Spule erkennbar.
    const uint32_t hub_color = contrast_color(tray_color);
    lv_obj_t *hub = lv_obj_create(dot);
    lv_obj_remove_flag(hub, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(hub, 18, 18);
    lv_obj_remove_flag(hub, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(hub, 9, 0);
    lv_obj_set_style_border_width(hub, 0, 0);
    lv_obj_set_style_pad_all(hub, 0, 0);
    lv_obj_set_style_bg_color(hub, lv_color_hex(hub_color), 0);
    lv_obj_center(hub);

    // Ladekreis ueber der Spule, solange nach dem Konfigurieren noch auf die
    // Rueckmeldung des Druckers gewartet wird. Er liegt bewusst obenauf: Was
    // darunter steht, ist in dieser Zeit moeglicherweise veraltet.
    if (bambuddy_filament_slot_pending(unit.id, slot_index)) {
        lv_obj_t *busy = lv_spinner_create(slot);
        lv_obj_set_size(busy, 34, 34);
        lv_obj_align(busy, LV_ALIGN_TOP_MID, 0, 5);
        lv_obj_remove_flag(busy, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_arc_width(busy, 4, LV_PART_MAIN);
        lv_obj_set_style_arc_width(busy, 4, LV_PART_INDICATOR);
        lv_obj_set_style_arc_opa(busy, LV_OPA_20, LV_PART_MAIN);
        lv_obj_set_style_arc_color(busy, lv_color_hex(COL_ACCENT), LV_PART_INDICATOR);
    }

    lv_obj_t *number = lv_label_create(hub);
    lv_label_set_text_fmt(number, "%d", slot_index + 1);
    lv_obj_set_style_text_font(number, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(number, lv_color_hex(contrast_color(hub_color)), 0);
    lv_obj_center(number);

    char label[48];
    tray_label(unit.id, tray, label, sizeof(label));

    lv_obj_t *type = lv_label_create(slot);
    lv_obj_remove_flag(type, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text(type, label[0] ? label : "Leer");
    // Feste Breite UND Hoehe: nur dann kann LVGL mehrzeilig kuerzen. Namen
    // wie "Bambu PETG Basic" passen in zwei Zeilen, laengere bekommen
    // Punkte statt aus der Kachel zu laufen.
    lv_obj_set_size(type, SLOT_W - 6, 30);
    lv_label_set_long_mode(type, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(type, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(type, &lv_font_montserrat_12, 0);
    if (!tray.exists) lv_obj_set_style_text_color(type, lv_color_hex(COL_MUTED), 0);
    lv_obj_align(type, LV_ALIGN_TOP_MID, 0, 44);

    lv_obj_t *remain = lv_bar_create(slot);
    lv_obj_remove_flag(remain, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(remain, SLOT_W - 16, 7);
    lv_obj_align(remain, LV_ALIGN_BOTTOM_MID, 0, -7);
    lv_bar_set_range(remain, 0, 100);
    lv_bar_set_value(remain, tray.exists ? tray.remain : 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(remain, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(remain, 4, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(remain, lv_color_hex(tray_color), LV_PART_INDICATOR);
}

static void build_unit(const bambuddy_ams_unit_t &unit)
{
    lv_obj_t *card = lv_obj_create(list_cont);
    lv_obj_set_size(card, LV_PCT(100), UNIT_H);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 0, 0);

    char name[20];
    unit_name(unit, name, sizeof(name));
    lv_obj_t *name_lbl = lv_label_create(card);
    lv_label_set_text(name_lbl, name);
    lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_24, 0);
    lv_obj_align(name_lbl, LV_ALIGN_TOP_LEFT, 16, 14);

    lv_obj_t *humidity_lbl = lv_label_create(card);
    if (unit.humidity >= 0) {
        lv_label_set_text_fmt(humidity_lbl, LV_SYMBOL_TINT " %d%%", (int)unit.humidity);
    } else {
        lv_label_set_text(humidity_lbl, LV_SYMBOL_TINT " --");
    }
    lv_obj_set_style_text_color(humidity_lbl,
                                lv_color_hex(humidity_color(unit.humidity)), 0);
    lv_obj_set_style_text_font(humidity_lbl, &lv_font_montserrat_16, 0);
    lv_obj_align(humidity_lbl, LV_ALIGN_TOP_RIGHT, -126, 17);

    lv_obj_t *temp_lbl = lv_label_create(card);
    if (unit.temperature_known) {
        const int tenths = (int)(unit.temperature * 10.0f + 0.5f);
        lv_label_set_text_fmt(temp_lbl, "%d.%d C", tenths / 10, tenths % 10);
    } else {
        lv_label_set_text(temp_lbl, "-- C");
    }
    lv_obj_set_style_text_color(temp_lbl, lv_color_hex(COL_WARN), 0);
    lv_obj_set_style_text_font(temp_lbl, &lv_font_montserrat_16, 0);
    lv_obj_align(temp_lbl, LV_ALIGN_TOP_RIGHT, -16, 17);

    const int slot_count = unit.is_ht ? (unit.tray_count > 0 ? unit.tray_count : 1)
                                      : BB_AMS_MAX_TRAYS;
    for (int i = 0; i < slot_count && i < BB_AMS_MAX_TRAYS; i++) {
        build_slot(card, unit, i);
    }
}

static void rebuild()
{
    lv_obj_clean(list_cont);

    if (!have_status) {
        ui_set_text(state_lbl, "AMS-Daten werden geladen ...");
        lv_obj_remove_flag(state_lbl, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    if (shown.ams_count == 0) {
        ui_set_text(state_lbl, "Kein AMS verbunden.");
        lv_obj_remove_flag(state_lbl, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_add_flag(state_lbl, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < shown.ams_count; i++) build_unit(shown.ams[i]);
}

static bool ams_changed(const bambuddy_status_t &next)
{
    return !have_status || shown.ams_exists != next.ams_exists ||
           shown.ams_count != next.ams_count || shown.tray_now != next.tray_now ||
           memcmp(shown.ams, next.ams, sizeof(shown.ams)) != 0;
}

static void ui_tick_cb(lv_timer_t *)
{
    // Solange die Konfiguration offen ist, bleibt die Liste stehen. Ein
    // Neuaufbau wuerde den gerade angetippten Slot-Knopf loeschen, waehrend
    // LVGL noch dessen Ereignis abarbeitet — das Geraet startet dann neu.
    if (filament_config_is_open()) return;

    if (reload_block_until_ms && millis() >= reload_block_until_ms) {
        reload_block_until_ms = 0;
        if (reload_btn) lv_obj_remove_state(reload_btn, LV_STATE_DISABLED);
    }

    // Der Ladekreis haengt nicht am Druckerstatus. Erscheint oder verschwindet
    // er, muss trotzdem neu gezeichnet werden.
    const uint32_t token = bambuddy_filament_pending_token();
    const bool pending_changed = token != shown_pending_token;
    shown_pending_token = token;

    bambuddy_status_t next;
    if (!bambuddy_api_copy_status(&next)) {
        if (pending_changed) rebuild();
        return;
    }
    if (!ams_changed(next) && !pending_changed) return;

    shown = next;
    have_status = true;
    rebuild();
}

static void build_screen()
{
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(root);
    lv_label_set_text(title, "AMS Status");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, PAD + 4, 14);

    // Der Hintergrundabruf laeuft alle 30 Sekunden. Nach einem Spulenwechsel
    // ist das lang — dieser Knopf holt sofort.
    // Masse und Position wie die Knoepfe oben rechts im Status-Screen —
    // gleiche Stelle, gleiche Groesse, gleiche Rundung.
    reload_btn = lv_button_create(root);
    lv_obj_set_size(reload_btn, 52, 30);
    lv_obj_align(reload_btn, LV_ALIGN_TOP_RIGHT, -PAD, 8);
    lv_obj_set_style_radius(reload_btn, 10, 0);
    lv_obj_set_style_bg_color(reload_btn, lv_color_hex(0x546E7A), 0);
    lv_obj_add_event_cb(reload_btn, reload_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *reload_lbl = lv_label_create(reload_btn);
    lv_label_set_text(reload_lbl, LV_SYMBOL_REFRESH);
    lv_obj_center(reload_lbl);

    list_cont = lv_obj_create(root);
    lv_obj_set_size(list_cont, SCREEN_W - 2 * PAD, CONTENT_H - HEADER_H - PAD);
    lv_obj_align(list_cont, LV_ALIGN_TOP_MID, 0, HEADER_H);
    lv_obj_set_style_bg_opa(list_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list_cont, 0, 0);
    lv_obj_set_style_pad_all(list_cont, 0, 0);
    lv_obj_set_style_pad_row(list_cont, 10, 0);
    lv_obj_set_flex_flow(list_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list_cont, LV_DIR_VER);

    state_lbl = lv_label_create(root);
    lv_label_set_text(state_lbl, "AMS-Daten werden geladen ...");
    lv_obj_set_style_text_color(state_lbl, lv_color_hex(COL_MUTED), 0);
    lv_obj_align(state_lbl, LV_ALIGN_CENTER, 0, -8);

    ui_timer = lv_timer_create(ui_tick_cb, 500, nullptr);
    lv_timer_set_repeat_count(ui_timer, -1);
    ui_tick_cb(nullptr);
}

void ams_screen_create(lv_obj_t *parent)
{
    root = parent;
}

void ams_screen_set_visible(bool visible)
{
    if (visible == screen_visible) return;
    screen_visible = visible;
    bambuddy_api_set_ams_visible(visible);
    // Die Namen zu den Kurz-IDs stehen in der Profilliste. Sie hier zu holen
    // kostet einmalig gut 4 KB und erspart, dass in den Kaechern dauerhaft
    // nur "PLA" steht, bis jemand die Konfiguration oeffnet.
    if (visible) bambuddy_filament_preload();

    if (visible) {
        build_screen();
        return;
    }

    if (ui_timer) {
        lv_timer_delete(ui_timer);
        ui_timer = nullptr;
    }
    lv_obj_clean(root);
    list_cont = nullptr;
    state_lbl = nullptr;
    reload_btn = nullptr;
    reload_block_until_ms = 0;
    have_status = false;
    shown_pending_token = 0;
    memset(&shown, 0, sizeof(shown));
}
