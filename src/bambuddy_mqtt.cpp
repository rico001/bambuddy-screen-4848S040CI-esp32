#include "bambuddy_mqtt.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <WiFi.h>

#include "bambuddy_api.h"
#include "bambuddy_config.h"
#include "bambuddy_status_parse.h"

// Der Status-Payload ist knapp 1 KB. PubSubClient verwirft zu grosse
// Nachrichten stillschweigend — deshalb reichlich Puffer.
static constexpr uint16_t MQTT_BUFFER = 4096;
// KeepAlive muss laenger sein als ein Kamera-Abruf den Task blockiert (~10 s),
// sonst fliegt die Verbindung waehrend des Snapshots raus.
static constexpr uint16_t KEEPALIVE_S = 30;
static constexpr uint16_t SOCKET_TIMEOUT_S = 5;
static constexpr uint32_t RECONNECT_INTERVAL_MS = 5000;
static constexpr uint32_t TCP_CONNECT_TIMEOUT_MS = 3000;

static WiFiClient tcp_client;
static PubSubClient mqtt(tcp_client);

static bool configured = false;
static bool wifi_was_connected = false;
static uint32_t last_attempt_ms = 0;
static char subscribed_topic[129] = "";

// ============================================================
// Diagnose
// ============================================================

static const char *state_text(int state)
{
    switch (state) {
    case MQTT_CONNECTION_TIMEOUT:      return "Zeitueberschreitung";
    case MQTT_CONNECTION_LOST:         return "Verbindung verloren";
    case MQTT_CONNECT_FAILED:          return "Broker nicht erreichbar";
    case MQTT_DISCONNECTED:            return "getrennt";
    case MQTT_CONNECTED:               return "verbunden";
    case MQTT_CONNECT_BAD_PROTOCOL:    return "Protokoll abgelehnt";
    case MQTT_CONNECT_BAD_CLIENT_ID:   return "Client-ID abgelehnt";
    case MQTT_CONNECT_UNAVAILABLE:     return "Broker nicht verfuegbar";
    case MQTT_CONNECT_BAD_CREDENTIALS: return "Benutzer oder Passwort falsch";
    case MQTT_CONNECT_UNAUTHORIZED:    return "Zugriff verweigert";
    default:                           return "unbekannter Fehler";
    }
}

// ============================================================
// Eingehende Nachrichten
// ============================================================

static void on_message(char *topic, uint8_t *payload, unsigned int len)
{
    JsonDocument doc;
    const DeserializationError err = deserializeJson(doc, payload, len);
    if (err) {
        Serial.printf("[MQTT] Payload nicht lesbar: %s\n", err.c_str());
        bambuddy_api_report_link(BB_LINK_NO_SERVER, "MQTT-Nachricht nicht lesbar");
        return;
    }

    bambuddy_status_t status;
    bambuddy_status_from_json(doc, &status);
    status.updated_ms = millis();

    bambuddy_api_publish_status(&status);
}

// ============================================================
// Verbindung
// ============================================================

static void configure_client()
{
    mqtt.setServer(bambuddy_mqtt_host(), bambuddy_mqtt_port());
    mqtt.setBufferSize(MQTT_BUFFER);
    mqtt.setKeepAlive(KEEPALIVE_S);
    mqtt.setSocketTimeout(SOCKET_TIMEOUT_S);
    mqtt.setCallback(on_message);
    configured = true;
}

// Aendert sich die Konfiguration im laufenden Betrieb, muss die Verbindung
// neu aufgebaut werden — sonst haengt das Display am alten Broker.
static bool config_changed()
{
    return strcmp(subscribed_topic, bambuddy_mqtt_topic()) != 0;
}

static void force_disconnect()
{
    mqtt.disconnect();
    tcp_client.stop();
    subscribed_topic[0] = '\0';
}

