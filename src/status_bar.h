#pragma once

#include <lvgl.h>

// Schmale Leiste ganz oben, auf allen Screens sichtbar: WLAN, Datenquelle
// und Uhrzeit. Zeigt den WLAN-Zustand direkt aus dem Treiber — unabhaengig
// davon, was der Netzwerk-Task gerade meldet.
void status_bar_create(lv_obj_t *parent);
