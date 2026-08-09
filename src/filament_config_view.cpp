#include "filament_config_view.h"

#include <Arduino.h> // ps_malloc
#include <lvgl.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "bambuddy_filament.h"
#include "ui_layout.h"
#include "ui_theme.h"
#include "ui_util.h"
#include "ui_watch.h"

static constexpr int PAD = 12;
static constexpr int CONTENT_W = SCREEN_W - 2 * PAD;

// Hoehenplan. Die Summe muss unter 480 bleiben, sonst rutscht die Fusszeile
// aus dem Bild — auf einem Geraet ohne Scrollen faellt das erst beim
// Antippen auf, wenn der Knopf nicht mehr da ist.
static constexpr int HEADER_H = 44;
static constexpr int CHIPS_Y = 52;
static constexpr int CHIPS_H = 36;
static constexpr int LIST_Y = 96;
// Nicht LIST_H nennen: Das ist anderswo ein Header-Guard und damit
// ein Makro, das diese Zeile zerlegt, bevor der Compiler sie sieht.
static constexpr int LIST_HEIGHT = 244;
static constexpr int COLOR_Y = 348;
static constexpr int COLOR_H = 44;
static constexpr int FOOT_Y = 400;
static constexpr int FOOT_H = 54;

static constexpr int ROW_H = 44;

// Fuenf feste Farben decken den Alltag ab; alles andere kommt aus dem Rad.
static const uint32_t QUICK_COLORS[] = {0x000000, 0xFFFFFF, 0xE53935, 0x43A047, 0x1E88E5};
static constexpr int QUICK_COLOR_COUNT = sizeof(QUICK_COLORS) / sizeof(QUICK_COLORS[0]);

static constexpr int WHEEL_SIZE = 200;
static constexpr int WHEEL_RADIUS = 96;

static lv_obj_t *overlay = nullptr;
static lv_obj_t *title_lbl = nullptr;
static lv_obj_t *preview_lbl = nullptr;
static lv_obj_t *preview_dot = nullptr;
static lv_obj_t *list_cont = nullptr;
static lv_obj_t *chips_cont = nullptr;
static lv_obj_t *hint_lbl = nullptr;
static lv_obj_t *color_row = nullptr;
static lv_obj_t *custom_btn = nullptr;
static lv_obj_t *apply_btn = nullptr;
static lv_obj_t *apply_lbl = nullptr;
static lv_timer_t *ui_timer = nullptr;

static lv_obj_t *wheel_overlay = nullptr;
static lv_obj_t *wheel_canvas = nullptr;
static lv_obj_t *wheel_preview = nullptr;
static lv_obj_t *wheel_slider = nullptr;
static uint16_t *wheel_buf = nullptr;
static int wheel_hue = 0;
static int wheel_sat = 100;
static int wheel_val = 100;

static int32_t slot_ams_id = 0;
static int32_t slot_tray_id = 0;
static uint32_t chosen_color = 0xFFFFFF;
static int chosen_index = -1;
static char slot_title[40] = "";       // "AMS-A - Fach 1"
static char filter_material[12] = ""; // leer = alle
static bool waiting_for_write = false;

static void rebuild_list();
static void rebuild_chips();
static void update_color_row();
static void update_apply_state();
static void update_title();
static void update_preview();
static void wheel_open();

// ============================================================
// Kleinkram
// ============================================================

static uint32_t contrast_color(uint32_t color)
{
    const uint32_t r = (color >> 16) & 0xFF;
    const uint32_t g = (color >> 8) & 0xFF;
    const uint32_t b = color & 0xFF;
    return (r * 299 + g * 587 + b * 114) > 150000 ? 0x111111 : 0xFFFFFF;
}

static uint32_t color_from_hsv(int h, int s, int v)
{
    const lv_color_t c = lv_color_hsv_to_rgb((uint16_t)h, (uint8_t)s, (uint8_t)v);
    return ((uint32_t)c.red << 16) | ((uint32_t)c.green << 8) | c.blue;
}

