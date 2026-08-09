#pragma once

#include <stdint.h>

// Filamentprofile fuer die AMS-Slots.
//
// Zwei Quellen, wie in Bambuddy selbst:
//   "Integriert"  GET /cloud/builtin-filaments  -> {filament_id, name}
//   "Lokal"       GET /local-presets/           -> filament[]
//
// Die Liste wird einmal geholt und behalten: 86 integrierte Eintraege
// aendern sich nicht im Betrieb, und jeder erneute Abruf kostet Bandbreite,
// die dem Bildaufbau fehlt.

#define BB_FILAMENT_MAX_PRESETS 128

struct bambuddy_filament_preset_t {
    char id[24];       // "builtin_GFA00" / "local_3" — so merkt Bambuddy es sich
    char name[48];
    char material[12]; // aus dem Namen hergeleitet, z.B. "PLA"
    int16_t temp_min;  // 0 = nicht mitgeliefert, dann gilt die Materialtabelle
    int16_t temp_max;
    bool local;
};

void bambuddy_filament_set_visible(bool visible);
bool bambuddy_filament_visible();
void bambuddy_filament_update();

int bambuddy_filament_count();
bool bambuddy_filament_get(int index, bambuddy_filament_preset_t *out);
bool bambuddy_filament_ready();
bool bambuddy_filament_take_fresh();

// Welches Profil ist fuer diesen Slot hinterlegt? Index in die Liste,
// sonst -1. Speist die Vorauswahl beim Oeffnen.
int bambuddy_filament_slot_preset_index(int32_t ams_id, int32_t tray_id);

// Klartextname des hinterlegten Profils, sonst "". Steht auch dann zur
// Verfuegung, wenn die Profilliste den Eintrag nicht kennt — etwa nach dem
// Loeschen eines lokalen Presets in Bambuddy.
const char *bambuddy_filament_slot_preset_name(int32_t ams_id, int32_t tray_id);

// Welche Duesentemperaturen gingen fuer dieses Profil an den Drucker?
// Eigene Werte des lokalen Presets, sonst die Materialtabelle. Die
// Oberflaeche zeigt sie vor dem Absenden an — sonst waere die einzige
// Angabe, die niemand eintippt, auch die einzige, die niemand sieht.
void bambuddy_filament_effective_temps(const bambuddy_filament_preset_t &preset,
                                       int16_t *lo, int16_t *hi);

// Slot belegen. color_rgb ist 0xRRGGBB; die Deckkraft haengt der Aufruf an.
void bambuddy_filament_request_configure(int32_t ams_id, int32_t tray_id,
                                         int preset_index, uint32_t color_rgb);

// Slot leeren (POST /printers/{id}/ams/{ams}/tray/{tray}/reset).
void bambuddy_filament_request_reset(int32_t ams_id, int32_t tray_id);

// Laeuft gerade ein Schreibvorgang? Solange bleiben die Knoepfe gesperrt.
bool bambuddy_filament_busy();

const char *bambuddy_filament_message();
uint32_t bambuddy_filament_message_age();
