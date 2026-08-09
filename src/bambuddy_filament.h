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

// Profilliste im Hintergrund holen, ohne dass die Konfiguration offen ist.
// Der AMS-Screen braucht sie, um Kurz-IDs in Namen zu uebersetzen.
void bambuddy_filament_preload();

// Klartextname zu einer Filament-Kurz-ID ("GFL99" -> "Generic PLA").
// Leerer String, solange die Liste nicht geladen oder die ID unbekannt ist.
const char *bambuddy_filament_name_for_idx(const char *info_idx);

// Was steckt laut Drucker in diesem Fach? info_idx ist die Kurz-ID aus dem
// Status.
//
// Massgeblich ist immer der Drucker. Die in Bambuddy hinterlegte Zuordnung
// ist nur eine Notiz und kann veraltet sein — sie wird deshalb bloss dann
// bevorzugt, wenn sie zur gemeldeten Kurz-ID passt. Dann traegt sie mehr:
// Ein eigenes Preset schickt die generische ID seines Materials an den
// Drucker, "Generic PLA - 1" waere sonst nicht von "Generic PLA" zu
// unterscheiden.
const char *bambuddy_filament_tray_name(int32_t ams_id, int32_t tray_id,
                                        const char *info_idx);
bool bambuddy_filament_visible();
void bambuddy_filament_update();

int bambuddy_filament_count();
bool bambuddy_filament_get(int index, bambuddy_filament_preset_t *out);
bool bambuddy_filament_ready();
bool bambuddy_filament_take_fresh();

// Welches Profil ist fuer diesen Slot hinterlegt? Index in die Liste,
// sonst -1. Speist die Vorauswahl beim Oeffnen.
int bambuddy_filament_slot_preset_index(int32_t ams_id, int32_t tray_id);

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

// Hat der letzte Schreibvorgang geklappt? Die Ansicht schliesst sich nur
// dann von selbst — bei einem Fehler bleibt sie offen und zeigt ihn an.
bool bambuddy_filament_last_write_ok();

// Steht noch Arbeit an — ein Schreibvorgang oder die Nachfass-Abrufe danach?
// Der Netzwerk-Task darf solange nicht in sein langes Leerlaufintervall
// fallen, sonst kaeme die Auffrischung erst eine halbe Minute spaeter.
bool bambuddy_filament_pending_work();

// Wartet dieses Fach noch auf frische Daten vom Drucker? Zwischen dem
// Konfigurieren und dem letzten Nachfass-Abruf zeigt der AMS-Screen dort
// einen Ladekreis — sonst sieht man sekundenlang den alten Stand und haelt
// ihn fuer das Ergebnis.
bool bambuddy_filament_slot_pending(int32_t ams_id, int32_t tray_id);

// Kennung des wartenden Fachs, 0 wenn keines wartet. Aendert sie sich, muss
// der AMS-Screen neu zeichnen — an den Druckerdaten allein waere das nicht
// zu erkennen.
uint32_t bambuddy_filament_pending_token();

const char *bambuddy_filament_message();
uint32_t bambuddy_filament_message_age();
