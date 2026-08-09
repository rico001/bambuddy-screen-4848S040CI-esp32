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
// Kopfbereich: Titel, darunter eine Zeile fuers Filament und eine fuer die
// Farbe. Die Liste rueckt entsprechend nach unten.
static constexpr int HEADER_H = 44;
static constexpr int FILAMENT_Y = 28;
static constexpr int COLOR_LINE_Y = 48;
static constexpr int TEMP_LINE_Y = 68;
static constexpr int CHIPS_Y = 90;
static constexpr int CHIPS_H = 36;
static constexpr int LIST_Y = 132;
// Nicht LIST_H nennen: Das ist anderswo ein Header-Guard und damit
// ein Makro, das diese Zeile zerlegt, bevor der Compiler sie sieht.
static constexpr int LIST_HEIGHT = 208;
static constexpr int COLOR_Y = 348;
static constexpr int COLOR_H = 44;
static constexpr int FOOT_Y = 400;
static constexpr int FOOT_H = 54;

static constexpr int ROW_H = 44;

// Hoechstzahl gleichzeitig dargestellter Profile.
//
// Jede Zeile kostet einen Behaelter und zwei Beschriftungen, und LVGL legt
// solche kleinen Objekte im internen RAM an — nur Bloecke ueber 4 KB gehen
// ins PSRAM. Alle 87 Profile auf einmal haben rund 70 KB belegt und den
// internen Speicher bis auf wenige hundert Byte geleert; danach bekam der
// TCP-Stack keine Puffer mehr, und jeder Schreibvorgang lief in einen
// Timeout. Der Materialfilter oben ist deshalb nicht nur Bequemlichkeit,
// sondern die Bedingung dafuer, dass der Rest funktioniert.
// PLA ist mit 25 Eintraegen die groesste Materialgruppe — die Grenze liegt
// knapp darueber, damit fuer sie keine Hinweiszeile noetig wird.
static constexpr int MAX_ROWS = 28;

// 27 verschiedene Materialien kommen in der Liste vor. Zu wenige Plaetze
// hiesse: Profile, deren Material keinen Knopf bekommt, sind gar nicht mehr
// erreichbar — es gibt kein "Alle" mehr, das sie einfangen wuerde.
static constexpr int MATERIAL_SLOTS = 32;

// Fuenf feste Farben decken den Alltag ab; alles andere kommt aus dem Rad.
static const uint32_t QUICK_COLORS[] = {0x000000, 0xFFFFFF, 0xE53935, 0x43A047, 0x1E88E5};
static constexpr int QUICK_COLOR_COUNT = sizeof(QUICK_COLORS) / sizeof(QUICK_COLORS[0]);

static constexpr int WHEEL_SIZE = 200;
static constexpr int WHEEL_RADIUS = 96;

static lv_obj_t *overlay = nullptr;
static lv_obj_t *title_lbl = nullptr;
static lv_obj_t *preview_lbl = nullptr;
static lv_obj_t *preview_dot_old = nullptr;
static lv_obj_t *preview_dot_new = nullptr;
static lv_obj_t *preview_arrow = nullptr;
static lv_obj_t *color_caption = nullptr;
static lv_obj_t *type_caption = nullptr;
static lv_obj_t *temp_caption = nullptr;
static lv_obj_t *temp_lbl = nullptr;
static lv_obj_t *list_cont = nullptr;
static lv_obj_t *chips_cont = nullptr;
static lv_obj_t *hint_lbl = nullptr;
static lv_obj_t *color_row = nullptr;
static lv_obj_t *custom_btn = nullptr;
static lv_obj_t *custom_lbl = nullptr;
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
static char slot_type[16] = "";        // gemeldeter Typ der eingelegten Spule
static char current_name[48] = "";     // was bisher im Fach steckt
static uint32_t current_color_rgb = 0; // und in welcher Farbe
static int16_t current_temp_min = 0;   // und mit welchen Duesentemperaturen
static int16_t current_temp_max = 0;
static bool selection_touched = false; // hat der Benutzer selbst etwas gewaehlt?
static char filter_material[12] = ""; // leer = alle
static bool filter_touched = false;   // hat der Benutzer selbst gefiltert?
static bool waiting_for_write = false;

