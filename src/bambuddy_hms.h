#pragma once

#include <stdint.h>
#include <time.h>

// Meldungen des Druckers, mitgeschrieben seit dem Start.
//
// Zwei Quellen, weil eine nicht reicht:
//
//   1. "hms_errors" aus dem Status — die Gesundheitsueberwachung der
//      Hardware: verstopfte Duese, Filament leer, ueberlasteter Motor.
//      Diese Liste steht nur da, solange der Fehler anliegt; wer nicht
//      zufaellig hinsieht, erfaehrt nie, dass nachts das Filament ausging.
//
//   2. Die Zustandswechsel nach FAILED und FINISH. Ein abgebrochener Druck
//      ist fuer den Drucker kein Defekt und taucht in hms_errors gar nicht
//      auf — geprueft an einem selbst gestoppten Druck: state stand auf
//      FAILED, die Liste war leer. Ohne diese zweite Quelle fehlte im Log
//      ausgerechnet das Ereignis, nach dem man am ehesten sucht.
//
// Damit ist es kein reines Fehlerbuch, sondern ein Protokoll: Was ist
// schiefgegangen, und was ist fertig geworden.
//
// Das Protokoll ueberlebt den Neustart: Es liegt im NVS, denselben Speicher
// wie die Einstellungen. Geschrieben wird nur, wenn ein Eintrag dazukommt —
// ein paar Mal am Tag, nicht im Poll-Takt. Fuer den Flash-Speicher ist das
// unerheblich.
//
// Geleert wird ausschliesslich von Hand (bambuddy_hms_clear). Die API haette
// mit POST /printers/{id}/hms/clear einen eigenen Weg, der aber die Fehler
// am Drucker quittiert statt dieses Protokoll zu leeren — zwei verschiedene
// Dinge, die man nicht verwechseln sollte.

#define BB_HMS_LOG_MAX 15

// Eigene Stufe fuer Erfolgsmeldungen. Bewusst negativ: Die Schweregrade des
// Druckers beginnen bei 1, und 0 ist dort der Wert fuer "nicht angegeben" —
// eine gelungene Sache waere darunter nicht von einem unbekannten Fehler zu
// unterscheiden.
#define BB_HMS_SEVERITY_OK (-1)
#define BB_HMS_ACTIVE_MAX 8

struct bambuddy_hms_entry_t {
    char code[24];     // full_code, z.B. "0300_4006_0002_0001" — leer bei Zustaenden
    char text[64];     // Klartext, leer wenn der Code unbekannt ist
    int32_t severity;  // 1 = schwer, 2 = ernst, 3 = normal, 4 = Hinweis
    time_t when;       // 0, wenn die Uhr beim Auftreten noch nicht stand
    uint32_t uptime_s; // Ersatzangabe fuer genau diesen Fall
    bool restored;     // aus dem NVS geladen, also aus einem frueheren Lauf
};

// Meldet den gerade anliegenden Satz. Eingetragen wird nur, was vorher nicht
// dabei war — sonst schriebe ein Fehler, der eine Stunde ansteht, das Log
// bei zweisekuendigem Abruf binnen einer Minute mit sich selbst voll.
void bambuddy_hms_report(const char codes[][24], const int32_t *severities, int count);

// Zustand des Druckers melden. Eingetragen werden die Wechsel nach FAILED
// und FINISH, je einmal — der Zustand bleibt danach stehen, bis der naechste
// Druck laeuft. job darf leer sein.
void bambuddy_hms_report_state(const char *state, const char *job);

// Start des Displays vermerken. reason ist der Neustartgrund im Klartext;
// unexpected hebt Absturz, Watchdog und Spannungseinbruch hervor, damit man
// im Protokoll sieht, ob das Geraet nachts von selbst neu gestartet ist.
void bambuddy_hms_report_boot(const char *reason, bool unexpected);

int bambuddy_hms_count();

// index 0 ist der juengste Eintrag.
bool bambuddy_hms_get(int index, bambuddy_hms_entry_t *out);

// "schwer", "ernst", "normal", "Hinweis" — sonst die blosse Zahl.
const char *bambuddy_hms_severity_text(int32_t severity);

// Hat sich seit dem letzten Aufruf etwas geaendert? Die Ansicht baut sich
// nur dann neu auf.
bool bambuddy_hms_take_fresh();

// Ausstehende Aenderung ins NVS schreiben, falls faellig.
//
// Muss regelmaessig aus dem Netzwerk-Task gerufen werden. Warum nicht
// sofort beim Eintragen: Ein Flash-Schreibvorgang legt auf dem ESP32 kurz
// den Befehls-Cache still, und da der Code aus dem Flash laeuft, steht in
// dem Moment auch die Oberflaeche. Genau beim Zustandswechsel — wenn
// ohnehin die halbe Kachel neu gezeichnet wird — faellt das als Ruckeln auf.
//
// Ein paar Sekunden Versatz kosten im schlimmsten Fall den letzten Eintrag
// bei einem Stromausfall. Das ist der bessere Handel.
void bambuddy_hms_flush();

// Protokoll leeren, auch im NVS. Folgenreich und nicht rueckgaengig zu
// machen — die Ansicht fragt vorher nach.
void bambuddy_hms_clear();