static void style_button(lv_obj_t *btn, uint32_t color)
{
    lv_obj_set_style_bg_color(btn, lv_color_hex(color), 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
}

// ============================================================
// Farbrad
// ============================================================

static void wheel_draw()
{
    // Das Rad wird einmal in voller Helligkeit gezeichnet. Der Regler
    // darunter aendert nur die gewaehlte Farbe, nicht das Bild — ein
    // Neuzeichnen bei jeder Reglerbewegung waere auf diesem Board deutlich
    // sichtbar, weil es dem Panel Speicherbandbreite wegnimmt.
    const int center = WHEEL_SIZE / 2;
    const lv_color_t bg = lv_color_hex(0x101418);

    for (int y = 0; y < WHEEL_SIZE; y++) {
        const int dy = y - center;
        for (int x = 0; x < WHEEL_SIZE; x++) {
            const int dx = x - center;
            const float dist = sqrtf((float)(dx * dx + dy * dy));

            if (dist > WHEEL_RADIUS) {
                lv_canvas_set_px(wheel_canvas, x, y, bg, LV_OPA_COVER);
                continue;
            }

            int hue = (int)(atan2f((float)dy, (float)dx) * 180.0f / (float)M_PI);
            if (hue < 0) hue += 360;
            const int sat = (int)(dist * 100.0f / WHEEL_RADIUS);

            lv_canvas_set_px(wheel_canvas, x, y, lv_color_hsv_to_rgb((uint16_t)hue,
                                                                     (uint8_t)sat, 100),
                             LV_OPA_COVER);
        }
    }
}

static void wheel_update_preview()
{
    const uint32_t color = color_from_hsv(wheel_hue, wheel_sat, wheel_val);
    ui_set_bg_color(wheel_preview, color);
}

static void wheel_touch_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;

    lv_point_t point;
    lv_indev_get_point(indev, &point);

    lv_area_t area;
    lv_obj_get_coords((lv_obj_t *)lv_event_get_target(e), &area);

    const int dx = point.x - (area.x1 + WHEEL_SIZE / 2);
    const int dy = point.y - (area.y1 + WHEEL_SIZE / 2);
    const float dist = sqrtf((float)(dx * dx + dy * dy));
    if (dist > WHEEL_RADIUS) return;

    int hue = (int)(atan2f((float)dy, (float)dx) * 180.0f / (float)M_PI);
    if (hue < 0) hue += 360;

    wheel_hue = hue;
    wheel_sat = (int)(dist * 100.0f / WHEEL_RADIUS);
    wheel_update_preview();
}

static void wheel_slider_cb(lv_event_t *e)
{
    wheel_val = (int)lv_slider_get_value((lv_obj_t *)lv_event_get_target(e));
    wheel_update_preview();
}

static void wheel_close()
{
    if (!wheel_overlay) return;

    // Asynchron loeschen: Wir stecken im Klick-Callback eines Kindes.
    lv_obj_delete_async(wheel_overlay);
    wheel_overlay = nullptr;
    wheel_canvas = nullptr;
    wheel_preview = nullptr;
    wheel_slider = nullptr;
}

static void wheel_cancel_cb(lv_event_t *)
{
    wheel_close();
}

static void wheel_ok_cb(lv_event_t *)
{
    chosen_color = color_from_hsv(wheel_hue, wheel_sat, wheel_val);
    wheel_close();
    update_color_row();
    update_preview();
}

