#pragma once

#include <stddef.h>
#include <stdint.h>

// Abbild des Bildschirms, so wie er gerade wirklich aussieht.
//
// Der naheliegende Weg waere LVGLs lv_snapshot_take() — der zeichnet aber nur
// den uebergebenen Objektbaum. Vollbilder (Einstellungen, WLAN, Jog, Dialoge,
// Bildschirmschoner) liegen auf lv_layer_top und fehlten dann ausgerechnet
// dort, wo man sie fotografieren will. Ebenso scheidet das Auslesen des
// Panel-Bildspeichers aus: esp_lcd_rgb_panel_get_frame_buffer() gibt es erst
// ab IDF 5, hier laeuft 4.4.
//
// Deshalb wird beim Zeichnen mitgeschrieben: Auf Anforderung erklaert das
// Modul den ganzen Bildschirm fuer ungueltig und faengt die Kacheln ab, die
// LVGL danach an das Panel schickt. Was dabei zusammenkommt, ist genau das
// zusammengesetzte Bild — inklusive aller Ebenen.
//
// Der Puffer (450 KB, RGB565) liegt im PSRAM und existiert nur zwischen
// Anforderung und Freigabe.

// Aufnahme anfordern. Muss aus dem LVGL-Thread kommen (loop()).
// Liefert false, wenn schon eine laeuft oder der Speicher nicht reicht.
bool screenshot_request();

// Ist das Bild vollstaendig? Erst danach duerfen die Daten gelesen werden.
bool screenshot_ready();

// Rohdaten: 480x480 Pixel, RGB565 in der Bytefolge des Panels, zeilenweise
// von oben nach unten.
const uint8_t *screenshot_data();
size_t screenshot_size();
uint16_t screenshot_width();
uint16_t screenshot_height();

// Aufnahme abschliessen. Der Puffer bleibt dabei belegt: Ihn freizugeben und
// spaeter neu anzufordern schlaegt fehl, sobald das PSRAM zerstueckelt ist —
// und das ist es nach ein paar Modell- und Kamerabildern. Nach der ersten
// Aufnahme sind die 450 KB also dauerhaft weg.
void screenshot_release();
