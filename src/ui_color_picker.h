#pragma once

#include <stdint.h>

// Farbwähler als Vollbild: Farbrad, Helligkeitsregler, Vorschau.
//
// Er lag ursprünglich im Filament-Screen, weil er dort zuerst gebraucht
// wurde. Seit die Oberfläche eine einstellbare Akzentfarbe hat, brauchen ihn
// zwei Stellen — und zwei Kopien desselben Rades wären zwei Gelegenheiten,
// es unterschiedlich zu machen.
//
// Der Rückruf kommt nur bei "Übernehmen"; beim Abbrechen passiert nichts.
typedef void (*ui_color_picker_cb_t)(uint32_t rgb, void *user_data);

void ui_color_picker_open(const char *title, uint32_t start_rgb,
                          ui_color_picker_cb_t on_pick, void *user_data);

// Ist gerade einer offen? (Doppelte Overlays vermeiden.)
bool ui_color_picker_is_open();

// Offenen Waehler schliessen, ohne den Rueckruf zu feuern. Fuer den Fall,
// dass die Ansicht darunter verschwindet — ein Rad ohne Screen dahinter
// haette niemanden mehr, dem es antworten koennte.
void ui_color_picker_close();
