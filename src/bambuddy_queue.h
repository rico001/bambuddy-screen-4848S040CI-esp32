#pragma once

#include <stddef.h>
#include <stdint.h>

// Druckwarteschlange: anstehende Auftraege lesen und einen davon starten.
//
// Geholt wird nur, solange der Screen sichtbar ist — im Alltag steht das
// Display auf der Statusseite, und eine Warteschlange, die niemand ansieht,
// muss auch nicht abgefragt werden.

#define BB_QUEUE_MAX_ITEMS 20

struct bambuddy_queue_item_t {
    int32_t id;
    int32_t archive_id;  // fuer das Vorschaubild
    char name[64];
    int32_t print_seconds;
    float grams;
    char filament[16];
    uint32_t color;      // Filamentfarbe als 0xRRGGBB
    bool manual_start;   // wartet auf einen Startbefehl
};

// Sichtbarkeit des Screens melden. Beim Einschalten wird sofort geholt.
void bambuddy_queue_set_visible(bool visible);
bool bambuddy_queue_visible();

// Holt die Liste bei Bedarf. Nur aus dem Netzwerk-Task aufrufen.
void bambuddy_queue_update();

// --- Zugriff fuer die UI (LVGL-Thread) -----------------------------------
// Kopiert die aktuelle Liste. Liefert die Anzahl der Eintraege.
int bambuddy_queue_copy(bambuddy_queue_item_t *out, int max_items);

// True, wenn sich die Liste seit dem letzten Aufruf geaendert hat.
bool bambuddy_queue_take_fresh();

// Wie viele Auftraege der Server gemeldet hat — kann groesser sein als die
// Zahl der gespeicherten Eintraege. Dann fehlt der Rest in der Anzeige und
// darf nicht einfach verschwiegen werden.
int bambuddy_queue_total();

// Auftrag starten. clear_plate: dem Drucker vorher bestaetigen, dass die
// Druckplatte frei ist.
void bambuddy_queue_request_start(int32_t item_id, bool clear_plate);

// Auftrag aus der Warteschlange entfernen.
void bambuddy_queue_request_delete(int32_t item_id);

// --- AMS ------------------------------------------------------------------
// Welcher Slot koennte den Auftrag drucken? Bambuddy legt die Spule erst
// beim Start fest (ams_mapping ist vorher leer), deshalb ist das eine
// Vorhersage: Typ muss passen, unter mehreren gewinnt die naechste Farbe.
//
// out bekommt "A1".."D4", "" wenn die AMS-Belegung unbekannt ist.
// Rueckgabe: true, wenn ein passender Slot existiert.
bool bambuddy_queue_match_slot(const char *filament_type, uint32_t color,
                               char *out, size_t out_len);

// Rueckmeldung zur letzten Aktion und deren Alter in ms.
const char *bambuddy_queue_message();
uint32_t bambuddy_queue_message_age();
