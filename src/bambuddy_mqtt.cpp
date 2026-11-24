#include "bambuddy_mqtt.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "bambuddy_api.h"
#include "bambuddy_config.h"
#include "bambuddy_status_parse.h"

// Wie gross darf eine Statusnachricht werden?
//
// PubSubClient verwirft zu grosse Nachrichten stillschweigend: kein Fehler,
// keine Logzeile — das Display bleibt einfach auf dem letzten Stand stehen.
//
// Bis Bambuddy 1.3.1 war der Payload knapp 1 KB. Seit 1.3.2 traegt er
// zusaetzlich das komplette AMS (mit Trocknerwerten, je Einheit rund
// 1,6 KB), die externe Spule und die HMS-Fehler samt Klartext. Mit einem
// AMS sind es gemessen 2,8 KB; mit einer zweiten Einheit oder ein paar
// anstehenden Fehlern waeren 4 KB ueberschritten — und die Anzeige froere
// genau dann ein, wenn am Drucker etwas nicht stimmt.
//
// 8 KB reichen fuer vier AMS-Einheiten. Nebenbei landet ein Puffer ueber
// 4 KB im PSRAM statt im internen RAM (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL
// = 4096), um den sich WLAN und Oberflaeche streiten.
static constexpr uint16_t MQTT_BUFFER = 8192;
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
    case MQTT_CONNECTION_TIMEOUT:      return "Zeitüberschreitung";
    case MQTT_CONNECTION_LOST:         return "Verbindung verloren";
    case MQTT_CONNECT_FAILED:          return "Broker nicht erreichbar";
    case MQTT_DISCONNECTED:            return "getrennt";
    case MQTT_CONNECTED:               return "verbunden";
    case MQTT_CONNECT_BAD_PROTOCOL:    return "Protokoll abgelehnt";
    case MQTT_CONNECT_BAD_CLIENT_ID:   return "Client-ID abgelehnt";
    case MQTT_CONNECT_UNAVAILABLE:     return "Broker nicht verfügbar";
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
    // Nur auslesen, was bambuddy_status_from_json() auch auswertet. Seit
    // Bambuddy 1.3.2 traegt der Payload das komplette AMS, das hier ohnehin
    // verworfen wird (siehe unten) — ohne Filter legte ArduinoJson es bei
    // jeder Nachricht trotzdem vollstaendig im Heap an.
    //
    // Wertet der Parser ein neues Feld aus, muss es auch hier dazu, sonst
    // kommt es ueber MQTT stillschweigend als Null an.
    JsonDocument filter;
    filter["connected"] = true;
    filter["name"] = true;
    filter["printer_name"] = true;
    filter["state"] = true;
    filter["subtask_name"] = true;
    filter["progress"] = true;
    filter["remaining_time"] = true;
    filter["layer_num"] = true;
    filter["total_layers"] = true;
    filter["temperatures"] = true;
    filter["chamber_light"] = true;
    filter["awaiting_plate_clear"] = true;
    filter["speed_level"] = true;
    JsonObject hms_filter = filter["hms_errors"].add<JsonObject>();
    hms_filter["full_code"] = true;
    hms_filter["code"] = true;
    hms_filter["severity"] = true;

    JsonDocument doc;
    const DeserializationError err =
        deserializeJson(doc, payload, len, DeserializationOption::Filter(filter));
    if (err) {
        Serial.printf("[MQTT] Payload nicht lesbar: %s\n", err.c_str());
        bambuddy_api_report_link(BB_LINK_NO_SERVER, "MQTT-Nachricht nicht lesbar");
        return;
    }

    bambuddy_status_t status;
    bambuddy_status_from_json(doc, &status);

    // AMS wird bewusst ausschliesslich per HTTP geladen. MQTT-Payloads sind
    // je nach Bambuddy-Version unvollstaendig und duerfen den HTTP-Stand
    // weder ersetzen noch leeren.
    status.ams_data_present = false;
    status.ams_exists = false;
    status.ams_count = 0;
    status.tray_now = 255;
    memset(status.ams, 0, sizeof(status.ams));
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

static void mqtt_service()
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

        // Speicherstand mitschreiben.
        //
        // Abbrueche treten auffaellig oft dann auf, wenn die Oberflaeche
        // gerade viele Objekte anlegt — Bildschirmschoner, langes Scrollen
        // in den Einstellungen. LVGL nimmt dafuer den internen RAM, und aus
        // demselben Speicher holt sich lwIP seine Puffer. Ist das die
        // Ursache, steht es hier: wenig frei oder ein zu kleiner groesster
        // Block. Bleiben die Zahlen dagegen hoch, liegt es am Netz und nicht
        // am Geraet.
        Serial.printf("[MQTT] Verbindung verloren: %s (rc=%d) — intern frei %u, "
                      "groesster Block %u\n",
                      state_text(state), state,
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL |
                                                        MALLOC_CAP_8BIT),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
                                                                 MALLOC_CAP_8BIT));
        force_disconnect();
    }
}

static TaskHandle_t mqtt_task_handle = nullptr;

static void mqtt_task(void *)
{
    uint32_t next_log_ms = 0;

    for (;;) {
        // Stack-Reserve mitschreiben. Genau hier hat eine zu knappe
        // Bemessung schon einmal zugeschlagen: printf mit %f braucht
        // ueberraschend viel, und der Absturz zeigt dann irgendwo im
        // Speicherverwalter, nicht an der Ursache.
        if (millis() >= next_log_ms) {
            next_log_ms = millis() + 300000;
            Serial.printf("[MQTT] Stack-Reserve %u Bytes\n",
                          (unsigned)uxTaskGetStackHighWaterMark(nullptr));
        }

        if (bambuddy_source_mqtt()) {
            mqtt_service();
            vTaskDelay(pdMS_TO_TICKS(50));
        } else {
            // Quelle umgestellt: Verbindung sauber schliessen und ruhen
            if (mqtt.connected()) force_disconnect();
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
}

void bambuddy_mqtt_start()
{
    if (mqtt_task_handle) return;

    const BaseType_t ok = xTaskCreatePinnedToCore(mqtt_task, "bb-mqtt", 8192,
                                                  nullptr, 1, &mqtt_task_handle, 0);
    Serial.printf("[MQTT] Task %s\n", ok == pdPASS ? "gestartet" : "KONNTE NICHT STARTEN");
    if (ok != pdPASS) mqtt_task_handle = nullptr;
}


