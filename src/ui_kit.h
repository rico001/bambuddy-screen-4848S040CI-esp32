#pragma once

#include <lvgl.h>
#include <stdint.h>

// Bausteine der Oberflaeche.
//
// Vorher baute jeder Screen seine Karten und Knoepfe selbst: sechsmal
// derselbe Block aus Radius, Rand, Innenabstand und Hintergrundfarbe, jedes
// Mal mit leicht anderen Zahlen. Das sieht man auf dem Geraet als Unruhe —
// Kanten, die nicht fluchten, Rundungen, die nicht zusammenpassen.
//
// Hier stehen sie einmal. Wer eine Karte braucht, nimmt ui_card(); wer eine
// Zustandsanzeige braucht, nimmt ui_pill(). Aendert sich die Anmutung,
// aendert sie sich ueberall zugleich.

// Grundflaeche eines Screens: Hintergrundfarbe, kein Rand, kein Innenabstand.
// Fuer Tileview-Kacheln und Vollbild-Ansichten.
void ui_screen_surface(lv_obj_t *obj);

// Kartenlook auf ein bestehendes Objekt legen. Fuer Kinder von Listen mit
// eigenem Layout (Flex): Dort darf die Position nicht gesetzt werden, sonst
// streiten Layout und Ausrichtung.
void ui_card_style(lv_obj_t *obj);

// Karte: erhoehte Flaeche mit Radius und feinem Rand. Der Rand ist der
// Unterschied zwischen "Flaeche mit anderer Farbe" und "Karte" — ohne ihn
// verschwimmen die Kanten bei gedaempfter Helligkeit.
lv_obj_t *ui_card(lv_obj_t *parent, int x, int y, int w, int h);

// Flaeche auf einer Karte (Fortschrittsbalken-Grund, Bildplatzhalter,
// Wertekaesten). Eine Stufe heller als die Karte, ohne Rand.
lv_obj_t *ui_tile(lv_obj_t *parent, int w, int h);

// Knopf mit voller Farbflaeche. color ist die Fuellung; ist sie COL_NEUTRAL,
// wirkt der Knopf zurueckhaltend, sonst als Handlungsaufforderung.
lv_obj_t *ui_button(lv_obj_t *parent, uint32_t color, int w, int h);

// Runder Knopf mit einem Symbol darin. Fuer Kopfzeilen und Ecken, wo kein
// Platz fuer Beschriftung ist.
lv_obj_t *ui_icon_button(lv_obj_t *parent, const char *symbol, uint32_t color,
                         int size);

// Kapsel mit farbigem Punkt und Text — die Zustandsanzeige der Oberflaeche.
// Gibt die Kapsel zurueck; text_out liefert das Label zum spaeteren Aendern,
// dot_out den Punkt.
lv_obj_t *ui_pill(lv_obj_t *parent, uint32_t color, const char *text,
                  lv_obj_t **text_out, lv_obj_t **dot_out);
void ui_pill_set(lv_obj_t *text_lbl, lv_obj_t *dot, uint32_t color, const char *text);

// Kleine Ueberschrift in Grossbuchstaben mit weiter Laufweite — die Zeile,
// die ueber einem Wert steht ("DUESE", "RESTZEIT").
lv_obj_t *ui_overline(lv_obj_t *parent, const char *text);

// Grosser Wert (Zahl mit Einheit) in der Hauptfarbe.
lv_obj_t *ui_value(lv_obj_t *parent, const char *text);

// Duenne Flaeche als Linie — Trenner, Schlauch, Raster.
//
// lv_line braucht ein Punktfeld, das leben muss, solange die Linie lebt. Bei
// Screens, die sich zur Laufzeit neu bauen, ist das eine Fehlerquelle fuer
// nichts: Ein Rechteck von zwei Pixeln Hoehe zeichnet dasselbe Bild.
lv_obj_t *ui_rule(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color);

// Fortschrittsbalken im Stil der Oberflaeche: flach, ohne Rand, mit
// abgerundeten Enden.
lv_obj_t *ui_progress(lv_obj_t *parent, int w, int h);
