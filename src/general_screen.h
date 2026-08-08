#pragma once

#include <lvgl.h>

void general_screen_create(lv_obj_t *parent);
void general_screen_set_visible(bool visible);

// Direkt in die Smart-Plug-Ansicht springen, ohne den Umweg ueber die
// Kachelübersicht. Der Aufrufer muss vorher auf diese Kachel wechseln.
void general_screen_show_smart_plugs();

// Dasselbe fuer die Jog-Steuerung.
void general_screen_show_jog();
