#pragma once

#include <stdint.h>

// Konfiguration eines einzelnen AMS-Slots.
//
// Liegt als Overlay ueber allen Kacheln, wie die Bildansicht: Der AMS-Screen
// bleibt darunter stehen und muss nicht umgebaut werden.
//
// Der Ist-Zustand des Fachs. Er dient zweierlei: als Vorbelegung — wer nur
// die Farbe wechseln will, soll das Material nicht neu heraussuchen muessen
// — und als linke Seite der Gegenueberstellung "alt -> neu" im Kopfbereich.
//
// Als Struktur statt als acht einzelne Parameter: Bei so vielen Werten
// gleichen Typs vertauscht man beim Aufruf sonst unbemerkt zwei davon.
struct filament_slot_info_t {
    int32_t ams_id;
    int32_t tray_id;
    const char *label; // "AMS-A - Fach 1"
    const char *type;  // gemeldetes Material, "" bei leerem Fach
    const char *name;  // Profilname, sonst das Material
    uint32_t color;    // 0xRRGGBB
    int16_t temp_min;
    int16_t temp_max;
};

void filament_config_open(const filament_slot_info_t &slot);

// Steht die Konfiguration offen? Der AMS-Screen darf seine Slots dann nicht
// neu aufbauen — er wuerde den gerade angetippten Knopf loeschen.
bool filament_config_is_open();
