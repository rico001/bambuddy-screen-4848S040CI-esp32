#pragma once

#include <lvgl.h>

// Statusanzeige des Druckers: Zustand, Auftrag, Fortschritt, Restzeit,
// Temperaturen und — waehrend eines Drucks — ein kleines Kamerabild.
// Holt sich die Daten selbst aus bambuddy_api; kein Aufruf aus loop() noetig.
void status_screen_create(lv_obj_t *parent);
