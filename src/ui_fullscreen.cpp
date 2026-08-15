#include "ui_fullscreen.h"

#include "ui_layout.h"
#include "ui_watch.h"
#include "ui_font.h"

static constexpr int PAD = 12;

static lv_obj_t *overlay = nullptr;
static ui_fullscreen_close_cb on_close_cb = nullptr;

static void back_cb(lv_event_t *)
{
    ui_fullscreen_close();
}

void ui_fullscreen_open(const char *title, uint32_t accent,
                        ui_fullscreen_build_cb build, ui_fullscreen_close_cb on_close)
{
    // Nur eine gleichzeitig. Zwei uebereinander waeren zwei Ansichten, die
    // beide ihre Daten abfragen — und nur eine davon sieht man.
    if (overlay) return;

    ui_watch("vollbild:oeffnen");

    on_close_cb = on_close;

    overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(overlay, SCREEN_W, SCREEN_H);
    lv_obj_align(overlay, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(overlay, 0, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_pad_all(overlay, 0, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(0x101418), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);

    lv_obj_t *back = lv_button_create(overlay);
    lv_obj_set_size(back, 40, 40);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, PAD, 6);
    lv_obj_set_style_radius(back, 20, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(accent), 0);
    lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *back_lbl = lv_label_create(back);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
    lv_obj_center(back_lbl);

    lv_obj_t *title_lbl = lv_label_create(overlay);
    lv_label_set_text(title_lbl, title);
    lv_obj_set_style_text_font(title_lbl, &bb_font_16, 0);
    lv_obj_set_width(title_lbl, SCREEN_W - (PAD + 52) - PAD);
    lv_label_set_long_mode(title_lbl, LV_LABEL_LONG_DOT);
    lv_obj_align(title_lbl, LV_ALIGN_TOP_LEFT, PAD + 52, 14);

    if (build) build(overlay);
}

void ui_fullscreen_close()
{
    if (!overlay) return;

    // Erst aufraeumen, dann loeschen. Die Module halten Zeitgeber, die auf
    // ihre Labels zeigen — liefe einer noch, waehrend die Objekte schon weg
    // sind, greift er ins Leere.
    if (on_close_cb) on_close_cb();
    on_close_cb = nullptr;

    // Asynchron: Der Aufruf kommt aus dem Klick-Callback des Zurueck-Knopfes,
    // und der ist ein Kind dieser Flaeche.
    lv_obj_delete_async(overlay);
    overlay = nullptr;
}

bool ui_fullscreen_is_open()
{
    return overlay != nullptr;
}
