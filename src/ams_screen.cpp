#include "ams_screen.h"

#include <Arduino.h>
#include <string.h>

#include "bambuddy_api.h"
#include "bambuddy_filament.h"
#include "filament_config_view.h"
#include "ui_kit.h"
#include "ui_layout.h"
#include "ui_theme.h"
#include "ui_util.h"
#include "ui_font.h"

static constexpr int PAD = 12;
static constexpr int HEADER_H = 48;
static constexpr int SLOT_GAP = 8;

// Die Faecher teilen sich die volle Breite: Luftfeuchte und Temperatur
// stehen seit dem Umbau oben in der Kopfzeile, nicht mehr daneben.
static constexpr int SLOT_W = (SCREEN_W - 4 * PAD - 3 * SLOT_GAP) / 4;

// Aufbau einer Einheit von oben nach unten: Kopfzeile, Faecher, Schlauch.
static constexpr int SLOT_Y = 54;   // Oberkante der Faecher
static constexpr int SPOOL_W = 56;  // Rolle
static constexpr int SPOOL_H = 68;
// Zwei Zeilen Beschriftung, aus der Schrifthoehe gerechnet statt geraten:
// Die Latin-1-Schnitte sind hoeher als die eingebauten von frueher, und eine
// feste Zahl waere beim naechsten Schriftwechsel wieder daneben.
static constexpr int LABEL_LINES = 2;
static constexpr int SLOT_H = SPOOL_H + 12 + LABEL_LINES * 17;
static constexpr int TUBE_DROP = 14;   // Stueck bis zur Sammelschiene
static constexpr int HEAD_H = 42;      // Druckkopf am Abgang
static constexpr int UNIT_H = SLOT_Y + SLOT_H + TUBE_DROP + HEAD_H;

// Bedeutungsfarben kommen aus ui_theme.h. Frueher standen sie hier noch
// einmal in eigenen Werten — dieselben Namen, leicht andere Toene. Genau das
// sieht man auf dem Geraet als Stilbruch von Kachel zu Kachel.
static constexpr uint32_t COL_EMPTY = 0x30363D; // leerer Slot, nur hier

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


static uint32_t contrast_color(uint32_t color)
{
    const uint32_t r = (color >> 16) & 0xFF;
    const uint32_t g = (color >> 8) & 0xFF;
    const uint32_t b = color & 0xFF;
    return (r * 299 + g * 587 + b * 114) > 150000 ? 0x111111 : 0xFFFFFF;
}

// Foerdert der Drucker gerade? Der Schlauch des geladenen Fachs wird dann
// voll ausgezeichnet, sonst nur angedeutet: Geladen ist das Material auch im
// Leerlauf, unterwegs ist es nur waehrend eines Drucks.
static bool state_is_feeding(const char *state)
{
    return strcasecmp(state, "RUNNING") == 0 || strcasecmp(state, "PREPARE") == 0;
}

