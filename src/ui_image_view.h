#pragma once

#include <lvgl.h>
#include <stdint.h>

// Bildschirmfuellende Bildansicht.
//
// Kamera-Livebild, Modellbild des laufenden Drucks, Vorschau aus der
// Warteschlange und aus dem Archiv brauchten alle dasselbe: schwarze Flaeche
// ueber allem, mittig ein Canvas, oben ein Titel, in der Mitte ein Hinweis
// bis das Bild da ist, und ein Tipp irgendwohin schliesst wieder. Das stand
// viermal fast wortgleich im Code.
//
// Angelegt wird erst beim ersten Oeffnen — vier ungenutzte Overlays im
// Speicher zu halten waere Verschwendung.

struct ui_image_view_t;

typedef void (*ui_image_view_close_cb_t)(void);

struct ui_image_view_t {
    lv_obj_t *overlay;
    lv_obj_t *canvas;
    lv_obj_t *title;
    lv_obj_t *hint;
    ui_image_view_close_cb_t on_close;
};

// Oeffnet die Ansicht (legt sie beim ersten Mal an). title darf nullptr sein.
// on_close wird beim Schliessen gerufen — dort gehoert das Abbestellen des
// Bildnachschubs hin.
void ui_image_view_open(ui_image_view_t *view, int canvas_w, int canvas_h,
                        const char *title, const char *hint,
                        ui_image_view_close_cb_t on_close);

void ui_image_view_close(ui_image_view_t *view);

bool ui_image_view_is_open(const ui_image_view_t *view);

// Fertiges Bild anzeigen (RGB565-Puffer) und den Hinweis ausblenden.
void ui_image_view_set_frame(ui_image_view_t *view, void *buf, int w, int h);

// Hinweistext ersetzen, etwa um einen Fehler zu melden.
void ui_image_view_set_hint(ui_image_view_t *view, const char *text, uint32_t color);