static void wheel_open()
{
    if (wheel_overlay) return;

    if (!wheel_buf) {
        // 200x200 in RGB565 sind 80 KB — die gehoeren ins PSRAM, der interne
        // Speicher waere damit zu einem guten Teil belegt.
        wheel_buf = (uint16_t *)ps_malloc(
            LV_CANVAS_BUF_SIZE(WHEEL_SIZE, WHEEL_SIZE, 16, LV_DRAW_BUF_STRIDE_ALIGN));
        if (!wheel_buf) return;
    }

    ui_watch("farbrad:oeffnen");

    wheel_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(wheel_overlay, SCREEN_W, SCREEN_H);
    lv_obj_align(wheel_overlay, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_remove_flag(wheel_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(wheel_overlay, 0, 0);
    lv_obj_set_style_border_width(wheel_overlay, 0, 0);
    lv_obj_set_style_pad_all(wheel_overlay, 0, 0);
    lv_obj_set_style_bg_color(wheel_overlay, lv_color_hex(0x101418), 0);
    lv_obj_set_style_bg_opa(wheel_overlay, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(wheel_overlay);
    lv_label_set_text(title, "Eigene Farbe");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 14);

    wheel_canvas = lv_canvas_create(wheel_overlay);
    lv_canvas_set_buffer(wheel_canvas, wheel_buf, WHEEL_SIZE, WHEEL_SIZE,
                         LV_COLOR_FORMAT_RGB565);
    lv_obj_set_size(wheel_canvas, WHEEL_SIZE, WHEEL_SIZE);
    lv_obj_align(wheel_canvas, LV_ALIGN_TOP_MID, 0, 48);
    lv_obj_add_flag(wheel_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(wheel_canvas, wheel_touch_cb, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(wheel_canvas, wheel_touch_cb, LV_EVENT_CLICKED, nullptr);
    wheel_draw();

    lv_obj_t *slider_lbl = lv_label_create(wheel_overlay);
    lv_label_set_text(slider_lbl, "Helligkeit");
    lv_obj_set_style_text_color(slider_lbl, lv_color_hex(COL_MUTED), 0);
    lv_obj_align(slider_lbl, LV_ALIGN_TOP_LEFT, PAD + 4, 266);

    wheel_slider = lv_slider_create(wheel_overlay);
    lv_obj_set_size(wheel_slider, CONTENT_W - 8, 16);
    lv_obj_align(wheel_slider, LV_ALIGN_TOP_MID, 0, 292);
    lv_slider_set_range(wheel_slider, 10, 100);
    lv_slider_set_value(wheel_slider, wheel_val, LV_ANIM_OFF);
    lv_obj_add_event_cb(wheel_slider, wheel_slider_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    wheel_preview = lv_obj_create(wheel_overlay);
    lv_obj_set_size(wheel_preview, 96, 56);
    lv_obj_align(wheel_preview, LV_ALIGN_TOP_LEFT, PAD, 332);
    lv_obj_remove_flag(wheel_preview, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(wheel_preview, 10, 0);
    lv_obj_set_style_border_width(wheel_preview, 2, 0);
    lv_obj_set_style_border_color(wheel_preview, lv_color_hex(0x555555), 0);
    wheel_update_preview();

    lv_obj_t *cancel = lv_button_create(wheel_overlay);
    lv_obj_set_size(cancel, 150, 56);
    lv_obj_align(cancel, LV_ALIGN_TOP_LEFT, PAD + 104, 332);
    style_button(cancel, COL_NEUTRAL);
    lv_obj_add_event_cb(cancel, wheel_cancel_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *cancel_lbl = lv_label_create(cancel);
    lv_label_set_text(cancel_lbl, "Abbrechen");
    lv_obj_center(cancel_lbl);

    lv_obj_t *ok = lv_button_create(wheel_overlay);
    lv_obj_set_size(ok, CONTENT_W - 104 - 158, 56);
    lv_obj_align(ok, LV_ALIGN_TOP_RIGHT, -PAD, 332);
    style_button(ok, COL_OK);
    lv_obj_add_event_cb(ok, wheel_ok_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *ok_lbl = lv_label_create(ok);
    lv_label_set_text(ok_lbl, "Uebernehmen");
    lv_obj_center(ok_lbl);
}

// ============================================================
// Farbzeile
// ============================================================

static void quick_color_cb(lv_event_t *e)
{
    chosen_color = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    update_color_row();
    update_preview();
}

static void custom_color_cb(lv_event_t *)
{
    wheel_open();
}

static void update_color_row()
{
    if (!color_row) return;

    // Die gewaehlte Farbe bekommt einen hellen Rahmen. Bei Schwarz auf
    // dunklem Grund waere sie sonst nicht von einem leeren Feld zu
    // unterscheiden.
    const uint32_t child_count = lv_obj_get_child_count(color_row);
    for (uint32_t i = 0; i < child_count; i++) {
        lv_obj_t *swatch = lv_obj_get_child(color_row, i);
        const bool is_custom = swatch == custom_btn;
        bool selected = false;

        if (!is_custom) {
            const uint32_t color = (uint32_t)(uintptr_t)lv_obj_get_user_data(swatch);
            selected = color == chosen_color;
        } else {
            selected = true;
            for (int q = 0; q < QUICK_COLOR_COUNT; q++) {
                if (QUICK_COLORS[q] == chosen_color) selected = false;
            }
            ui_set_bg_color(swatch, chosen_color);
        }

        lv_obj_set_style_border_width(swatch, selected ? 3 : 1, 0);
        lv_obj_set_style_border_color(
            swatch, lv_color_hex(selected ? COL_ACCENT : 0x555555), 0);
    }
}

// ============================================================
// Profilliste
// ============================================================

// Auswahl nur umfaerben, nicht neu aufbauen.
//
// Wir stecken hier im Klick-Callback einer Zeile. Ein lv_obj_clean() wuerde
// genau diese Zeile loeschen, waehrend LVGL noch ihr Ereignis abarbeitet —
// der naechste Zugriff geht auf freigegebenen Speicher, und das Geraet
// startet neu. Dieselbe Falle wie damals beim Muelleimer-Symbol im Archiv.
static void update_list_selection()
{
    if (!list_cont) return;

    const uint32_t rows = lv_obj_get_child_count(list_cont);
    for (uint32_t i = 0; i < rows; i++) {
        lv_obj_t *row = lv_obj_get_child(list_cont, i);
        const int index = (int)(intptr_t)lv_obj_get_user_data(row);
        const bool selected = index == chosen_index;

        lv_obj_set_style_border_width(row, selected ? 2 : 0, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(COL_ACCENT), 0);
    }
}

static void preset_cb(lv_event_t *e)
{
    chosen_index = (int)(intptr_t)lv_event_get_user_data(e);
    update_list_selection();
    update_preview();
    update_apply_state();
}

// Ebenso fuer die Materialknoepfe: Der angetippte Knopf bleibt stehen, nur
// seine Farbe wechselt. Die Profilliste darunter darf neu gebaut werden —
// sie liegt in einem anderen Behaelter als der Knopf, der gerade das
// Ereignis traegt.
static void update_chip_selection()
{
    if (!chips_cont) return;

    const uint32_t count = lv_obj_get_child_count(chips_cont);
    for (uint32_t i = 0; i < count; i++) {
        lv_obj_t *chip = lv_obj_get_child(chips_cont, i);
        const char *value = (const char *)lv_obj_get_user_data(chip);
        const bool active = strcmp(filter_material, value ? value : "") == 0;
        ui_set_bg_color(chip, active ? COL_ACCENT : 0x2A3038);
    }
}

static void chip_cb(lv_event_t *e)
{
    const char *material = (const char *)lv_event_get_user_data(e);
    strncpy(filter_material, material ? material : "", sizeof(filter_material) - 1);
    filter_material[sizeof(filter_material) - 1] = '\0';

    update_chip_selection();
    rebuild_list();
}

static bool passes_filter(const bambuddy_filament_preset_t &p)
{
    return !filter_material[0] || strcmp(p.material, filter_material) == 0;
}

static void rebuild_chips()
{
    if (!chips_cont) return;
    lv_obj_clean(chips_cont);

    // "Alle" plus jedes Material, das in der Liste wirklich vorkommt. Eine
    // feste Materialliste haette Knoepfe gezeigt, hinter denen nichts liegt.
    static char materials[16][12];
    int material_count = 0;

    const int count = bambuddy_filament_count();
    for (int i = 0; i < count && material_count < 16; i++) {
        bambuddy_filament_preset_t p;
        if (!bambuddy_filament_get(i, &p) || !p.material[0]) continue;

        bool known = false;
        for (int m = 0; m < material_count; m++) {
            if (strcmp(materials[m], p.material) == 0) known = true;
        }
        if (!known) {
            strncpy(materials[material_count], p.material, sizeof(materials[0]) - 1);
            materials[material_count][sizeof(materials[0]) - 1] = '\0';
            material_count++;
        }
    }

    for (int i = -1; i < material_count; i++) {
        const char *label = i < 0 ? "Alle" : materials[i];
        const char *value = i < 0 ? "" : materials[i];
        const bool active = i < 0 ? filter_material[0] == '\0'
                                  : strcmp(filter_material, materials[i]) == 0;

        lv_obj_t *chip = lv_button_create(chips_cont);
        lv_obj_set_size(chip, LV_SIZE_CONTENT, CHIPS_H - 4);
        lv_obj_set_style_pad_hor(chip, 14, 0);
        style_button(chip, active ? COL_ACCENT : 0x2A3038);
        lv_obj_set_user_data(chip, (void *)value);
        lv_obj_add_event_cb(chip, chip_cb, LV_EVENT_CLICKED, (void *)value);

        lv_obj_t *lbl = lv_label_create(chip);
        lv_label_set_text(lbl, label);
        lv_obj_center(lbl);
    }
}

static void rebuild_list()
{
    if (!list_cont) return;
    lv_obj_clean(list_cont);

    if (!bambuddy_filament_ready()) {
        ui_set_text(hint_lbl, bambuddy_filament_message()[0]
                                  ? bambuddy_filament_message()
                                  : "Profile werden geladen ...");
        lv_obj_remove_flag(hint_lbl, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_add_flag(hint_lbl, LV_OBJ_FLAG_HIDDEN);

    const int count = bambuddy_filament_count();
    for (int i = 0; i < count; i++) {
        bambuddy_filament_preset_t p;
        if (!bambuddy_filament_get(i, &p) || !passes_filter(p)) continue;

        const bool selected = i == chosen_index;

        lv_obj_t *row = lv_obj_create(list_cont);
        lv_obj_set_size(row, LV_PCT(100), ROW_H);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_border_width(row, selected ? 2 : 0, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(COL_ACCENT), 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(row, (void *)(intptr_t)i);
        lv_obj_add_event_cb(row, preset_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *name = lv_label_create(row);
        lv_label_set_text(name, p.name);
        lv_obj_set_width(name, CONTENT_W - 130);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 14, 0);

        // Herkunft als Abzeichen — welches Profil woher kommt, entscheidet,
        // ob eigene Temperaturen gelten oder die Materialtabelle.
        lv_obj_t *badge = lv_label_create(row);
        lv_label_set_text(badge, p.local ? "Lokal" : "Integriert");
        lv_obj_set_style_text_font(badge, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(badge, lv_color_hex(p.local ? COL_OK : COL_WARN), 0);
        lv_obj_align(badge, LV_ALIGN_RIGHT_MID, -14, 0);
    }
}

// ============================================================
// Fusszeile
// ============================================================

// Kopfzeile: Slot und, in Klammern, das derzeit hinterlegte Profil. Ohne
// diese Angabe sieht man dem Slot nicht an, was ueberhaupt eingestellt ist —
// die Liste zeigt nur, was man waehlen koennte.
//
// Der Name kommt aus den Slot-Zuordnungen, die erst mit der Profilliste
// eintreffen. Beim ersten Oeffnen steht deshalb kurz nur der Slot da, und
// die Klammer kommt nach.
static void update_title()
{
    if (!title_lbl) return;

    const char *current = bambuddy_filament_slot_preset_name(slot_ams_id, slot_tray_id);
    if (current && current[0]) {
        ui_set_text_fmt(title_lbl, "%s (%s)", slot_title, current);
    } else {
        ui_set_text(title_lbl, slot_title);
    }
}

// Was ginge beim Antippen von "Konfigurieren" an den Drucker? Farbe als
// Punkt, dahinter Material und die Duesentemperaturen. Gerade die
// Temperaturen gibt niemand ein — ohne diese Zeile waere der einzige Wert,
// den das Geraet selbst bestimmt, auch der einzige, den niemand zu sehen
// bekommt, bevor er beim Drucker liegt.
static void update_preview()
{
    if (!preview_lbl || !preview_dot) return;

    ui_set_bg_color(preview_dot, chosen_color);

    bambuddy_filament_preset_t p;
    if (chosen_index < 0 || !bambuddy_filament_get(chosen_index, &p)) {
        ui_set_text(preview_lbl, "Profil waehlen");
        ui_set_text_color(preview_lbl, COL_MUTED);
        return;
    }

    int16_t lo = 0;
    int16_t hi = 0;
    bambuddy_filament_effective_temps(p, &lo, &hi);

    ui_set_text_fmt(preview_lbl, "%s  %d-%d C", p.material, (int)lo, (int)hi);
    ui_set_text_color(preview_lbl, COL_ACCENT);
}

static void update_apply_state()
{
    if (!apply_btn) return;

    const bool ready = chosen_index >= 0 && !bambuddy_filament_busy();
    if (ready) {
        lv_obj_remove_state(apply_btn, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(apply_btn, LV_STATE_DISABLED);
    }
    ui_set_text(apply_lbl, bambuddy_filament_busy() ? "Sende ..." : "Konfigurieren");
}

static void close_view()
{
    if (!overlay) return;

    if (ui_timer) {
        lv_timer_delete(ui_timer);
        ui_timer = nullptr;
    }
    wheel_close();

    lv_obj_delete_async(overlay);
    overlay = nullptr;
    title_lbl = nullptr;
    preview_lbl = nullptr;
    preview_dot = nullptr;
    list_cont = nullptr;
    chips_cont = nullptr;
    hint_lbl = nullptr;
    color_row = nullptr;
    custom_btn = nullptr;
    apply_btn = nullptr;
    apply_lbl = nullptr;

    bambuddy_filament_set_visible(false);
}

static void close_cb(lv_event_t *)
{
    close_view();
}

static void apply_cb(lv_event_t *)
{
    if (chosen_index < 0 || bambuddy_filament_busy()) return;

    bambuddy_filament_request_configure(slot_ams_id, slot_tray_id, chosen_index,
                                        chosen_color);
    waiting_for_write = true;
    update_apply_state();
}

static void reset_cb(lv_event_t *)
{
    if (bambuddy_filament_busy()) return;

    bambuddy_filament_request_reset(slot_ams_id, slot_tray_id);
    waiting_for_write = true;
    update_apply_state();
}

// ============================================================
// Aufbau
// ============================================================

static void tick_cb(lv_timer_t *)
{
    if (bambuddy_filament_take_fresh()) {
        if (chosen_index < 0) {
            chosen_index = bambuddy_filament_slot_preset_index(slot_ams_id, slot_tray_id);
        }
        rebuild_chips();
        rebuild_list();
        update_title();
        update_preview();
    }

    update_apply_state();

    // Nach einem erfolgreichen Schreiben schliesst sich die Ansicht von
    // selbst — der Slot ist gesetzt, es gibt hier nichts mehr zu tun.
    if (waiting_for_write && !bambuddy_filament_busy()) {
        waiting_for_write = false;
        close_view();
    }
}

static void build(const char *slot_label)
{
    overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(overlay, SCREEN_W, SCREEN_H);
    lv_obj_align(overlay, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(overlay, 0, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_pad_all(overlay, 0, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(0x101418), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);

    title_lbl = lv_label_create(overlay);
    lv_label_set_text(title_lbl, slot_label);
    lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_16, 0);
    // Breite begrenzen und kuerzen lassen: "AMS-A - Fach 1 (Bambu Support
    // For PLA/PETG)" waere sonst breiter als der Bildschirm und schoebe sich
    // unter den Schliessen-Knopf.
    lv_obj_set_width(title_lbl, SCREEN_W - (PAD + 4) - (44 + PAD) - 8);
    lv_label_set_long_mode(title_lbl, LV_LABEL_LONG_DOT);
    lv_obj_align(title_lbl, LV_ALIGN_TOP_LEFT, PAD + 4, 8);

    // Farbpunkt und Vorschautext unter dem Titel. Beides passt noch in die
    // Kopfzeile, weil der Titel dafuer nach oben gerueckt ist.
    preview_dot = lv_obj_create(overlay);
    lv_obj_set_size(preview_dot, 14, 14);
    lv_obj_align(preview_dot, LV_ALIGN_TOP_LEFT, PAD + 4, 30);
    lv_obj_remove_flag(preview_dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(preview_dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(preview_dot, 7, 0);
    lv_obj_set_style_pad_all(preview_dot, 0, 0);
    lv_obj_set_style_border_width(preview_dot, 1, 0);
    lv_obj_set_style_border_color(preview_dot, lv_color_hex(0x555555), 0);

    preview_lbl = lv_label_create(overlay);
    lv_obj_set_style_text_font(preview_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_width(preview_lbl, SCREEN_W - (PAD + 26) - (44 + PAD) - 8);
    lv_label_set_long_mode(preview_lbl, LV_LABEL_LONG_DOT);
    lv_obj_align(preview_lbl, LV_ALIGN_TOP_LEFT, PAD + 26, 31);

    lv_obj_t *close = lv_button_create(overlay);
    lv_obj_set_size(close, 44, HEADER_H - 8);
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, -PAD, 4);
    style_button(close, COL_NEUTRAL);
    lv_obj_add_event_cb(close, close_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *close_lbl = lv_label_create(close);
    lv_label_set_text(close_lbl, LV_SYMBOL_CLOSE);
    lv_obj_center(close_lbl);

    // Materialfilter: 86 integrierte Profile sind sonst nur mit langem
    // Wischen erreichbar.
    chips_cont = lv_obj_create(overlay);
    lv_obj_set_size(chips_cont, CONTENT_W, CHIPS_H);
    lv_obj_align(chips_cont, LV_ALIGN_TOP_MID, 0, CHIPS_Y);
    lv_obj_set_style_bg_opa(chips_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(chips_cont, 0, 0);
    lv_obj_set_style_pad_all(chips_cont, 0, 0);
    lv_obj_set_style_pad_column(chips_cont, 6, 0);
    lv_obj_set_flex_flow(chips_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_scroll_dir(chips_cont, LV_DIR_HOR);

    list_cont = lv_obj_create(overlay);
    lv_obj_set_size(list_cont, CONTENT_W, LIST_HEIGHT);
    lv_obj_align(list_cont, LV_ALIGN_TOP_MID, 0, LIST_Y);
    lv_obj_set_style_bg_opa(list_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list_cont, 0, 0);
    lv_obj_set_style_pad_all(list_cont, 0, 0);
    lv_obj_set_style_pad_row(list_cont, 6, 0);
    lv_obj_set_flex_flow(list_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list_cont, LV_DIR_VER);

    hint_lbl = lv_label_create(overlay);
    lv_label_set_text(hint_lbl, "Profile werden geladen ...");
    lv_obj_set_style_text_color(hint_lbl, lv_color_hex(COL_MUTED), 0);
    lv_obj_align(hint_lbl, LV_ALIGN_TOP_MID, 0, LIST_Y + LIST_HEIGHT / 2 - 10);

    color_row = lv_obj_create(overlay);
    lv_obj_set_size(color_row, CONTENT_W, COLOR_H);
    lv_obj_align(color_row, LV_ALIGN_TOP_MID, 0, COLOR_Y);
    lv_obj_remove_flag(color_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(color_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(color_row, 0, 0);
    lv_obj_set_style_pad_all(color_row, 0, 0);
    lv_obj_set_style_pad_column(color_row, 8, 0);
    lv_obj_set_flex_flow(color_row, LV_FLEX_FLOW_ROW);

    const int swatch_w = (CONTENT_W - QUICK_COLOR_COUNT * 8) / (QUICK_COLOR_COUNT + 1);
    for (int i = 0; i < QUICK_COLOR_COUNT; i++) {
        lv_obj_t *swatch = lv_obj_create(color_row);
        lv_obj_set_size(swatch, swatch_w, COLOR_H);
        lv_obj_remove_flag(swatch, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_radius(swatch, 10, 0);
        lv_obj_set_style_pad_all(swatch, 0, 0);
        lv_obj_set_style_bg_color(swatch, lv_color_hex(QUICK_COLORS[i]), 0);
        lv_obj_set_user_data(swatch, (void *)(uintptr_t)QUICK_COLORS[i]);
        lv_obj_add_flag(swatch, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(swatch, quick_color_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)QUICK_COLORS[i]);
    }

    custom_btn = lv_obj_create(color_row);
    lv_obj_set_size(custom_btn, swatch_w, COLOR_H);
    lv_obj_remove_flag(custom_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(custom_btn, 10, 0);
    lv_obj_set_style_pad_all(custom_btn, 0, 0);
    lv_obj_add_flag(custom_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(custom_btn, custom_color_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *custom_lbl = lv_label_create(custom_btn);
    lv_label_set_text(custom_lbl, LV_SYMBOL_PLUS);
    lv_obj_set_style_text_color(custom_lbl, lv_color_hex(contrast_color(chosen_color)), 0);
    lv_obj_center(custom_lbl);

    lv_obj_t *reset = lv_button_create(overlay);
    lv_obj_set_size(reset, 130, FOOT_H);
    lv_obj_align(reset, LV_ALIGN_TOP_LEFT, PAD, FOOT_Y);
    style_button(reset, COL_ERR);
    lv_obj_add_event_cb(reset, reset_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *reset_lbl = lv_label_create(reset);
    lv_label_set_text(reset_lbl, "Leeren");
    lv_obj_center(reset_lbl);

    lv_obj_t *cancel = lv_button_create(overlay);
    lv_obj_set_size(cancel, 110, FOOT_H);
    lv_obj_align(cancel, LV_ALIGN_TOP_LEFT, PAD + 138, FOOT_Y);
    style_button(cancel, COL_NEUTRAL);
    lv_obj_add_event_cb(cancel, close_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *cancel_lbl = lv_label_create(cancel);
    lv_label_set_text(cancel_lbl, "Abbrechen");
    lv_obj_center(cancel_lbl);

    apply_btn = lv_button_create(overlay);
    lv_obj_set_size(apply_btn, CONTENT_W - 138 - 118, FOOT_H);
    lv_obj_align(apply_btn, LV_ALIGN_TOP_RIGHT, -PAD, FOOT_Y);
    style_button(apply_btn, COL_OK);
    lv_obj_add_event_cb(apply_btn, apply_cb, LV_EVENT_CLICKED, nullptr);
    apply_lbl = lv_label_create(apply_btn);
    lv_label_set_text(apply_lbl, "Konfigurieren");
    lv_obj_center(apply_lbl);

    ui_timer = lv_timer_create(tick_cb, 300, nullptr);
    lv_timer_set_repeat_count(ui_timer, -1);
}

void filament_config_open(int32_t ams_id, int32_t tray_id, const char *slot_label,
                          uint32_t current_color)
{
    if (overlay) return;

    ui_watch("filament:oeffnen");

    slot_ams_id = ams_id;
    slot_tray_id = tray_id;
    // Bewusst ohne Ausweichwert: 0x000000 ist Schwarz, eine gueltige
    // Filamentfarbe. Wer hier auf "nicht gesetzt" pruefen wollte, machte aus
    // jeder schwarzen Spule eine weisse. Der AMS-Screen entscheidet, was er
    // bei leerem Fach mitgibt.
    chosen_color = current_color;
    filter_material[0] = '\0';
    waiting_for_write = false;
    strncpy(slot_title, slot_label ? slot_label : "AMS", sizeof(slot_title) - 1);
    slot_title[sizeof(slot_title) - 1] = '\0';

    bambuddy_filament_set_visible(true);
    chosen_index = bambuddy_filament_slot_preset_index(ams_id, tray_id);

    build(slot_label);
    rebuild_chips();
    rebuild_list();
    update_color_row();
    update_title();
    update_preview();
    update_apply_state();
}

bool filament_config_is_open()
{
    return overlay != nullptr;
}