static void rebuild_list();
static void rebuild_chips();
static void update_color_row();
static void update_apply_state();
static void update_title();
static void update_preview();
static int generic_index_for(const char *material);
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

static lv_obj_t *make_dot(lv_obj_t *parent, int x, int y)
{
    lv_obj_t *dot = lv_obj_create(parent);
    lv_obj_set_size(dot, 14, 14);
    lv_obj_align(dot, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(dot, 7, 0);
    lv_obj_set_style_pad_all(dot, 0, 0);
    lv_obj_set_style_border_width(dot, 1, 0);
    lv_obj_set_style_border_color(dot, lv_color_hex(0x555555), 0);
    return dot;
}

static void style_button(lv_obj_t *btn, uint32_t color)
{
    lv_obj_set_style_bg_color(btn, lv_color_hex(color), 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
}

// Bausteine des Kopfbereichs und der Fusszeile.
//
// Ohne sie stand jeder Knopf und jede Beschriftung als eigener Block aus
// fuenf bis sieben fast gleichen Zeilen da — sechs Knoepfe, vier
// Beschriftungen, sechs Farbfelder. Solche Kopien laufen beim naechsten
// Anpassen auseinander: Man aendert die Schriftgroesse an drei Stellen und
// uebersieht die vierte.

// Gedaempfte Beschriftung ("Typ", "Farbe", "Temp", Pfeil).
static lv_obj_t *make_caption(lv_obj_t *parent, const char *text, int x, int y)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(COL_MUTED), 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, x, y);
    return lbl;
}

// Wertzeile daneben. Feste Breite und Kuerzen statt Umbrechen: Eine zweite
// Zeile wuerde in die naechste Kopfzeile hineinlaufen.
static lv_obj_t *make_value(lv_obj_t *parent, int x, int y, int width)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_width(lbl, width);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, x, y);
    return lbl;
}

// Knopf mit zentrierter Beschriftung. Die Beschriftung ist sein erstes Kind
// — wer sie spaeter aendern will, holt sie mit lv_obj_get_child(btn, 0).
static lv_obj_t *make_button(lv_obj_t *parent, int w, int h, lv_align_t align, int dx,
                             int dy, uint32_t color, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_align(btn, align, dx, dy);
    style_button(btn, color);
    if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_center(lbl);
    return btn;
}

