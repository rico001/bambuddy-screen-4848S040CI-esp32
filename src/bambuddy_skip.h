#pragma once

#include <stdint.h>

// Objekte des laufenden Drucks überspringen.
//
// Der Drucker kennt die einzelnen Körper der Platte und kann sie einzeln
// auslassen — misslingt einer, muss deshalb nicht die ganze Platte in den
// Müll. Bambuddy bietet das im Frontend als Dialog an; hier ist dasselbe
// fürs Display.
//
// Geholt wird nur, solange die Ansicht offen ist: Die Liste gilt fuer genau
// einen Druck und hat ohne offenen Dialog keinen Leser.
//
// Uebersprungen wird endgueltig. Es gibt keinen Weg zurueck — der Drucker
// laesst den Koerper aus, bis der Druck vorbei ist. Deshalb fragt die
// Oberflaeche vorher nach.

#define BB_SKIP_MAX_OBJECTS 32

struct bambuddy_skip_object_t {
    int32_t id;      // identify_id, so heisst es der Drucker
    char name[48];
    bool skipped;    // schon uebersprungen
    // Mittelpunkt auf der Druckplatte in Millimetern. Die Grundflaeche des
    // Koerpers liefert die API nicht mit — die Draufsicht kann deshalb nur
    // zeigen, wo etwas steht, nicht wie gross es ist. Das Bambuddy-Frontend
    // malt aus demselben Grund gleich grosse Rechtecke.
    float x;
    float y;
    bool pos_known;
};

// Sichtbarkeit der Ansicht melden. Beim Aufschlagen wird sofort geholt.
void bambuddy_skip_set_visible(bool visible);
bool bambuddy_skip_visible();

// Holt die Liste bei Bedarf und schickt angeforderte Befehle ab.
// Nur aus dem Netzwerk-Task aufrufen.
void bambuddy_skip_update();

// --- Zugriff fuer die UI (LVGL-Thread) -----------------------------------
// Kopiert die aktuelle Liste, liefert die Anzahl der Eintraege.
int bambuddy_skip_copy(bambuddy_skip_object_t *out, int max_items);

// True, wenn sich seit dem letzten Aufruf etwas geaendert hat.
bool bambuddy_skip_take_fresh();

// Wie viele Objekte der Server gemeldet hat — kann groesser sein als die
// Zahl der gespeicherten Eintraege.
int bambuddy_skip_total();

// Umschliessendes Rechteck ueber alle Objekte (x0, y0, x1, y1 in mm), wie es
// der Server mitliefert. Die Draufsicht zeigt genau diesen Ausschnitt statt
// der ganzen Platte: Ein Druck belegt oft nur eine Handflaeche in der Mitte,
// und auf 200 Pixel Plattenbreite waeren das drei nicht mehr trennbare
// Punkte. Liefert false, wenn der Server keines gemeldet hat.
bool bambuddy_skip_bbox(float *out_x0, float *out_y0, float *out_x1, float *out_y1);

// Wurde ueberhaupt schon einmal erfolgreich geholt? Solange nicht, zeigt die
// Ansicht "Lade ..." statt "Keine Objekte" — das ist ein Unterschied.
bool bambuddy_skip_loaded();

// Meldet der Server, dass gerade gedruckt wird? Ohne laufenden Druck gibt es
// nichts zu ueberspringen.
bool bambuddy_skip_is_printing();

// Objekte zum Ueberspringen anmelden. Wird im Netzwerk-Task abgeschickt.
void bambuddy_skip_request(const int32_t *ids, int count);

// Liegt noch ein Befehl an? Der Netzwerk-Task wacht dafuer oefter auf.
bool bambuddy_skip_pending_work();

// Rueckmeldung zur letzten Aktion und deren Alter in ms.
const char *bambuddy_skip_message();
uint32_t bambuddy_skip_message_age();
