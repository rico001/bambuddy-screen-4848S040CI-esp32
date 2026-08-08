#pragma once

#include <stdint.h>

// Modellbild des laufenden Druckauftrags.
//
// Bambuddy liefert es als 512x512-PNG mit Transparenz — dekodiert waere das
// 1 MB. Deshalb wird der PNG-Datenstrom pixelweise durchgereicht (pngle) und
// dabei direkt auf 128x128 heruntergerechnet: 32 KB statt 1 MB. Die
// Transparenz wird gegen COVER_BG verrechnet, dieselbe Farbe hat die Kachel
// im Screen — dadurch wirkt das Bild freigestellt.

static constexpr uint16_t COVER_SIZE = 128;

// Fuers Vollbild wird dasselbe PNG noch einmal geholt und in echter Groesse
// dekodiert. Ein hochskaliertes 128er-Bild waere klobig, und Skalieren beim
// Zeichnen kostet jedes Mal aufs Neue Rechenzeit und Speicherbandbreite.
static constexpr uint16_t COVER_BIG_SIZE = 400;

static constexpr uint32_t COVER_BG = 0x1E2228;

// Holt das Bild, sobald sich der Auftrag aendert. Nur aus dem Netzwerk-Task
// aufrufen. job_name == aktueller subtask_name.
void bambuddy_cover_update(const char *job_name);

// Liefert true, wenn ein neues Bild bereitliegt. Nur aus dem LVGL-Thread.
bool bambuddy_cover_take_frame(void **buf);

bool bambuddy_cover_has_frame();

// Bild verwerfen (kein Auftrag mehr).
void bambuddy_cover_reset();

// --- Vollbild ------------------------------------------------------------
// Grosse Fassung anfordern bzw. wieder freigeben (Vollbild auf/zu).
void bambuddy_cover_set_big_wanted(bool wanted);

// Holt die grosse Fassung, falls angefordert und noch nicht vorhanden.
// Wird pro Motiv genau einmal geladen. Nur aus dem Netzwerk-Task.
void bambuddy_cover_update_big(const char *job_name);

// Statt des laufenden Auftrags das Vorschaubild eines Archiv-Eintrags
// anzeigen (Warteschlange). 0 schaltet zurueck auf den laufenden Auftrag.
void bambuddy_cover_request_archive(int32_t archive_id);

// Liefert true, wenn die grosse Fassung neu bereitliegt.
bool bambuddy_cover_take_big_frame(void **buf);

bool bambuddy_cover_has_big_frame();