static bool printer_is_feeding()
{
    return state_is_feeding(shown.state);
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

static void reload_cb(lv_event_t *)
{
    if (reload_block_until_ms && millis() < reload_block_until_ms) return;

    bambuddy_api_send_command(BB_CMD_REFRESH_AMS);
    reload_block_until_ms = millis() + 3000;
    if (reload_btn) lv_obj_add_state(reload_btn, LV_STATE_DISABLED);
}

// Antippen oeffnet die Filamentkonfiguration fuer genau diesen Slot. Welcher
// gemeint ist, steckt in den Nutzdaten des Knopfes: AMS oben, Fach unten.
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

// Ein Fach als Spule von der Seite — Rand, farbiger Koerper, Nabe mit
// Fachnummer — die Bildsprache der Anzeige am Drucker, aber aus
// LVGL-Objekten: Die Bibliothek zeichnet in diesem Projekt kein SVG, und die
// Farben kommen ohnehin aus den Druckerdaten.
//
// Kein Fuellstandsbalken: Der Drucker meldet "remain" nur mit RFID-Spulen,
// sonst dauerhaft -1. Ein Balken, der meistens auf null steht, sagt weniger
// als gar keiner.
static void build_slot(lv_obj_t *card, const bambuddy_ams_unit_t &unit, int slot_index)
{
    const bambuddy_ams_tray_t &tray = unit.trays[slot_index];
    const bool active = tray_is_active(unit, tray);
    const uint32_t tray_color = tray.exists ? tray.color : COL_EMPTY;

    const int cell_x = PAD + slot_index * (SLOT_W + SLOT_GAP);

    lv_obj_t *slot = lv_obj_create(card);
    lv_obj_set_size(slot, SLOT_W, SLOT_H);
    lv_obj_align(slot, LV_ALIGN_TOP_LEFT, cell_x, SLOT_Y);
    lv_obj_remove_flag(slot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(slot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(slot, slot_clicked_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)(((uint32_t)unit.id << 16) |
                                            ((uint32_t)slot_index & 0xFFFF)));
    lv_obj_set_style_radius(slot, RADIUS_CTRL, 0);
    lv_obj_set_style_pad_all(slot, 0, 0);
    lv_obj_set_style_bg_opa(slot, LV_OPA_TRANSP, 0);
    // Das gerade foerdernde Fach bekommt den Rahmen — dieselbe Aussage wie
    // der hervorgehobene Schlauch darunter, nur am anderen Ende.
    lv_obj_set_style_border_width(slot, active ? 2 : 0, 0);
    lv_obj_set_style_border_color(slot, lv_color_hex(COL_ACCENT), 0);

    // --- Spule ---
    // Kinder duerfen den Tipp nicht abfangen: lv_obj_create() macht jeden
    // Behaelter anklickbar, und Rand, Koerper und Nabe decken fast die
    // gesamte Flaeche ab. Ohne das reagiert nur ein schmaler Streifen.
    lv_obj_t *rim = lv_obj_create(slot);
    lv_obj_remove_flag(rim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(rim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(rim, SPOOL_W, SPOOL_H);
    lv_obj_align(rim, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_set_style_radius(rim, 14, 0);
    lv_obj_set_style_border_width(rim, 0, 0);
    lv_obj_set_style_pad_all(rim, 0, 0);
    lv_obj_set_style_bg_color(rim, lv_color_hex(COL_RAISED), 0);

    lv_obj_t *band = lv_obj_create(rim);
    lv_obj_remove_flag(band, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(band, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(band, SPOOL_W - 12, SPOOL_H - 14);
    lv_obj_center(band);
    lv_obj_set_style_radius(band, 9, 0);
    lv_obj_set_style_border_width(band, 0, 0);
    lv_obj_set_style_pad_all(band, 0, 0);
    lv_obj_set_style_bg_color(band, lv_color_hex(tray_color), 0);
    if (!tray.exists) lv_obj_set_style_bg_opa(band, LV_OPA_40, 0);

    // Nabe in der Gegenfarbe: Auch eine weisse oder schwarze Spule bleibt so
    // als Spule erkennbar, und die Fachnummer bleibt lesbar.
    const uint32_t hub_color = contrast_color(tray_color);
    lv_obj_t *hub = lv_obj_create(band);
    lv_obj_remove_flag(hub, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(hub, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(hub, 26, 26);
    lv_obj_center(hub);
    lv_obj_set_style_radius(hub, 13, 0);
    lv_obj_set_style_border_width(hub, 0, 0);
    lv_obj_set_style_pad_all(hub, 0, 0);
    lv_obj_set_style_bg_color(hub, lv_color_hex(hub_color), 0);

    lv_obj_t *number = lv_label_create(hub);
    lv_label_set_text_fmt(number, "%d", slot_index + 1);
    lv_obj_set_style_text_font(number, &bb_font_12, 0);
    lv_obj_set_style_text_color(number, lv_color_hex(contrast_color(hub_color)), 0);
    lv_obj_center(number);

    // Ladekreis ueber der Spule, solange nach dem Konfigurieren noch auf die
    // Rueckmeldung des Druckers gewartet wird. Er liegt bewusst obenauf: Was
    // darunter steht, ist in dieser Zeit moeglicherweise veraltet.
    if (bambuddy_filament_slot_pending(unit.id, slot_index)) {
        lv_obj_t *busy = lv_spinner_create(slot);
        lv_obj_set_size(busy, 34, 34);
        lv_obj_align(busy, LV_ALIGN_TOP_MID, 0, 4 + (SPOOL_H - 34) / 2);
        lv_obj_remove_flag(busy, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_arc_width(busy, 4, LV_PART_MAIN);
        lv_obj_set_style_arc_width(busy, 4, LV_PART_INDICATOR);
        lv_obj_set_style_arc_opa(busy, LV_OPA_20, LV_PART_MAIN);
        lv_obj_set_style_arc_color(busy, lv_color_hex(COL_ACCENT), LV_PART_INDICATOR);
    }

    // --- Beschriftung ---
    char label[48];
    tray_label(unit.id, tray, label, sizeof(label));

    lv_obj_t *type = lv_label_create(slot);
    lv_obj_remove_flag(type, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text(type, label[0] ? label : "Leer");
    // Feste Breite UND Hoehe: nur dann bricht LVGL um und kuerzt mit
    // Punkten. Die Hoehe ist genau zwei Zeilen — "Bambu PETG Basic" passt
    // damit vollstaendig, "Overture Matte PLA Charcoal" bekommt am Ende
    // seine Punkte.
    lv_obj_set_size(type, SLOT_W - 6,
                    LABEL_LINES * lv_font_get_line_height(&bb_font_12));
    lv_label_set_long_mode(type, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(type, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(type, &bb_font_12, 0);
    lv_obj_set_style_text_color(
        type, lv_color_hex(tray.exists ? COL_TEXT : COL_MUTED), 0);
    lv_obj_align(type, LV_ALIGN_TOP_MID, 0, SPOOL_H + 8);

    // --- Schlauch ---
    // Vom Fach nach unten auf die Sammelschiene. Nur waehrend eines Drucks
    // traegt der Strang des foerdernden Fachs dessen Filamentfarbe — dann
    // zeigt er, welches Material gerade unterwegs ist. Steht der Drucker,
    // sind alle Straenge gleich: Geladen ist das Material zwar weiterhin,
    // aber es bewegt sich nichts, und eine farbige Leitung wuerde das
    // Gegenteil behaupten.
    const bool feeding = active && printer_is_feeding();
    const int width = feeding ? 4 : 2;
    ui_rule(card, cell_x + SLOT_W / 2 - width / 2, SLOT_Y + SLOT_H, width, TUBE_DROP,
         feeding ? tray_color : COL_LINE);
}

// Sammelschiene unter den Faechern samt Abgang zum Werkzeugkopf. Sie gehoert
// der Einheit, nicht dem einzelnen Fach — deshalb hier und nicht in
// build_slot().
static void build_manifold(lv_obj_t *card, int slot_count, int active_index,
                           uint32_t active_color)
{
    const int y = SLOT_Y + SLOT_H + TUBE_DROP;
    const int first = PAD + SLOT_W / 2;
    const int last = PAD + (slot_count - 1) * (SLOT_W + SLOT_GAP) + SLOT_W / 2;
    const int mid = (first + last) / 2;

    if (slot_count > 1) ui_rule(card, first, y, last - first, 2, COL_LINE);

    // Waehrend des Drucks den Weg vom foerdernden Fach bis zum Abgang in
    // dessen Farbe weiterzeichnen: Der Weg des Materials soll durchgehend
    // sein, nicht am Knick aufhoeren.
    const bool feeding = active_index >= 0 && printer_is_feeding();

    if (feeding) {
        const int from = PAD + active_index * (SLOT_W + SLOT_GAP) + SLOT_W / 2;
        const int lo = from < mid ? from : mid;
        const int hi = from < mid ? mid : from;
        if (hi > lo) ui_rule(card, lo, y, hi - lo, 4, active_color);
    }

    ui_rule(card, mid - (feeding ? 2 : 1), y, feeding ? 4 : 2, 12,
         feeding ? active_color : COL_LINE);

    // Der Druckkopf am Ende der Leitung: Heizblock und darunter die Duese.
    //
    // Der Kegel entsteht aus drei immer schmaleren Streifen. Eine echte
    // Dreiecksform gaebe es in LVGL nur ueber lv_line — und das braeuchte ein
    // Punktfeld, das leben muss, solange die Linie lebt. Bei einem Screen,
    // der sich staendig neu baut, sind drei Rechtecke die ehrlichere Loesung,
    // und bei dieser Groesse sieht man den Unterschied ohnehin nicht.
    //
    // Waehrend des Drucks traegt die Duesenspitze die Farbe des Filaments:
    // Der Weg endet dann nicht an einem grauen Kasten, sondern laeuft
    // sichtbar hindurch.
    const uint32_t head_color = feeding ? active_color : COL_LINE;

    lv_obj_t *block = lv_obj_create(card);
    lv_obj_remove_flag(block, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(block, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(block, 26, 24);
    lv_obj_align(block, LV_ALIGN_TOP_LEFT, mid - 13, y + 6);
    lv_obj_set_style_radius(block, 4, 0);
    lv_obj_set_style_border_width(block, 0, 0);
    lv_obj_set_style_pad_all(block, 0, 0);
    lv_obj_set_style_bg_color(block, lv_color_hex(COL_RAISED), 0);

    // Fenster im Heizblock, rund: So liest es sich als Blick auf das
    // Filament im Kopf und nicht als Schlitz — und waehrend des Drucks
    // traegt genau dieser Punkt die Materialfarbe.
    lv_obj_t *slot_window = lv_obj_create(block);
    lv_obj_remove_flag(slot_window, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(slot_window, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(slot_window, 10, 10);
    lv_obj_center(slot_window);
    lv_obj_set_style_radius(slot_window, 5, 0);
    lv_obj_set_style_border_width(slot_window, 0, 0);
    lv_obj_set_style_pad_all(slot_window, 0, 0);
    lv_obj_set_style_bg_color(slot_window, lv_color_hex(head_color), 0);

    static constexpr int CONE_W[] = {14, 9, 5};
    int cone_y = y + 30;
    for (int i = 0; i < 3; i++) {
        ui_rule(card, mid - CONE_W[i] / 2, cone_y, CONE_W[i], 2,
                i == 2 ? head_color : COL_RAISED);
        cone_y += 2;
    }
}

// Luftfeuchte und Temperatur als waagerechte Kapsel in der Kopfzeile.
//
// Sie stand vorher hochkant neben den Faechern und nahm ihnen Breite weg.
// Oben rechts gehoert sie zum Namen der Einheit — beides beschreibt die
// Einheit als Ganzes, waehrend darunter die einzelnen Faecher stehen.
static void build_badge(lv_obj_t *card, const bambuddy_ams_unit_t &unit)
{
    lv_obj_t *badge = lv_obj_create(card);
    lv_obj_remove_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(badge, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(badge, LV_SIZE_CONTENT, 34);
    lv_obj_align(badge, LV_ALIGN_TOP_RIGHT, -GAP_L, 12);
    lv_obj_set_style_radius(badge, RADIUS_CTRL, 0);
    lv_obj_set_style_border_width(badge, 0, 0);
    lv_obj_set_style_pad_hor(badge, GAP_M, 0);
    lv_obj_set_style_pad_ver(badge, 0, 0);
    lv_obj_set_style_pad_column(badge, GAP_S, 0);
    lv_obj_set_style_bg_color(badge, lv_color_hex(COL_RAISED), 0);
    lv_obj_set_flex_flow(badge, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(badge, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    // Blau wie in der uebrigen Oberflaeche: Der Tropfen sagt "Luftfeuchte",
    // nicht "Achtung".
    lv_obj_t *drop = lv_label_create(badge);
    lv_label_set_text(drop, LV_SYMBOL_TINT);
    lv_obj_set_style_text_color(drop, lv_color_hex(COL_ACCENT), 0);

    lv_obj_t *humidity = lv_label_create(badge);
    if (unit.humidity >= 0) {
        lv_label_set_text_fmt(humidity, "%d%%", (int)unit.humidity);
    } else {
        lv_label_set_text(humidity, "--");
    }
    lv_obj_set_style_text_font(humidity, &bb_font_16, 0);
    lv_obj_set_style_text_color(humidity, lv_color_hex(COL_TEXT), 0);

    // Senkrechter Strich zwischen den beiden Werten: Es sind zwei Angaben
    // derselben Einheit, keine zwei Kapseln.
    lv_obj_t *divider = lv_obj_create(badge);
    lv_obj_remove_flag(divider, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(divider, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(divider, 1, 18);
    lv_obj_set_style_radius(divider, 0, 0);
    lv_obj_set_style_border_width(divider, 0, 0);
    lv_obj_set_style_pad_all(divider, 0, 0);
    lv_obj_set_style_bg_color(divider, lv_color_hex(COL_LINE), 0);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);

    lv_obj_t *thermo = lv_label_create(badge);
    lv_label_set_text(thermo, BB_SYMBOL_TEMP);
    lv_obj_set_style_text_color(thermo, lv_color_hex(COL_MUTED), 0);

    lv_obj_t *temp = lv_label_create(badge);
    if (unit.temperature_known) {
        lv_label_set_text_fmt(temp, "%d °C", (int)(unit.temperature + 0.5f));
    } else {
        lv_label_set_text(temp, "--");
    }
    lv_obj_set_style_text_font(temp, &bb_font_16, 0);
    lv_obj_set_style_text_color(temp, lv_color_hex(COL_TEXT), 0);
}

static void build_unit(const bambuddy_ams_unit_t &unit)
{
    lv_obj_t *card = lv_obj_create(list_cont);
    lv_obj_set_size(card, LV_PCT(100), UNIT_H);
    ui_card_style(card);

    char name[20];
    unit_name(unit, name, sizeof(name));
    lv_obj_t *name_lbl = lv_label_create(card);
    lv_label_set_text(name_lbl, name);
    lv_obj_set_style_text_font(name_lbl, &bb_font_24, 0);
    lv_obj_set_style_text_color(name_lbl, lv_color_hex(COL_TEXT), 0);
    lv_obj_align(name_lbl, LV_ALIGN_TOP_LEFT, GAP_L, 14);

    build_badge(card, unit);

    const int slot_count = unit.is_ht ? (unit.tray_count > 0 ? unit.tray_count : 1)
                                      : BB_AMS_MAX_TRAYS;

    int active_index = -1;
    for (int i = 0; i < slot_count && i < BB_AMS_MAX_TRAYS; i++) {
        if (tray_is_active(unit, unit.trays[i])) active_index = i;
    }

    // Schiene zuerst, Faecher darueber: Die Stutzen der Faecher sollen die
    // Linie ueberdecken, nicht umgekehrt.
    const uint32_t active_color =
        (active_index >= 0 && unit.trays[active_index].exists)
            ? unit.trays[active_index].color
            : COL_ACCENT;

    build_manifold(card, slot_count < BB_AMS_MAX_TRAYS ? slot_count : BB_AMS_MAX_TRAYS,
                   active_index, active_color);

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
    // Der Druckzustand gehoert dazu, seit der Schlauch ihn zeigt: Ohne diese
    // Zeile bliebe die Zufuhr gedaempft, bis sich zufaellig etwas am AMS
    // aendert — und der Beginn eines Drucks aendert daran nichts.
    return !have_status || shown.ams_exists != next.ams_exists ||
           shown.ams_count != next.ams_count || shown.tray_now != next.tray_now ||
           state_is_feeding(shown.state) != state_is_feeding(next.state) ||
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
    lv_obj_set_style_text_font(title, &bb_font_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, PAD + 4, 14);

    // Der Hintergrundabruf laeuft alle 30 Sekunden. Nach einem Spulenwechsel
    // ist das lang — dieser Knopf holt sofort.
    // Masse und Position wie die Knoepfe oben rechts im Status-Screen —
    // gleiche Stelle, gleiche Groesse, gleiche Rundung.
    reload_btn = lv_button_create(root);
    lv_obj_set_size(reload_btn, 52, 30);
    lv_obj_align(reload_btn, LV_ALIGN_TOP_RIGHT, -PAD, 8);
    lv_obj_set_style_radius(reload_btn, RADIUS_CTRL, 0);
    lv_obj_set_style_bg_color(reload_btn, lv_color_hex(COL_NEUTRAL), 0);
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
