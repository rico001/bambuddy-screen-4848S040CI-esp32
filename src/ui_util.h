#pragma once

#include <lvgl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// Nur schreiben, wenn sich wirklich etwas geaendert hat.
//
// Der Framebuffer liegt im PSRAM und wird vom Panel dauerhaft per DMA
// ausgelesen. Jede Neuzeichnung konkurriert mit diesem Datenstrom um die
// Bandbreite und kann als kurzes Zucken sichtbar werden. Ein Label neu zu
// setzen macht LVGL immer schmutzig — auch mit identischem Text. Diese
// Helfer verhindern genau das.

static inline void ui_set_text(lv_obj_t *label, const char *text)
{
    if (!label || !text) return;

    const char *current = lv_label_get_text(label);
    if (current && strcmp(current, text) == 0) return;

    lv_label_set_text(label, text);
}

static inline void ui_set_text_fmt(lv_obj_t *label, const char *fmt, ...)
{
    if (!label) return;

    char buf[96];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    ui_set_text(label, buf);
}

static inline void ui_set_text_color(lv_obj_t *obj, uint32_t rgb)
{
    if (!obj) return;

    const lv_color_t color = lv_color_hex(rgb);
    if (lv_color_eq(lv_obj_get_style_text_color(obj, LV_PART_MAIN), color)) return;

    lv_obj_set_style_text_color(obj, color, 0);
}

static inline void ui_set_bg_color(lv_obj_t *obj, uint32_t rgb)
{
    if (!obj) return;

    const lv_color_t color = lv_color_hex(rgb);
    if (lv_color_eq(lv_obj_get_style_bg_color(obj, LV_PART_MAIN), color)) return;

    lv_obj_set_style_bg_color(obj, color, 0);
}
