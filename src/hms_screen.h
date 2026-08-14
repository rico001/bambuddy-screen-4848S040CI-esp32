#pragma once

#include <lvgl.h>

// Liste der Druckermeldungen seit dem Start. Wird als Vollbild geoeffnet
// (siehe ui_fullscreen.h) und baut sich in die uebergebene Flaeche.
void hms_screen_create(lv_obj_t *parent);
void hms_screen_destroy();
