#include "nav_bar.h"

#include "printer_icon.h"
#include "ui_layout.h"
#include "ui_nav.h"
#include "ui_theme.h"
#include "ui_font.h"

static constexpr int BTN_W = SCREEN_W / NAV_TILE_COUNT;

// Womit wird der Knopf bebildert?
//
// Drei Arten, weil LVGLs Zeichensatz nur einen Teil davon hergibt: Fuer
// Warteschlange, Archiv und System gibt es passende Symbole, fuer den
// Drucker liegt ein eigenes Bild im Projekt, und die Filamentrolle muss aus
// zwei Kreisen gebaut werden.
enum icon_kind_t {
    ICON_SYMBOL, // Zeichen aus dem LVGL-Zeichensatz
    ICON_IMAGE,  // printer_icon (A8, einfaerbbar)
    ICON_SPOOL,  // schmaler Ring mit kleinem Kreis darin
};

struct nav_entry_t {
    icon_kind_t kind;
    const char *symbol; // nur bei ICON_SYMBOL
    const char *label;
};

// Reihenfolge wie die Kacheln: AMS links vom Status, rechts davon
// Warteschlange, Archiv und System.
//
// "Auftraege" statt "Warteschlange": Der ausgeschriebene Name passt bei 96
// Pixel Knopfbreite nicht und wuerde abgeschnitten. Eine Abkuerzung mit
// Punkt sieht nach Platzmangel aus — ein kuerzeres, richtiges Wort nicht.
static const nav_entry_t ENTRIES[NAV_TILE_COUNT] = {
    {ICON_SPOOL, nullptr, "AMS"},
    {ICON_IMAGE, nullptr, "Status"},
    {ICON_SYMBOL, LV_SYMBOL_LIST, "Aufträge"},
    {ICON_SYMBOL, LV_SYMBOL_DIRECTORY, "Archiv"},
    {ICON_SYMBOL, LV_SYMBOL_SETTINGS, "System"},
};

// Je Knopf merken, was eingefaerbt werden muss. Ueber die Kinder zu laufen
// und blind die Textfarbe zu setzen wuerde bei Ring und Bild nichts
// bewirken — die haben keine.
struct nav_button_t {
    lv_obj_t *btn;
    lv_obj_t *text;
    lv_obj_t *symbol; // ICON_SYMBOL
    lv_obj_t *image;  // ICON_IMAGE
    lv_obj_t *ring;   // ICON_SPOOL
    lv_obj_t *hub;    // ICON_SPOOL
};

static nav_button_t buttons[NAV_TILE_COUNT];
static int active_index = -1;

static void button_cb(lv_event_t *e)
{
    ui_nav_tile((int)(intptr_t)lv_event_get_user_data(e));
}

