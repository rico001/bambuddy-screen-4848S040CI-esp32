#pragma once

#include <stdint.h>

// Konfiguration eines einzelnen AMS-Slots.
//
// Liegt als Overlay ueber allen Kacheln, wie die Bildansicht: Der AMS-Screen
// bleibt darunter stehen und muss nicht umgebaut werden.
//
// slot_label ist die Beschriftung fuer den Kopf ("AMS-A - Slot 1"),
// current_color die Farbe der eingelegten Spule als 0xRRGGBB.
void filament_config_open(int32_t ams_id, int32_t tray_id, const char *slot_label,
                          uint32_t current_color);

// Steht die Konfiguration offen? Der AMS-Screen darf seine Slots dann nicht
// neu aufbauen — er wuerde den gerade angetippten Knopf loeschen.
bool filament_config_is_open();
