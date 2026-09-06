#pragma once

#include <lvgl.h>

// Objekte des laufenden Drucks zum Ueberspringen auswaehlen. Wird als
// Vollbild geoeffnet (siehe ui_fullscreen.h) und baut sich in die
// uebergebene Flaeche.
void skip_objects_view_create(lv_obj_t *parent);
void skip_objects_view_destroy();

// Oeffnet die Ansicht als Vollbild. Aus dem Status-Screen gerufen.
void skip_objects_view_open();
