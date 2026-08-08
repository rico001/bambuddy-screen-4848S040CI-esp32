#pragma once

#include <lvgl.h>

// Erstellt den WLAN-Screen auf dem gegebenen Parent (z.B. ein Tileview-Tile).
// Uebernimmt komplett: Scan, Passwort-Eingabe, Verbindung, Auto-Reconnect.
// Laeuft ausschliesslich ueber LVGL-Timer — kein Polling aus loop() noetig.
void wifi_screen_create(lv_obj_t *parent);

// Fuer andere Module (MQTT, Statusleiste, ...): ist das Geraet online?
bool wifi_screen_is_connected();

// SSID des aktuell verbundenen Netzes ("" wenn offline).
const char *wifi_screen_ssid();
