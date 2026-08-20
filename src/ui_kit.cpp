#include "ui_kit.h"

#include <string.h>

#include "ui_font.h"
#include "ui_theme.h"

// Kein gemeinsamer lv_style_t, sondern gesetzte Eigenschaften je Objekt.
//
// Geteilte Stile waeren sparsamer, muessten aber leben, solange irgendein
// Objekt sie benutzt — und die Screens hier werden zur Laufzeit abgeraeumt
// und neu gebaut. Ein Stil, der einen geloeschten Screen ueberlebt, ist eine
// Fehlerquelle, die man erst Wochen spaeter als Absturz bemerkt. Die paar
// Aufrufe mehr beim Aufbau kosten nichts Messbares; gezeichnet wird ohnehin
// nicht oefter.

void ui_screen_surface(lv_obj_t *obj)
{
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(obj, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
}

void ui_card_style(lv_obj_t *obj)
{
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(obj, RADIUS_CARD, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(COL_SURFACE), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(COL_LINE), 0);
    lv_obj_set_style_border_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
}

lv_obj_t *ui_card(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, w, h);
    lv_obj_align(card, LV_ALIGN_TOP_LEFT, x, y);
    ui_card_style(card);
    return card;
}

lv_obj_t *ui_tile(lv_obj_t *parent, int w, int h)
{
    lv_obj_t *tile = lv_obj_create(parent);
    lv_obj_set_size(tile, w, h);
    lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(tile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(tile, RADIUS_CTRL, 0);
    lv_obj_set_style_bg_color(tile, lv_color_hex(COL_RAISED), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tile, 0, 0);
    lv_obj_set_style_pad_all(tile, 0, 0);
    lv_obj_set_style_shadow_width(tile, 0, 0);
    return tile;
}

lv_obj_t *ui_button(lv_obj_t *parent, uint32_t color, int w, int h)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_radius(btn, RADIUS_CTRL, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);

    // Gedrueckt: dieselbe Farbe, nur dunkler. Ein eigener Ton je Knopf waere
    // ein zweiter Wert, den man bei jeder Farbaenderung mitpflegen muesste.
    lv_obj_set_style_bg_color(btn, lv_color_darken(lv_color_hex(color), LV_OPA_30),
                              LV_STATE_PRESSED);

    // Gesperrt: keine Farbe mehr, nur noch Flaeche. Ein abgeblendeter bunter
    // Knopf sieht aus wie ein Knopf mit schlechtem Kontrast; ein grauer sieht
    // aus wie gesperrt.
    lv_obj_set_style_bg_color(btn, lv_color_hex(COL_RAISED), LV_STATE_DISABLED);
    lv_obj_set_style_text_color(btn, lv_color_hex(COL_MUTED), LV_STATE_DISABLED);
    lv_obj_set_style_opa(btn, LV_OPA_COVER, LV_STATE_DISABLED);
    return btn;
}

lv_obj_t *ui_icon_button(lv_obj_t *parent, const char *symbol, uint32_t color,
                         int size)
{
    lv_obj_t *btn = ui_button(parent, color, size, size);
    lv_obj_set_style_radius(btn, size / 2, 0);

    // Innenabstand weg. LVGLs Theme gibt jedem Knopf einen mit, und
    // lv_obj_center() zentriert im Inhaltsbereich — also innerhalb dieses
    // Abstands. Bei einem runden Knopf mit einem einzigen Zeichen sieht man
    // die Verschiebung sofort, weil der Kreis die Mitte vorgibt.
    lv_obj_set_style_pad_all(btn, 0, 0);

    lv_obj_t *icon = lv_label_create(btn);
    lv_label_set_text(icon, symbol);
    lv_obj_set_style_text_color(icon, lv_color_hex(COL_TEXT), 0);
    lv_obj_center(icon);
    return btn;
}

lv_obj_t *ui_pill(lv_obj_t *parent, uint32_t color, const char *text,
                  lv_obj_t **text_out, lv_obj_t **dot_out)
{
    static constexpr int PILL_H = 26;

    lv_obj_t *pill = lv_obj_create(parent);
    lv_obj_set_height(pill, PILL_H);
    lv_obj_set_width(pill, LV_SIZE_CONTENT);
    lv_obj_remove_flag(pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(pill, PILL_H / 2, 0);
    lv_obj_set_style_bg_color(pill, lv_color_hex(COL_RAISED), 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(pill, 0, 0);
    lv_obj_set_style_pad_hor(pill, GAP_M, 0);
    lv_obj_set_style_pad_ver(pill, 0, 0);
    lv_obj_set_style_shadow_width(pill, 0, 0);
    lv_obj_set_flex_flow(pill, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pill, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    // Der farbige Punkt traegt die Bedeutung, der Text benennt sie. Frueher
    // war der ganze Hintergrund farbig — bei fuenf Zustaenden nebeneinander
    // ist das viel Farbe fuer wenig Aussage, und farbenblinde Augen haben
    // ohne Text ohnehin nichts davon.
    lv_obj_t *dot = lv_obj_create(pill);
    lv_obj_set_size(dot, 8, 8);
    lv_obj_set_style_radius(dot, 4, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_set_style_pad_all(dot, 0, 0);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_margin_right(dot, GAP_S, 0);

    lv_obj_t *lbl = lv_label_create(pill);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &bb_font_12, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(COL_TEXT), 0);

    if (text_out) *text_out = lbl;
    if (dot_out) *dot_out = dot;
    return pill;
}

void ui_pill_set(lv_obj_t *text_lbl, lv_obj_t *dot, uint32_t color, const char *text)
{
    if (dot) {
        const lv_color_t c = lv_color_hex(color);
        if (lv_color_to_u32(lv_obj_get_style_bg_color(dot, 0)) != lv_color_to_u32(c)) {
            lv_obj_set_style_bg_color(dot, c, 0);
        }
    }
    if (!text_lbl || !text) return;

    const char *current = lv_label_get_text(text_lbl);
    if (current && strcmp(current, text) == 0) return;
    lv_label_set_text(text_lbl, text);
}

lv_obj_t *ui_overline(lv_obj_t *parent, const char *text)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &bb_font_12, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(COL_MUTED), 0);
    // Weite Laufweite: Grossbuchstaben stehen sonst zu dicht und lesen sich
    // als Block statt als Wort.
    lv_obj_set_style_text_letter_space(lbl, 1, 0);
    return lbl;
}

lv_obj_t *ui_value(lv_obj_t *parent, const char *text)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &bb_font_24, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(COL_TEXT), 0);
    return lbl;
}

lv_obj_t *ui_rule(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color)
{
    lv_obj_t *line = lv_obj_create(parent);
    lv_obj_remove_flag(line, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(line, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(line, w, h);
    lv_obj_align(line, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_set_style_radius(line, 1, 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_pad_all(line, 0, 0);
    lv_obj_set_style_bg_color(line, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    return line;
}

lv_obj_t *ui_progress(lv_obj_t *parent, int w, int h)
{
    lv_obj_t *bar = lv_bar_create(parent);
    lv_obj_set_size(bar, w, h);
    lv_obj_set_style_radius(bar, h / 2, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(COL_RAISED), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, h / 2, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, lv_color_hex(COL_ACCENT), LV_PART_INDICATOR);
    return bar;
}
