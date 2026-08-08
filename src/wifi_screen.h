#pragma once

#include <lvgl.h>

// Startet Auto-Connect und Reconnect ohne die speicherintensive Ansicht.
void wifi_service_start();

// Erstellt bzw. entfernt nur die WLAN-Ansicht. Der Hintergrunddienst und eine
// bestehende Verbindung laufen nach destroy weiter.
void wifi_screen_create(lv_obj_t *parent);
void wifi_screen_destroy();


