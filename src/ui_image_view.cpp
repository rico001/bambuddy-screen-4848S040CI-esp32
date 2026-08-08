#include "ui_image_view.h"

#include "ui_layout.h"
#include "ui_theme.h"
#include "ui_watch.h"

static void close_cb(lv_event_t *e)
{
    ui_image_view_t *view = (ui_image_view_t *)lv_event_get_user_data(e);
    if (view) ui_image_view_close(view);
}

static void build(ui_image_view_t *view, int canvas_w, int canvas_h)
{
    // lv_layer_top: liegt ueber Statusleiste und allen Kacheln
    view->overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(view->overlay, SCREEN_W, SCREEN_H);
    lv_obj_align(view->overlay, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_remove_flag(view->overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(view->overlay, 0, 0);
    lv_obj_set_style_border_width(view->overlay, 0, 0);
    lv_obj_set_style_pad_all(view->overlay, 0, 0);
    lv_obj_set_style_bg_color(view->overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(view->overlay, LV_OPA_COVER, 0);
    lv_obj_add_flag(view->overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(view->overlay, close_cb, LV_EVENT_CLICKED, view);

    // Bewusst ohne lv_image_set_scale: Skalieren wuerde LVGL bei jeder
    // Neuzeichnung erneut rechnen lassen, und das kostet auf diesem Board
    // sichtbar Bandbreite. Die Bilder kommen in Anzeigegroesse aus dem
    // Decoder.
    view->canvas = lv_canvas_create(view->overlay);
    lv_obj_set_size(view->canvas, canvas_w, canvas_h);
    lv_obj_center(view->canvas);
    lv_obj_add_flag(view->canvas, LV_OBJ_FLAG_HIDDEN);

    view->hint = lv_label_create(view->overlay);
    lv_obj_set_style_text_color(view->hint, lv_color_hex(COL_MUTED), 0);
    lv_obj_set_style_text_align(view->hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(view->hint);

    // Titel oben: sonst sieht man ein Modell und weiss nicht, welches
    view->title = lv_label_create(view->overlay);
    lv_obj_set_width(view->title, SCREEN_W - 40);
    lv_label_set_long_mode(view->title, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(view->title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(view->title, lv_color_white(), 0);
    lv_obj_align(view->title, LV_ALIGN_TOP_MID, 0, 18);
}

void ui_image_view_open(ui_image_view_t *view, int canvas_w, int canvas_h,
                        const char *title, const char *hint,
                        ui_image_view_close_cb_t on_close)
{
    if (!view) return;
    ui_watch("bild:oeffnen");
    if (!view->overlay) build(view, canvas_w, canvas_h);

    view->on_close = on_close;

    lv_label_set_text(view->title, title ? title : "");
    ui_image_view_set_hint(view, hint ? hint : "", COL_MUTED);
    lv_obj_add_flag(view->canvas, LV_OBJ_FLAG_HIDDEN);

    lv_obj_remove_flag(view->overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(view->overlay);
}

void ui_image_view_close(ui_image_view_t *view)
{
    if (!view || !view->overlay) return;

    lv_obj_add_flag(view->overlay, LV_OBJ_FLAG_HIDDEN);
    if (view->on_close) view->on_close();
}

bool ui_image_view_is_open(const ui_image_view_t *view)
{
    return view && view->overlay &&
           !lv_obj_has_flag(view->overlay, LV_OBJ_FLAG_HIDDEN);
}

void ui_image_view_set_frame(ui_image_view_t *view, void *buf, int w, int h)
{
    if (!view || !view->canvas || !buf) return;

    ui_watch("bild:puffer");
    lv_canvas_set_buffer(view->canvas, buf, w, h, LV_COLOR_FORMAT_RGB565);
    lv_obj_remove_flag(view->canvas, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(view->hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(view->canvas);
}

void ui_image_view_set_hint(ui_image_view_t *view, const char *text, uint32_t color)
{
    if (!view || !view->hint) return;

    lv_label_set_text(view->hint, text ? text : "");
    lv_obj_set_style_text_color(view->hint, lv_color_hex(color), 0);
    lv_obj_remove_flag(view->hint, LV_OBJ_FLAG_HIDDEN);
}
