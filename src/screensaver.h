#pragma once

#include <stdint.h>

// Bildschirmschoner: grosse Uhr statt schwarzem Bildschirm.
//
// Tritt an die Stelle der Abschaltung, wenn sie im Einstellungs-Screen
// gewaehlt ist. Der Ablauf davor bleibt unveraendert — erst abdunkeln, dann
// die Untaetigkeitsgrenze —, nur das Ergebnis ist ein anderes.
//
// Liegt als Overlay ueber allen Kacheln. Die unsichtbare Weckflaeche der
// Abschaltung bleibt darueber liegen und faengt die Beruehrung ab, damit ein
// Tipp nicht blind auf dem Screen darunter landet.

enum screensaver_mode_t {
    SCREENSAVER_OFF = 0,
    SCREENSAVER_CLOCK = 1,
};

// Zeigt oder versteckt die Uhr. Beim ersten Anzeigen wird sie angelegt.
void screensaver_show(bool visible);

bool screensaver_visible();

// Laesst sich die Uhr ueberhaupt sinnvoll anzeigen? Ohne gestellte Zeit
// nicht — dann bleibt es bei der Abschaltung, statt eine erfundene Uhrzeit
// gross ins Zimmer zu leuchten.
bool screensaver_clock_available();
