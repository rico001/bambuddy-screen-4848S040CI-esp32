#pragma once

#include <lvgl.h>

// Eigene Montserrat-Schnitte statt der eingebauten.
//
// LVGLs mitgelieferte Fonts enthalten nur 0x20-0x7F plus Gradzeichen, Bullet
// und die benutzten FontAwesome-Symbole. Alles darueber hat keine Glyphe, und
// LVGL zeichnet dafuer ein leeres Rechteck — sowohl fuer eigene Texte als
// auch fuer Dateinamen, die aus Bambuddy kommen und beliebige Zeichen
// enthalten duerfen.
//
// Diese Schnitte sind mit derselben Optionszeile erzeugt, die im Kopf jeder
// eingebauten Font-Datei steht (gleiche Symbolliste, gleiches bpp), nur um
// 0xA0-0xFF erweitert: Umlaute, ss, Akzente, die skandinavischen und
// romanischen Zeichen. Was darueber hinausgeht — chinesische Modellnamen von
// MakerWorld etwa — bleibt ein Rechteck; eine CJK-Schrift wiegt Megabyte und
// passt nicht in die OTA-Partition.
//
// Erzeugt mit (Groesse jeweils einsetzen; alles auf einer Zeile — ein
// Backslash am Zeilenende waere hier eine Fortsetzung des Kommentars und
// laesst den Compiler warnen):
//
//   npx lv_font_conv --no-compress --no-prefilter --bpp 4 --size 16
//     --font Montserrat-Medium.ttf -r '0x20-0x7F,0xA0-0xFF,0x2022'
//     --font FontAwesome5-Solid+Brands+Regular.woff -r <Symbolliste>
//     --format lvgl -o src/fonts/bb_font_16.c --force-fast-kern-format
//     --lv-include lvgl.h
//
// Beide Schriftdateien und die Symbolliste liegen im LVGL-Paket unter
// scripts/built_in_font/ bzw. im Kopf von src/font/lv_font_montserrat_16.c.
//
// Die Zeilenhoehe faellt ein bis drei Pixel groesser aus als bei den
// eingebauten: Akzente auf Grossbuchstaben brauchen den Platz. Sie hier von
// Hand zurueckzusetzen wuerde genau diese Zeichen oben abschneiden.
//
// Groesse 48 (Uhr im Bildschirmschoner) bleibt die eingebaute: Dort stehen
// nur Ziffern, und der Schnitt waere der mit Abstand groesste.

LV_FONT_DECLARE(bb_font_12);
LV_FONT_DECLARE(bb_font_14);
LV_FONT_DECLARE(bb_font_16);
LV_FONT_DECLARE(bb_font_24);
