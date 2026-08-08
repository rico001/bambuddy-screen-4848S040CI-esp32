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

// Reine Auskunft: ein Text, ein Knopf zum Schliessen. Nutzt dieselbe Box und
// damit dieselbe Regel — es steht immer nur ein Dialog gleichzeitig offen.
void ui_info(const char *title, const char *text, const char *close_label);

// Auswahl aus mehreren Moeglichkeiten, untereinander als volle Zeilen.
// Die aktuelle Wahl ist hervorgehoben; current ist ihr Index oder -1.
// on_choose bekommt den gewaehlten Index; beim Abbrechen passiert nichts.
typedef void (*ui_choice_cb_t)(int index, void *user_data);

void ui_choice(const char *title, const char *const *options, int count,
               int current, ui_choice_cb_t on_choose, void *user_data);

// Steht gerade ein Dialog offen? (Doppelte Dialoge vermeiden — und die
// Listen bauen sich nicht um, solange einer offen ist.)
bool ui_confirm_is_open();

// Offene Rueckfrage schliessen, ohne on_ok zu rufen — etwa wenn der
// zugehoerige Eintrag inzwischen verschwunden ist.
void ui_confirm_close();
