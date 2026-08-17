#pragma once

#include <stdint.h>

// Bildschirmschoner: tritt an die Stelle der Abschaltung, wenn er im
// Einstellungs-Screen gewaehlt ist. Der Ablauf davor bleibt unveraendert —
// erst abdunkeln, dann die Untaetigkeitsgrenze —, nur das Ergebnis ist ein
// anderes.
//
// Liegt als Overlay ueber allen Kacheln. Die unsichtbare Weckflaeche der
// Abschaltung bleibt darueber liegen und faengt die Beruehrung ab, damit ein
// Tipp nicht blind auf dem Screen darunter landet.

enum screensaver_mode_t {
    SCREENSAVER_OFF = 0,
    SCREENSAVER_CLOCK = 1,        // grosse Uhr, Datum, Druckerzustand
    SCREENSAVER_MATRIX = 2,       // fallende Zeichen
    SCREENSAVER_MATRIX_CLOCK = 3, // fallende Zeichen, Uhr in einer Tafel davor
};

// Schaltet auf die gewuenschte Art um. SCREENSAVER_OFF blendet aus.
void screensaver_show(screensaver_mode_t mode);

bool screensaver_visible();

// Laesst sich diese Art gerade sinnvoll anzeigen? Die reine Uhr braucht eine
// gestellte Zeit — ohne sie bleibt es bei der Abschaltung, statt eine
// erfundene Uhrzeit gross ins Zimmer zu leuchten. Matrix braucht nichts, und
// "Matrix + Uhr" laesst ohne Zeit einfach die Tafel weg.
bool screensaver_mode_available(screensaver_mode_t mode);