// Filamentrolle: schmaler Ring, darin ein kleiner Kreis. Beides sind
// randlose Kreise — der Ring traegt seine Farbe im Rahmen, der Kern im
// Hintergrund.
static void build_spool(nav_button_t &nb, lv_obj_t *parent)
{
    nb.ring = lv_obj_create(parent);
    lv_obj_set_size(nb.ring, 18, 18);
    lv_obj_align(nb.ring, LV_ALIGN_TOP_MID, 0, 3);
    lv_obj_remove_flag(nb.ring, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(nb.ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(nb.ring, 9, 0);
    lv_obj_set_style_pad_all(nb.ring, 0, 0);
    lv_obj_set_style_bg_opa(nb.ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(nb.ring, 2, 0);

    nb.hub = lv_obj_create(nb.ring);
    lv_obj_set_size(nb.hub, 6, 6);
    lv_obj_center(nb.hub);
    lv_obj_remove_flag(nb.hub, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(nb.hub, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(nb.hub, 3, 0);
    lv_obj_set_style_pad_all(nb.hub, 0, 0);
    lv_obj_set_style_border_width(nb.hub, 0, 0);
}

void nav_bar_create(lv_obj_t *parent)
{
    // Zurueckstellen, falls der Kachelwechsel schon vor dem Aufbau gefeuert
    // hat: nav_bar_set_active() verwirft sonst spaeter den ersten echten
    // Aufruf als "steht schon so", und die Leiste bliebe unbemalt.
    active_index = -1;

    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_size(bar, SCREEN_W, NAV_BAR_H);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x14181C), 0);

    for (int i = 0; i < NAV_TILE_COUNT; i++) {
        nav_button_t &nb = buttons[i];
        nb = {};

        nb.btn = lv_obj_create(bar);
        lv_obj_set_size(nb.btn, BTN_W, NAV_BAR_H);
        lv_obj_align(nb.btn, LV_ALIGN_TOP_LEFT, i * BTN_W, 0);
        lv_obj_remove_flag(nb.btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(nb.btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_radius(nb.btn, 0, 0);
        lv_obj_set_style_border_width(nb.btn, 0, 0);
        lv_obj_set_style_pad_all(nb.btn, 0, 0);
        lv_obj_set_style_bg_color(nb.btn, lv_color_hex(COL_ACCENT), 0);
        lv_obj_set_style_bg_opa(nb.btn, LV_OPA_TRANSP, 0);
        lv_obj_add_event_cb(nb.btn, button_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        // Symbol oben, Beschriftung darunter. Das Symbol allein waere bei
        // Archiv und System nicht eindeutig, die Beschriftung allein bei
        // einem Blick aus zwei Metern zu klein.
        switch (ENTRIES[i].kind) {
        case ICON_SPOOL:
            build_spool(nb, nb.btn);
            break;
        case ICON_IMAGE:
            nb.image = lv_image_create(nb.btn);
            lv_image_set_src(nb.image, &printer_icon);
            // Das Bild ist 27x28 und damit zu gross fuer eine 40 Pixel hohe
            // Leiste mit Beschriftung. Auf etwa 17 Pixel verkleinern; 256
            // entspricht der Originalgroesse.
            lv_image_set_scale(nb.image, 160);
            lv_obj_set_style_image_recolor_opa(nb.image, LV_OPA_COVER, 0);
            lv_obj_align(nb.image, LV_ALIGN_TOP_MID, 0, -2);
            break;
        case ICON_SYMBOL:
        default:
            nb.symbol = lv_label_create(nb.btn);
            lv_label_set_text(nb.symbol, ENTRIES[i].symbol);
            lv_obj_set_style_text_font(nb.symbol, &bb_font_16, 0);
            lv_obj_align(nb.symbol, LV_ALIGN_TOP_MID, 0, 3);
            break;
        }

        nb.text = lv_label_create(nb.btn);
        lv_label_set_text(nb.text, ENTRIES[i].label);
        lv_obj_set_style_text_font(nb.text, &bb_font_12, 0);
        lv_obj_align(nb.text, LV_ALIGN_BOTTOM_MID, 0, -3);
    }
}

void nav_bar_set_active(int index)
{
    if (index == active_index) return;
    active_index = index;

    for (int i = 0; i < NAV_TILE_COUNT; i++) {
        nav_button_t &nb = buttons[i];
        if (!nb.btn) continue;

        const bool on = i == index;
        const lv_color_t color = lv_color_hex(on ? COL_ACCENT : COL_MUTED);

        // Nur Farben wechseln, keine Groessen: Eine Groessenaenderung wuerde
        // die Nachbarknoepfe mit neu zeichnen lassen.
        lv_obj_set_style_bg_opa(nb.btn, on ? LV_OPA_20 : LV_OPA_TRANSP, 0);

        if (nb.text) lv_obj_set_style_text_color(nb.text, color, 0);
        if (nb.symbol) lv_obj_set_style_text_color(nb.symbol, color, 0);
        if (nb.image) lv_obj_set_style_image_recolor(nb.image, color, 0);
        if (nb.ring) lv_obj_set_style_border_color(nb.ring, color, 0);
        if (nb.hub) lv_obj_set_style_bg_color(nb.hub, color, 0);
    }
}