// Farbfeld der Farbzeile. Bewusst ein lv_obj und kein Knopf: Der Knopf-Stil
// des Themes legt sich sonst ueber die Farbe, die hier die ganze Aussage ist.
static lv_obj_t *make_swatch(lv_obj_t *parent, int w, int h)
{
    lv_obj_t *swatch = lv_obj_create(parent);
    lv_obj_set_size(swatch, w, h);
    lv_obj_remove_flag(swatch, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(swatch, 10, 0);
    lv_obj_set_style_pad_all(swatch, 0, 0);
    lv_obj_add_flag(swatch, LV_OBJ_FLAG_CLICKABLE);
    return swatch;
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
    selection_touched = true;
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

    make_button(wheel_overlay, 150, 56, LV_ALIGN_TOP_LEFT, PAD + 104, 332, COL_NEUTRAL,
                "Abbrechen", wheel_cancel_cb);
    make_button(wheel_overlay, CONTENT_W - 104 - 158, 56, LV_ALIGN_TOP_RIGHT, -PAD, 332,
                COL_OK, "Uebernehmen", wheel_ok_cb);
}

// ============================================================
// Farbzeile
// ============================================================

static void quick_color_cb(lv_event_t *e)
{
    chosen_color = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    selection_touched = true;
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
            // Auch das Zeichen umfaerben, nicht nur den Grund. Es bekam
            // seine Farbe frueher einmalig beim Aufbau — auf einer hellen
            // Wunschfarbe verschwand das weisse Plus danach spurlos.
            ui_set_text_color(custom_lbl, contrast_color(chosen_color));
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
    selection_touched = true;
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

    filter_touched = true;
    update_chip_selection();
    rebuild_list();
}

// "Generic PLA" bevorzugen, sonst das erste Profil dieses Materials. Der
// generische Eintrag ist der, den auch der Drucker von sich aus meldet.
static int generic_index_for(const char *material)
{
    const int count = bambuddy_filament_count();
    int fallback = -1;

    for (int i = 0; i < count; i++) {
        bambuddy_filament_preset_t p;
        if (!bambuddy_filament_get(i, &p)) continue;
        if (strcmp(p.material, material) != 0) continue;

        if (strncmp(p.name, "Generic ", 8) == 0) return i;
        if (fallback < 0) fallback = i;
    }
    return fallback;
}

static bool passes_filter(const bambuddy_filament_preset_t &p)
{
    return !filter_material[0] || strcmp(p.material, filter_material) == 0;
}

static void rebuild_chips()
{
    if (!chips_cont) return;
    lv_obj_clean(chips_cont);

    // Jedes Material, das in der Liste wirklich vorkommt. Eine feste
    // Materialliste haette Knoepfe gezeigt, hinter denen nichts liegt.
    static char materials[MATERIAL_SLOTS][12];
    int material_count = 0;

    const int count = bambuddy_filament_count();
    for (int i = 0; i < count && material_count < MATERIAL_SLOTS; i++) {
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

    // Steht im Filter ein Material, das es gar nicht gibt, waere die Liste
    // dauerhaft leer und kein Knopf hervorgehoben. Dann auf das erste
    // vorhandene ausweichen.
    bool filter_exists = false;
    for (int i = 0; i < material_count; i++) {
        if (strcmp(filter_material, materials[i]) == 0) filter_exists = true;
    }
    if (!filter_exists && material_count > 0) {
        strncpy(filter_material, materials[0], sizeof(filter_material) - 1);
        filter_material[sizeof(filter_material) - 1] = '\0';
    }

    for (int i = 0; i < material_count; i++) {
        const char *label = materials[i];
        const char *value = materials[i];
        const bool active = strcmp(filter_material, materials[i]) == 0;

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
    int shown = 0;
    int hidden = 0;

    for (int i = 0; i < count; i++) {
        bambuddy_filament_preset_t p;
        if (!bambuddy_filament_get(i, &p) || !passes_filter(p)) continue;

        // Ueber der Grenze nur noch zaehlen, nicht mehr bauen. Die
        // ausgewaehlte Zeile ist davon ausgenommen — sie muss sichtbar
        // bleiben, sonst sieht man seine eigene Wahl nicht mehr.
        if (shown >= MAX_ROWS && i != chosen_index) {
            hidden++;
            continue;
        }
        shown++;

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

    // Sagen, dass etwas fehlt, statt es stillschweigend wegzulassen. Wer
    // sein Profil nicht findet, soll wissen, wo es steckt.
    if (hidden > 0) {
        lv_obj_t *more = lv_obj_create(list_cont);
        lv_obj_set_size(more, LV_PCT(100), ROW_H);
        lv_obj_remove_flag(more, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(more, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_opa(more, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(more, 0, 0);
        lv_obj_set_style_pad_all(more, 0, 0);
        // Kein gueltiger Index: Ohne das gaebe lv_obj_get_user_data() eine
        // Null zurueck, und update_list_selection() hielte diese Zeile fuer
        // das Profil mit dem Index 0.
        lv_obj_set_user_data(more, (void *)(intptr_t)-1);

        lv_obj_t *lbl = lv_label_create(more);
        lv_label_set_text_fmt(lbl, "%d weitere Profile in diesem Material", hidden);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(COL_MUTED), 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 14, 0);
    }
}

// ============================================================
// Fusszeile
// ============================================================

// Kopfzeile: nur der Slot.
//
// Hier stand einmal zusaetzlich das hinterlegte Profil in Klammern. Das war
// doppelt — die Zeile darunter zeigt es ohnehin — und stammte aus der
// schlechteren Quelle: den in Bambuddy gemerkten Zuordnungen, die veralten
// koennen. Was wirklich im Fach steckt, sagt der Drucker.
static void update_title()
{
    if (!title_lbl) return;
    ui_set_text(title_lbl, slot_title);
}

// Was ginge beim Antippen von "Konfigurieren" an den Drucker? Farbe als
// Punkt, dahinter Material und die Duesentemperaturen. Gerade die
// Temperaturen gibt niemand ein — ohne diese Zeile waere der einzige Wert,
// den das Geraet selbst bestimmt, auch der einzige, den niemand zu sehen
// bekommt, bevor er beim Drucker liegt.
static void update_preview()
{
    if (!preview_lbl || !preview_dot_old || !preview_dot_new || !preview_arrow ||
        !temp_lbl) {
        return;
    }

    ui_set_bg_color(preview_dot_old, current_color_rgb);
    ui_set_bg_color(preview_dot_new, chosen_color);

    // Pfeil und zweiter Punkt gehoeren zusammen: Solange es nichts zu
    // vergleichen gibt, steht dort nur die aktuelle Farbe.
    if (selection_touched) {
        lv_obj_remove_flag(preview_arrow, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(preview_dot_new, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(preview_arrow, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(preview_dot_new, LV_OBJ_FLAG_HIDDEN);
    }

    char now_temp[24] = "";
    if (current_temp_min > 0 && current_temp_max > 0) {
        snprintf(now_temp, sizeof(now_temp), "%d-%d C", (int)current_temp_min,
                 (int)current_temp_max);
    }

    // Solange nichts angetippt wurde, steht hier nur der Ist-Zustand. Erst
    // wenn der Benutzer etwas waehlt, wird daraus "alt -> neu" — sonst
    // muesste man raten, was sich eigentlich aendert.
    bambuddy_filament_preset_t p;
    if (!selection_touched || chosen_index < 0 ||
        !bambuddy_filament_get(chosen_index, &p)) {
        ui_set_text(preview_lbl, current_name[0] ? current_name : "Fach leer");
        ui_set_text_color(preview_lbl, COL_MUTED);
        ui_set_text(temp_lbl, now_temp[0] ? now_temp : "unbekannt");
        ui_set_text_color(temp_lbl, COL_MUTED);
        return;
    }

    int16_t lo = 0;
    int16_t hi = 0;
    bambuddy_filament_effective_temps(p, &lo, &hi);

    // Eigener Puffer statt ui_set_text_fmt(): dessen 96 Zeichen reichen fuer
    // zwei Profilnamen nebeneinander nicht.
    char text[160];
    if (current_name[0]) {
        snprintf(text, sizeof(text), "%s " LV_SYMBOL_RIGHT " %s", current_name, p.name);
    } else {
        snprintf(text, sizeof(text), "%s", p.name);
    }
    ui_set_text(preview_lbl, text);
    ui_set_text_color(preview_lbl, COL_ACCENT);

    if (now_temp[0]) {
        ui_set_text_fmt(temp_lbl, "%s " LV_SYMBOL_RIGHT " %d-%d C", now_temp, (int)lo,
                        (int)hi);
    } else {
        ui_set_text_fmt(temp_lbl, "%d-%d C", (int)lo, (int)hi);
    }
    ui_set_text_color(temp_lbl, COL_ACCENT);
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
    preview_dot_old = nullptr;
    preview_dot_new = nullptr;
    preview_arrow = nullptr;
    color_caption = nullptr;
    type_caption = nullptr;
    temp_caption = nullptr;
    temp_lbl = nullptr;
    list_cont = nullptr;
    chips_cont = nullptr;
    hint_lbl = nullptr;
    color_row = nullptr;
    custom_btn = nullptr;
    custom_lbl = nullptr;
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
        // Ist fuer den Slot nichts hinterlegt, aber der Drucker meldet einen
        // Typ, dann das generische Profil dieses Materials vorwaehlen. Sonst
        // bliebe "Konfigurieren" gesperrt, und wer nur die Farbe aendern
        // will, muesste erst ein Material heraussuchen, das laengst feststeht.
        if (chosen_index < 0 && slot_type[0]) chosen_index = generic_index_for(slot_type);

        // Filter auf das Material des Slots vorbelegen. Damit steht nach dem
        // Oeffnen eine kurze, passende Liste da statt aller 87 Profile — das
        // spart nicht nur Wischen, sondern vor allem internen Speicher.
        // Sobald der Benutzer selbst gefiltert hat, bleibt seine Wahl stehen.
        if (!filter_touched && chosen_index >= 0) {
            bambuddy_filament_preset_t p;
            if (bambuddy_filament_get(chosen_index, &p) && p.material[0]) {
                strncpy(filter_material, p.material, sizeof(filter_material) - 1);
                filter_material[sizeof(filter_material) - 1] = '\0';
            }
        }
        rebuild_chips();
        rebuild_list();
        update_title();
        update_preview();
    }

    update_apply_state();

    // Nach einem erfolgreichen Schreiben schliesst sich die Ansicht von
    // selbst — der Slot ist gesetzt, es gibt hier nichts mehr zu tun.
    //
    // Bei einem Fehlschlag bleibt sie stehen und zeigt den Grund. Frueher
    // schloss sie in beiden Faellen: Der Fehler stand dann zwar im Log, auf
    // dem Display sah es aber so aus, als waere alles gutgegangen.
    if (waiting_for_write && !bambuddy_filament_busy()) {
        waiting_for_write = false;

        if (bambuddy_filament_last_write_ok()) {
            close_view();
            return;
        }

        ui_set_text(preview_lbl, bambuddy_filament_message());
        ui_set_text_color(preview_lbl, COL_ERR);
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
    lv_obj_set_width(title_lbl, SCREEN_W - (PAD + 4) - (44 + PAD) - 8);
    lv_label_set_long_mode(title_lbl, LV_LABEL_LONG_DOT);
    lv_obj_align(title_lbl, LV_ALIGN_TOP_LEFT, PAD + 4, 8);

    // Drei Zeilen im Kopf: Filament, Farbe, Temperaturen — jeweils links die
    // Beschriftung, rechts der Wert. Alle Werte beginnen an derselben
    // x-Position, sonst wirkte eine Zeile eingerueckt ohne erkennbaren Grund.
    //
    // Die Filamentzeile endet vor dem Schliessen-Knopf, der bis y40 hinunter
    // reicht. Die beiden Zeilen darunter liegen frei und duerfen breiter sein.
    type_caption = make_caption(overlay, "Typ", PAD + 4, FILAMENT_Y);
    preview_lbl = make_value(overlay, PAD + 52, FILAMENT_Y,
                             SCREEN_W - (PAD + 52) - (44 + PAD) - 8);

    color_caption = make_caption(overlay, "Farbe", PAD + 4, COLOR_LINE_Y);
    preview_dot_old = make_dot(overlay, PAD + 52, COLOR_LINE_Y - 1);
    preview_arrow = make_caption(overlay, LV_SYMBOL_RIGHT, PAD + 72, COLOR_LINE_Y);
    preview_dot_new = make_dot(overlay, PAD + 92, COLOR_LINE_Y - 1);

    // Die Temperaturen gibt niemand ein — sie werden aus dem Profil
    // hergeleitet. Hier stehen sie, bevor sie an den Drucker gehen.
    temp_caption = make_caption(overlay, "Temp", PAD + 4, TEMP_LINE_Y);
    temp_lbl = make_value(overlay, PAD + 52, TEMP_LINE_Y,
                          SCREEN_W - (PAD + 52) - PAD - 8);

    make_button(overlay, 44, HEADER_H - 8, LV_ALIGN_TOP_RIGHT, -PAD, 4, COL_NEUTRAL,
                LV_SYMBOL_CLOSE, close_cb);

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
        lv_obj_t *swatch = make_swatch(color_row, swatch_w, COLOR_H);
        lv_obj_set_style_bg_color(swatch, lv_color_hex(QUICK_COLORS[i]), 0);
        lv_obj_set_user_data(swatch, (void *)(uintptr_t)QUICK_COLORS[i]);
        lv_obj_add_event_cb(swatch, quick_color_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)QUICK_COLORS[i]);
    }

    // Letztes Feld: die eigene Farbe. Es zeigt die aktuelle Wahl und oeffnet
    // das Farbrad.
    custom_btn = make_swatch(color_row, swatch_w, COLOR_H);
    lv_obj_add_event_cb(custom_btn, custom_color_cb, LV_EVENT_CLICKED, nullptr);
    custom_lbl = lv_label_create(custom_btn);
    lv_label_set_text(custom_lbl, LV_SYMBOL_PLUS);
    lv_obj_center(custom_lbl);

    make_button(overlay, 130, FOOT_H, LV_ALIGN_TOP_LEFT, PAD, FOOT_Y, COL_ERR, "Leeren",
                reset_cb);
    make_button(overlay, 110, FOOT_H, LV_ALIGN_TOP_LEFT, PAD + 138, FOOT_Y, COL_NEUTRAL,
                "Abbrechen", close_cb);

    apply_btn = make_button(overlay, CONTENT_W - 138 - 118, FOOT_H, LV_ALIGN_TOP_RIGHT,
                            -PAD, FOOT_Y, COL_OK, "Konfigurieren", apply_cb);
    apply_lbl = lv_obj_get_child(apply_btn, 0);

    ui_timer = lv_timer_create(tick_cb, 300, nullptr);
    lv_timer_set_repeat_count(ui_timer, -1);
}

void filament_config_open(const filament_slot_info_t &slot)
{
    if (overlay) return;

    ui_watch("filament:oeffnen");

    slot_ams_id = slot.ams_id;
    slot_tray_id = slot.tray_id;
    // Bewusst ohne Ausweichwert: 0x000000 ist Schwarz, eine gueltige
    // Filamentfarbe. Wer hier auf "nicht gesetzt" pruefen wollte, machte aus
    // jeder schwarzen Spule eine weisse. Der AMS-Screen entscheidet, was er
    // bei leerem Fach mitgibt.
    chosen_color = slot.color;
    current_color_rgb = slot.color;
    current_temp_min = slot.temp_min;
    current_temp_max = slot.temp_max;
    filter_touched = false;
    selection_touched = false;
    waiting_for_write = false;

    strncpy(slot_title, slot.label ? slot.label : "AMS", sizeof(slot_title) - 1);
    slot_title[sizeof(slot_title) - 1] = '\0';
    strncpy(slot_type, slot.type ? slot.type : "", sizeof(slot_type) - 1);
    slot_type[sizeof(slot_type) - 1] = '\0';
    strncpy(current_name, slot.name ? slot.name : "", sizeof(current_name) - 1);
    current_name[sizeof(current_name) - 1] = '\0';

    // Erst danach der Startfilter — er liest slot_type. Startfilter ist das
    // Material der eingelegten Spule, sonst PLA als haeufigste Wahl.
    strncpy(filter_material, slot_type[0] ? slot_type : "PLA",
            sizeof(filter_material) - 1);
    filter_material[sizeof(filter_material) - 1] = '\0';

    bambuddy_filament_set_visible(true);
    chosen_index = bambuddy_filament_slot_preset_index(slot.ams_id, slot.tray_id);
    if (chosen_index < 0 && slot_type[0]) chosen_index = generic_index_for(slot_type);

    build(slot_title);
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
