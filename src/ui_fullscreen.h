#pragma once

#include <lvgl.h>
#include <stdint.h>

// Bildschirmfuellende Unteransicht mit Zurueck-Knopf.
//
// WLAN, Einstellungen, Smart Plugs und die Jog-Steuerung lagen frueher als
// Unteransichten auf der Systemkachel. Das hatte zwei Nachteile: Sie waren
// auf die Kachelhoehe beschraenkt — seit der Navigationsleiste noch einmal
// 40 Pixel weniger —, und sie fuehlten sich an, als gehoerten sie zu den
// Einstellungen, obwohl man Steckdosen und Steuerung vor dem Drucker
// braucht.
//
// Jetzt legen sie sich ueber alles, wie die Filamentkonfiguration. Der
// Rahmen steht einmal hier statt viermal in den Modulen: Ueberschrift,
// Zurueck-Knopf und Aufraeumen sind ueberall dieselben.

typedef void (*ui_fullscreen_build_cb)(lv_obj_t *parent);
typedef void (*ui_fullscreen_close_cb)(void);

// title ist die Ueberschrift ("System/WLAN"), accent die Farbe des
// Zurueck-Knopfes. build baut den Inhalt in die uebergebene Flaeche,
// on_close gibt ihn wieder frei — das sind die vorhandenen _create()- und
// _destroy()-Funktionen der Module.
void ui_fullscreen_open(const char *title, uint32_t accent,
                        ui_fullscreen_build_cb build, ui_fullscreen_close_cb on_close);

void ui_fullscreen_close();

bool ui_fullscreen_is_open();
