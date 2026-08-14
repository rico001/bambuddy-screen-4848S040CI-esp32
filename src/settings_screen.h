#pragma once

#include <lvgl.h>
#include <stdint.h>

// Laedt die gespeicherten Einstellungen und wendet Theme, Helligkeit und
// Zeitzone an. Frueh in setup() aufrufen — vor dem Bau der Screens, sonst
// blitzt beim Start kurz das falsche Theme auf.
void settings_apply_saved();

// Baut den Einstellungs-Screen auf dem gegebenen Parent (z.B. Tileview-Tile).
void settings_screen_create(lv_obj_t *parent);
void settings_screen_destroy();

// Bildschirm sofort einschalten und die Abschaltzeit von vorn laufen lassen —
// als haette man ihn beruehrt. Fuer Ereignisse, die man sehen soll, ohne
// davor zu stehen.
void settings_screen_wake();

// --- Werte fuer andere Module --------------------------------------------
// Abfrageintervall fuer den Druckerstatus. Waehrend eines Drucks kurz,
// im Leerlauf laenger (spart Funk und Serverlast).
uint32_t settings_poll_interval_ms();
uint32_t settings_poll_interval_idle_ms();

// TLS-Zertifikat der Bambuddy-Instanz pruefen? (Standard: ja)
bool settings_tls_verify();

// Soll ein Druckstart abgelehnt werden, solange der Drucker beschaeftigt
// ist? Aus heisst: Warteschlange und Archiv fragen wie frueher nur nach der
// Druckplatte und schicken den Start ab.
bool settings_start_guard();

// POSIX-Zeitzonenstring, z.B. "CET-1CEST,M3.5.0,M10.5.0/3"
const char *settings_timezone();

// Hat die Uhr eine gueltige Zeit vom NTP-Server bekommen?
bool settings_time_synced();

// --- Bausteine fuer weitere Einstellungen ---------------------------------

// Sektionsueberschrift in der Einstellungsliste.
lv_obj_t *settings_add_section(const char *title);

// Zeile mit Icon, Titel und optionalem Untertitel. Das Bedienelement wird als
// letztes Kind angehaengt und sitzt dadurch automatisch rechts:
//
//   lv_obj_t *row = settings_add_row(LV_SYMBOL_BELL, "Ton", "Klick beim Tippen");
//   lv_obj_t *sw  = lv_switch_create(row);
//
lv_obj_t *settings_add_row(const char *icon, const char *title, const char *subtitle);

// Hoehere Zeile, in der ein Slider unter dem Titel ueber die volle Breite
// sitzt. Gibt den Slider zurueck; value_lbl_out liefert das Label rechts oben.
lv_obj_t *settings_add_slider_row(const char *icon, const char *title,
                                  int32_t min, int32_t max, int32_t value,
                                  lv_event_cb_t cb, lv_obj_t **value_lbl_out);

// Zeile fuer einen Textwert. Tippen oeffnet einen Vollbild-Editor mit Tastatur;
// der Wert wird beim Speichern ueber set() abgelegt und in der Zeile angezeigt.
// masked: Wert wird gekuerzt dargestellt (fuer Keys und Tokens).
// numeric: Zahlentastatur statt Volltastatur.
typedef const char *(*settings_get_fn)();
typedef void (*settings_set_fn)(const char *value);

lv_obj_t *settings_add_text_row(const char *icon, const char *title,
                                settings_get_fn get, settings_set_fn set,
                                bool masked, bool numeric);
