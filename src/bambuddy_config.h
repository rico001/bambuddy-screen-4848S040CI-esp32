#pragma once

// Zugangsdaten zur Bambuddy-Instanz. Liegen im NVS und sind ueber den
// Einstellungs-Screen aenderbar. Beim ersten Boot werden die Werte aus
// include/secrets.h uebernommen.

// Frueh in setup() aufrufen, vor dem Bau der Screens.
void bambuddy_config_load();

const char *bambuddy_base_url();   // ohne abschliessenden Slash, z.B. "https://host"
const char *bambuddy_api_key();
const char *bambuddy_cam_token();
int bambuddy_printer_id();

void bambuddy_set_base_url(const char *value);
void bambuddy_set_api_key(const char *value);
void bambuddy_set_cam_token(const char *value);
void bambuddy_set_printer_id(const char *value); // Text, damit die UI direkt speichern kann

// --- MQTT ----------------------------------------------------------------
const char *bambuddy_mqtt_host();
const char *bambuddy_mqtt_user();
const char *bambuddy_mqtt_pass();
const char *bambuddy_mqtt_topic();
int bambuddy_mqtt_port();

void bambuddy_set_mqtt_host(const char *value);
void bambuddy_set_mqtt_user(const char *value);
void bambuddy_set_mqtt_pass(const char *value);
void bambuddy_set_mqtt_topic(const char *value);
void bambuddy_set_mqtt_port(const char *value);

// Woher kommen die Statusdaten? true = MQTT-Abo, false = HTTP-Polling.
bool bambuddy_source_mqtt();
void bambuddy_set_source_mqtt(bool use_mqtt);

// Reicht die Konfiguration fuer die gewaehlte Quelle?
bool bambuddy_config_complete();
bool bambuddy_mqtt_config_complete();

// Baut eine vollstaendige API-URL: bambuddy_url("/printers/%d/status", id)
// Ergebnis zeigt auf einen internen Puffer und gilt bis zum naechsten Aufruf.
const char *bambuddy_url(const char *path_fmt, ...);
