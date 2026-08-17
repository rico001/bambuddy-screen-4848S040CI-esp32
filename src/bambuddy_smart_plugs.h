#pragma once

#include <stdint.h>

#define BB_SMART_PLUG_MAX_ITEMS 6

struct bambuddy_smart_plug_t {
    int32_t id;
    char name[64];
    bool state_known;
    bool is_on;
    bool reachable;
};

void bambuddy_smart_plugs_set_visible(bool visible);
bool bambuddy_smart_plugs_visible();
void bambuddy_smart_plugs_update();

int bambuddy_smart_plugs_copy(bambuddy_smart_plug_t *out, int max_items);
bool bambuddy_smart_plugs_has_data();
bool bambuddy_smart_plugs_take_fresh();
void bambuddy_smart_plugs_request_control(int32_t plug_id, bool turn_on);

const char *bambuddy_smart_plugs_message();
uint32_t bambuddy_smart_plugs_message_age();

// --- Nach dem Druck ausschalten ------------------------------------------
//
// Bambuddy kann das selbst (`auto_off` an der Steckdose), aber nicht ueber
// einen API-Schluessel: `PATCH /smart-plugs/{id}` antwortet mit
// `403 "API keys cannot be used for administrative operations"`. Auch
// `auto_off_after` am Warteschlangeneintrag hilft nur halb — es laesst sich
// nur setzen, solange der Eintrag `pending` ist ("Can only update pending
// items"), und ein Druck, den jemand am Drucker selbst startet, hat gar
// keinen Eintrag.
//
// Deshalb macht es das Display: Es sieht den Zustandswechsel ohnehin und darf
// die Steckdose schalten (`POST /smart-plugs/{id}/control` ist erlaubt).
//
// Der Ablauf: Erst wenn ein Druck wirklich gelaufen ist (RUNNING gesehen) und
// danach FINISH meldet, beginnt die Nachlaufzeit von zehn Minuten. Erst
// danach — und erst unterhalb der Duesentemperatur-Schwelle — faellt die
// Steckdose. Ohne die
// Bedingung "RUNNING gesehen" wuerde ein Drucker, der seit gestern auf FINISH
// steht, im Moment des Einschaltens abgeschaltet.
bool bambuddy_auto_off_enabled();
void bambuddy_auto_off_set(bool on);

// Laeuft die Nachlaufzeit gerade? (Fuer die Anzeige: der Knopf zeigt dann,
// dass gleich etwas passiert.)
bool bambuddy_auto_off_pending();
