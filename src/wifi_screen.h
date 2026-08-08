#pragma once

#include <lvgl.h>

// Startet Auto-Connect und Reconnect ohne die speicherintensive Ansicht.
void wifi_service_start();

// Erstellt bzw. entfernt nur die WLAN-Ansicht. Der Hintergrunddienst und eine
// bestehende Verbindung laufen nach destroy weiter.
void wifi_screen_create(lv_obj_t *parent);
void wifi_screen_destroy();

// Fuer andere Module (MQTT, Statusleiste, ...): ist das Geraet online?
bool wifi_screen_is_connected();

// SSID des aktuell verbundenen Netzes ("" wenn offline).
const char *wifi_screen_ssid();
