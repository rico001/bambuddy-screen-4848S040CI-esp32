#pragma once

#include <stdint.h>

// Welche Bambuddy-Fassung laeuft auf der Instanz — und passt sie noch zu der,
// gegen die dieses Display entwickelt wurde?
//
// Der Abgleich ist nicht kosmetisch: Ein umbenanntes Antwortfeld erzeugt
// keinen Fehler, sondern stillschweigend eine Null. Auf dem Display steht
// dann dauerhaft "0 %", und niemand weiss warum. Laeuft drueben eine andere
// Version als die gepruefte, soll das sichtbar sein, bevor man der Anzeige
// glaubt.
//
// Verglichen wird gegen `current_version` aus /updates/check — nur diese
// Fassung laeuft wirklich. `latest_version` sagt lediglich, ob Rico seine
// Instanz aktualisieren koennte.
#define BB_TESTED_VERSION "1.2.5.3"

// Aus dem Netzwerk-Task aufrufen.
void bambuddy_version_update();

// Naechsten Durchlauf sofort abfragen lassen (z.B. beim Oeffnen des
// System-Screens). Ein eigener Mindestabstand verhindert Dauerabfragen.
void bambuddy_version_request_refresh();

// Liegt eine gelesene Version vor?
bool bambuddy_version_known();

const char *bambuddy_version_current(); // laufende Fassung, "" wenn unbekannt
const char *bambuddy_version_latest();  // auf GitHub verfuegbar, "" wenn unbekannt
bool bambuddy_version_update_available();

// Laeuft genau die Fassung, gegen die das Display geprueft wurde?
bool bambuddy_version_matches_tested();

// Klartext zum letzten Fehlversuch ("" wenn alles lief).
const char *bambuddy_version_error();

// Hat sich seit dem letzten Aufruf etwas geaendert? (Die UI zeichnet dann neu.)
bool bambuddy_version_take_fresh();
