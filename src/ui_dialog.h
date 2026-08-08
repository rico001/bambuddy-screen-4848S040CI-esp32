#pragma once

#include <lvgl.h>
#include <stdint.h>

// Rueckfrage vor folgenreichen Aktionen.
//
// Vorher trug jeder Screen seinen eigenen Dialog mit denselben vier
// Bausteinen: eine Box-Variable, ein Schliessen, ein Nein, ein Ja. Sechsmal
// dasselbe, mit sechs Gelegenheiten, es unterschiedlich zu machen.
//
// Der Dialog liegt ueber allen Screens, laesst immer nur einen gleichzeitig
// zu und raeumt sich beim Antippen einer der beiden Antworten selbst auf.
// on_ok wird ausschliesslich bei Zustimmung gerufen.

typedef void (*ui_confirm_cb_t)(void *user_data);

void ui_confirm(const char *title, const char *text,
                const char *cancel_label,
                const char *ok_label, uint32_t ok_color,
                ui_confirm_cb_t on_ok, void *user_data);

// Steht gerade eine Rueckfrage offen? (Doppelte Dialoge vermeiden.)
bool ui_confirm_is_open();

// Offene Rueckfrage schliessen, ohne on_ok zu rufen — etwa wenn der
// zugehoerige Eintrag inzwischen verschwunden ist.
void ui_confirm_close();
