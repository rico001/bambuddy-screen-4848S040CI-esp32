#include "ui_color_picker.h"

#include <Arduino.h>
#include <lvgl.h>
#include <math.h>

#include "ui_font.h"
#include "ui_kit.h"
#include "ui_layout.h"
#include "ui_theme.h"
#include "ui_util.h"
#include "ui_watch.h"

static constexpr int PAD = 12;
static constexpr int CONTENT_W = SCREEN_W - 2 * PAD;
static constexpr int WHEEL_SIZE = 200;
static constexpr int WHEEL_RADIUS = 96;

static lv_obj_t *wheel_overlay = nullptr;
static lv_obj_t *wheel_canvas = nullptr;
static lv_obj_t *wheel_preview = nullptr;
static lv_obj_t *wheel_slider = nullptr;

// Der Zeichenpuffer bleibt liegen, auch wenn das Rad zu ist.
//
// 200x200 in RGB565 sind 80 KB. Sie jedes Mal neu anzufordern hiesse, sie
// irgendwann nicht mehr am Stueck zu bekommen — im PSRAM kommen und gehen
// staendig Modell- und Kamerabilder.
static uint16_t *wheel_buf = nullptr;

static int wheel_hue = 0;
static int wheel_sat = 100;
static int wheel_val = 100;

static ui_color_picker_cb_t pick_cb = nullptr;
static void *pick_user = nullptr;

static void wheel_close();

static uint32_t color_from_hsv(int h, int s, int v)
{
    const lv_color_t c = lv_color_hsv_to_rgb((uint16_t)h, (uint8_t)s, (uint8_t)v);
    return ((uint32_t)c.red << 16) | ((uint32_t)c.green << 8) | c.blue;
}

static lv_obj_t *make_button(lv_obj_t *parent, int w, int h, lv_align_t align, int x,
                             int y, uint32_t color, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *btn = ui_button(parent, color, w, h);
    lv_obj_align(btn, align, x, y);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_hex(COL_TEXT), 0);
    lv_obj_center(lbl);
    return btn;
}

static void wheel_draw()
{
    // Das Rad wird einmal in voller Helligkeit gezeichnet. Der Regler
    // darunter aendert nur die gewaehlte Farbe, nicht das Bild — ein
    // Neuzeichnen bei jeder Reglerbewegung waere auf diesem Board deutlich
    // sichtbar, weil es dem Panel Speicherbandbreite wegnimmt.
    const int center = WHEEL_SIZE / 2;
    const lv_color_t bg = lv_color_hex(COL_BG);

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
    pick_cb = nullptr;
    pick_user = nullptr;

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
    // Erst merken, dann schliessen: wheel_close() raeumt die Zeiger auf, und
    // der Rueckruf darf seinerseits eine neue Ansicht oeffnen.
    const uint32_t picked = color_from_hsv(wheel_hue, wheel_sat, wheel_val);
    ui_color_picker_cb_t cb = pick_cb;
    void *user = pick_user;

    wheel_close();
    if (cb) cb(picked, user);
}

void ui_color_picker_open(const char *title, uint32_t start_rgb,
                          ui_color_picker_cb_t on_pick, void *user_data)
{
    if (wheel_overlay) return;

    pick_cb = on_pick;
    pick_user = user_data;

    // Startfarbe in Winkel, Saettigung und Helligkeit zerlegen, damit das Rad
    // dort aufgeht, wo die aktuelle Farbe sitzt.
    const lv_color_hsv_t hsv = lv_color_rgb_to_hsv((uint8_t)(start_rgb >> 16),
                                                   (uint8_t)(start_rgb >> 8),
                                                   (uint8_t)start_rgb);
    wheel_hue = hsv.h;
    wheel_sat = hsv.s;
    wheel_val = hsv.v < 10 ? 10 : hsv.v;

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
    lv_obj_set_style_bg_color(wheel_overlay, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(wheel_overlay, LV_OPA_COVER, 0);

    lv_obj_t *title_lbl = lv_label_create(wheel_overlay);
    lv_label_set_text(title_lbl, title);
    lv_obj_set_style_text_font(title_lbl, &bb_font_16, 0);
    lv_obj_set_style_text_color(title_lbl, lv_color_hex(COL_TEXT), 0);
    lv_obj_align(title_lbl, LV_ALIGN_TOP_MID, 0, 14);

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

    // Vorschau und Knoepfe stehen in einer Reihe am unteren Rand.
    //
    // Verankert wird an der Unterkante, nicht an einer Zahl von oben: Der
    // Abstand von 332 stammte aus einer Zeit mit anderen Hoehen darueber und
    // liess die Reihe seither in der Luft haengen, mit hundert Pixeln Leere
    // darunter. Von unten gemessen bleibt sie da, wo sie hingehoert, auch
    // wenn sich am Farbrad oder am Regler noch etwas aendert.
    constexpr int ROW_H = 46;

    wheel_preview = lv_obj_create(wheel_overlay);
    lv_obj_set_size(wheel_preview, 96, ROW_H);
    lv_obj_align(wheel_preview, LV_ALIGN_BOTTOM_LEFT, PAD, -PAD);
    lv_obj_remove_flag(wheel_preview, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(wheel_preview, RADIUS_CTRL, 0);
    lv_obj_set_style_border_width(wheel_preview, 2, 0);
    lv_obj_set_style_border_color(wheel_preview, lv_color_hex(COL_LINE), 0);
    wheel_update_preview();

    make_button(wheel_overlay, 150, ROW_H, LV_ALIGN_BOTTOM_LEFT, PAD + 104, -PAD,
                COL_NEUTRAL, "Abbrechen", wheel_cancel_cb);
    make_button(wheel_overlay, CONTENT_W - 104 - 158, ROW_H, LV_ALIGN_BOTTOM_RIGHT,
                -PAD, -PAD, COL_OK, "Übernehmen", wheel_ok_cb);
}


void ui_color_picker_close()
{
    wheel_close();
}

bool ui_color_picker_is_open()
{
    return wheel_overlay != nullptr;
}