static void connect()
{
    const uint32_t now = millis();
    if (now - last_attempt_ms < RECONNECT_INTERVAL_MS) return;
    last_attempt_ms = now;

    // WL_CONNECTED allein reicht nicht: nach einem WLAN-Reconnect kommt der
    // Status kurz vor dem DHCP-Lease. Ohne IP scheitert jeder Verbindungsversuch.
    if (WiFi.localIP() == IPAddress(0, 0, 0, 0)) return;

    if (!configured || config_changed()) configure_client();

    // Alten Socket sicher schliessen, sonst haengt ein halboffener herum
    tcp_client.stop();

    // TCP selbst aufbauen, mit Timeout: PubSubClient ruft connect() ohne
    // Timeout auf und kann dabei minutenlang haengen. Klappt der Vorab-
    // Connect, ueberspringt PubSubClient den TCP-Teil.
    if (!tcp_client.connect(bambuddy_mqtt_host(), bambuddy_mqtt_port(), TCP_CONNECT_TIMEOUT_MS)) {
        Serial.printf("[MQTT] %s:%d nicht erreichbar\n",
                      bambuddy_mqtt_host(), bambuddy_mqtt_port());
        bambuddy_api_report_link(BB_LINK_NO_SERVER, "MQTT-Broker nicht erreichbar");
        tcp_client.stop();
        return;
    }

    char client_id[32];
    snprintf(client_id, sizeof(client_id), "bambuddy-display-%06X",
             (uint32_t)(ESP.getEfuseMac() & 0xFFFFFF));

    const char *user = bambuddy_mqtt_user();
    const bool ok = user[0]
                        ? mqtt.connect(client_id, user, bambuddy_mqtt_pass())
                        : mqtt.connect(client_id);

    if (!ok) {
        const int state = mqtt.state();
        char msg[64];
        snprintf(msg, sizeof(msg), "MQTT: %s", state_text(state));
        Serial.printf("[MQTT] Anmeldung fehlgeschlagen: %s (rc=%d)\n", state_text(state), state);
        bambuddy_api_report_link(BB_LINK_NO_SERVER, msg);
        tcp_client.stop();
        return;
    }

    if (!mqtt.subscribe(bambuddy_mqtt_topic())) {
        Serial.println("[MQTT] Abo fehlgeschlagen");
        bambuddy_api_report_link(BB_LINK_NO_SERVER, "MQTT-Topic konnte nicht abonniert werden");
        force_disconnect();
        return;
    }

    strncpy(subscribed_topic, bambuddy_mqtt_topic(), sizeof(subscribed_topic) - 1);
    subscribed_topic[sizeof(subscribed_topic) - 1] = '\0';

    Serial.printf("[MQTT] verbunden mit %s:%d, abonniert: %s\n",
                  bambuddy_mqtt_host(), bambuddy_mqtt_port(), subscribed_topic);

    bambuddy_api_report_link(BB_LINK_OK, "");

    // Sofort einmal pumpen: der Broker schickt direkt nach dem Abo eine
    // eventuell vorhandene retained Message. Ohne diesen Aufruf stuende der
    // Screen bis zur naechsten Statusaenderung leer da.
    mqtt.loop();
}

// ============================================================
// Public API
// ============================================================

void bambuddy_mqtt_loop()
{
    const bool wifi_connected = (WiFi.status() == WL_CONNECTED);

    // WLAN-Verlust sofort nach unten durchreichen: sonst bleibt ein
    // halboffener Socket stehen und der naechste Connect scheitert.
    if (wifi_was_connected && !wifi_connected) {
        Serial.println("[MQTT] WLAN weg — trenne Verbindung");
        force_disconnect();
    }
    wifi_was_connected = wifi_connected;

    if (!wifi_connected) {
        bambuddy_api_report_link(BB_LINK_NO_WIFI, "Kein WLAN");
        return;
    }
    if (!bambuddy_mqtt_config_complete()) {
        bambuddy_api_report_link(BB_LINK_NO_CONFIG, "MQTT-Broker oder Topic fehlen");
        return;
    }

    if (!mqtt.connected()) {
        connect();
        return;
    }

    if (config_changed()) {
        Serial.println("[MQTT] Konfiguration geaendert — verbinde neu");
        force_disconnect();
        return;
    }

    if (!mqtt.loop()) {
        const int state = mqtt.state();
        Serial.printf("[MQTT] Verbindung verloren: %s (rc=%d)\n", state_text(state), state);
        force_disconnect();
    }
}

void bambuddy_mqtt_stop()
{
    force_disconnect();
    configured = false;
}

bool bambuddy_mqtt_connected()
{
    return mqtt.connected();
}
