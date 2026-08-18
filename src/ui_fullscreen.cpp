#include "ui_fullscreen.h"

#include "ui_kit.h"
#include "ui_layout.h"
#include "ui_theme.h"
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
    ui_screen_surface(overlay);

    lv_obj_t *back = ui_icon_button(overlay, LV_SYMBOL_LEFT, accent, 40);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, PAD, 6);
    lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *title_lbl = lv_label_create(overlay);
    lv_label_set_text(title_lbl, title);
    lv_obj_set_style_text_font(title_lbl, &bb_font_16, 0);
    lv_obj_set_style_text_color(title_lbl, lv_color_hex(COL_TEXT), 0);
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
