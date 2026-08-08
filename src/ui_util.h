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

// Sekunden als Dauer ausgeben ("13 min", "2 h 05 min"). Leer bei 0 —
// "0 min" waere eine Angabe, die es gar nicht gibt.
static inline void ui_format_duration(int32_t seconds, char *out, size_t out_len)
{
    if (seconds <= 0) {
        if (out_len) out[0] = '\0';
        return;
    }

    const int minutes = (seconds + 30) / 60; // auf Minuten runden
    if (minutes < 60) {
        snprintf(out, out_len, "%d min", minutes);
    } else {
        snprintf(out, out_len, "%d h %02d min", minutes / 60, minutes % 60);
    }
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
