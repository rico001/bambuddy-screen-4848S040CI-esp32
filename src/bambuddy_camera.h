#pragma once

#include <stdint.h>

// Livebild der Druckerkamera, nur auf Abruf.
//
// Der Snapshot der API ist 1280x720 — dekodiert waeren das 1,8 MB. TJpgDec
// halbiert beim Dekodieren auf 640x360, beim Ablegen wird auf 480x270
// ausgeduennt. Diese Groesse wird 1:1 angezeigt: Wuerde LVGL das Bild beim
// Zeichnen skalieren, liefe die Rechnerei bei jeder Neuzeichnung erneut und
// zerrt dem Panel die PSRAM-Bandbreite weg.

static constexpr uint16_t CAM_W = 480;
static constexpr uint16_t CAM_H = 270;

// Vollbild offen oder zu. Nur wenn offen, wird ueberhaupt etwas geholt —
// ein Snapshot kostet 15 KB, das laeuft nicht nebenbei mit.
void bambuddy_camera_set_active(bool active);
bool bambuddy_camera_active();

// Holt bei Bedarf ein neues Bild. Nur aus dem Netzwerk-Task aufrufen.
void bambuddy_camera_update();

// Liefert true, wenn ein neues Bild bereitliegt. Nur aus dem LVGL-Thread.
bool bambuddy_camera_take_frame(void **buf);

bool bambuddy_camera_has_frame();

// Fehlertext zum letzten Versuch ("" wenn alles lief).
const char *bambuddy_camera_error();
