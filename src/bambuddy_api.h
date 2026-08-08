#pragma once

#include <stdint.h>

// Zugriff auf die Bambuddy-API. Die Netzwerkarbeit laeuft in einem eigenen
// FreeRTOS-Task auf Core 0 — die LVGL-Schleife darf davon nichts merken.
// Die UI holt sich fertige Daten ueber bambuddy_api_take().

// Verbindungslage in Stufen: WLAN -> Server -> Drucker.
// Genau diese drei Ebenen koennen unabhaengig voneinander ausfallen.
enum bambuddy_link_t {
    BB_LINK_STARTING,    // Task laeuft noch nicht / hat noch nichts gemeldet
    BB_LINK_NO_WIFI,     // Display hat kein WLAN
    BB_LINK_NO_CONFIG,   // URL oder API-Key fehlen
    BB_LINK_NO_SERVER,   // Bambuddy nicht erreichbar
    BB_LINK_UNAUTHORIZED,// API-Key abgelehnt
    BB_LINK_OK,          // Bambuddy antwortet
};

// Zustand des Druckers, so wie ihn der Screen braucht.
struct bambuddy_status_t {
    bool printer_connected; // Bambuddy hat Verbindung zum Drucker
    char name[32];
    char state[16];         // Rohwert der API, z.B. "RUNNING"
    char job[64];           // subtask_name
    float progress;         // 0..100
    int32_t remaining_min;
    int32_t layer;
    int32_t total_layers;
    float nozzle;
    float nozzle_target;
    float bed;
    float bed_target;
    bool awaiting_plate_clear; // Drucker wartet auf "Platte ist frei"
    uint32_t updated_ms;    // millis() der letzten erfolgreichen Abfrage
};

// Steuerbefehle an den Drucker. Werden an den Netzwerk-Task uebergeben und
// dort abgearbeitet — die Oberflaeche wartet nie auf eine HTTP-Antwort.
enum bambuddy_cmd_t {
    BB_CMD_PAUSE,
    BB_CMD_RESUME,
    BB_CMD_STOP,
};

// Startet den Hintergrund-Task. Nach settings_apply_saved() und
// bambuddy_config_load() aufrufen.
void bambuddy_api_start();

// Befehl einreihen. Liefert false, wenn der Task nicht laeuft.
bool bambuddy_api_send_command(bambuddy_cmd_t cmd);

// Rueckmeldung zum zuletzt gesendeten Befehl und deren Alter in ms.
// Die UI blendet sie nach ein paar Sekunden wieder aus.
const char *bambuddy_api_command_message();
uint32_t bambuddy_api_command_message_age();

// Von der MQTT-Seite genutzt: fertigen Status einspeisen bzw. Lage melden.
void bambuddy_api_publish_status(const bambuddy_status_t *status);
void bambuddy_api_report_link(bambuddy_link_t link, const char *message);

// Kopiert den zuletzt geholten Status. Liefert true, wenn seit dem letzten
// Aufruf neue Daten angekommen sind.
bool bambuddy_api_take(bambuddy_status_t *out);

bambuddy_link_t bambuddy_api_link();

// Klartext zum letzten Fehler ("" wenn alles laeuft).
const char *bambuddy_api_error();

// Druckt der Drucker gerade? (bestimmt den Poll-Takt)
bool bambuddy_api_is_printing();

// Laeuft ein Auftrag — vorbereiten, drucken oder pausiert? Dann gibt es ein
// Modellbild zu zeigen, auch wenn noch keine Schicht gedruckt ist.
bool bambuddy_api_has_active_job();

// Wartet der Drucker darauf, dass die Druckplatte als frei gemeldet wird?
bool bambuddy_api_awaiting_plate_clear();

// millis() des letzten Task-Durchlaufs. Bleibt der Wert stehen, haengt oder
// starb der Hintergrund-Task — dann darf die UI nicht das WLAN beschuldigen.
uint32_t bambuddy_api_heartbeat();
