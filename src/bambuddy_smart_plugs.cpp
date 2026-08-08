#include "bambuddy_smart_plugs.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <string.h>

#include "bambuddy_config.h"
#include "bambuddy_http.h"

static constexpr uint32_t REFRESH_INTERVAL_MS = 15000;
static constexpr uint32_t RETRY_AFTER_ERROR_MS = 10000;

static bambuddy_smart_plug_t items[BB_SMART_PLUG_MAX_ITEMS];
static int item_count = 0;
static SemaphoreHandle_t items_mutex = nullptr;
static volatile bool list_fresh = false;
static volatile bool data_available = false;
static volatile bool visible = false;
static volatile bool read_request_active = false;
static BambuddyHttp smart_plug_http;

static uint32_t last_fetch_ms = 0;
static uint32_t last_error_ms = 0;
static volatile int32_t pending_control_id = 0;
static volatile bool pending_control_on = false;

static char message[64] = "";
static volatile uint32_t message_ms = 0;

static void ensure_mutex()
{
    if (!items_mutex) items_mutex = xSemaphoreCreateMutex();
}

static void set_message(const char *text)
{
    strncpy(message, text, sizeof(message) - 1);
    message[sizeof(message) - 1] = '\0';
    message_ms = millis();
}

static bool state_is_on(const char *state)
{
    return state && (strcasecmp(state, "on") == 0 ||
                     strcasecmp(state, "true") == 0 || strcmp(state, "1") == 0);
}

static bool fetch_status(bambuddy_smart_plug_t &item)
{
    BambuddyHttp &session = smart_plug_http;
    const char *url = bambuddy_url("/smart-plugs/%d/status", (int)item.id);
    if (!session.begin(url)) return false;

    HTTPClient &http = session.http();
    // Ein langsames einzelnes Geraet darf nicht die gesamte Liste blockieren.
    http.setTimeout(2500);
    http.setConnectTimeout(2500);

    read_request_active = true;
    if (!visible) {
        read_request_active = false;
        session.end();
        return false;
    }
    const int code = http.GET();
    if (code != 200) {
        session.end();
        read_request_active = false;
        if (visible) {
            Serial.printf("[Smart Plugs] Status %d HTTP %d\n", (int)item.id, code);
        }
        return false;
    }

    JsonDocument filter;
    filter["state"] = true;
    filter["reachable"] = true;

    JsonDocument doc;
    const DeserializationError err =
        deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
    session.end();
    read_request_active = false;
    if (err) {
        if (visible) {
            Serial.printf("[Smart Plugs] Status %d nicht lesbar: %s\n",
                          (int)item.id, err.c_str());
        }
        return false;
    }

    const char *state = doc["state"] | "";
    item.state_known = state[0] != '\0';
    item.is_on = state_is_on(state);
    item.reachable = doc["reachable"] | true;
    return true;
}

static void publish_items(const bambuddy_smart_plug_t *parsed, int count)
{
    ensure_mutex();
    xSemaphoreTake(items_mutex, portMAX_DELAY);
    memset(items, 0, sizeof(items));
    memcpy(items, parsed, sizeof(bambuddy_smart_plug_t) * count);
    item_count = count;
    data_available = true;
    list_fresh = true;
    xSemaphoreGive(items_mutex);
}

static void fetch_all()
{
    BambuddyHttp &session = smart_plug_http;
    const char *url = bambuddy_url("/smart-plugs/");
    if (!session.begin(url)) return;

    HTTPClient &http = session.http();
    http.setTimeout(2500);
    http.setConnectTimeout(2500);

    read_request_active = true;
    if (!visible) {
        read_request_active = false;
        session.end();
        return;
    }
    const int code = http.GET();
    if (code != 200) {
        session.end();
        read_request_active = false;
        if (!visible) return;
        Serial.printf("[Smart Plugs] Liste HTTP %d\n", code);
        last_error_ms = millis();
        return;
    }

    JsonDocument filter;
    JsonObject item_filter = filter.add<JsonObject>();
    item_filter["id"] = true;
    item_filter["name"] = true;
    item_filter["enabled"] = true;
    item_filter["last_state"] = true;

    JsonDocument doc;
    const DeserializationError err =
        deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
    session.end();
    read_request_active = false;
    if (err) {
        if (!visible) return;
        Serial.printf("[Smart Plugs] Liste nicht lesbar: %s\n", err.c_str());
        last_error_ms = millis();
        return;
    }

    bambuddy_smart_plug_t parsed[BB_SMART_PLUG_MAX_ITEMS];
    memset(parsed, 0, sizeof(parsed));
    int count = 0;

    for (JsonObject obj : doc.as<JsonArray>()) {
        if (count >= BB_SMART_PLUG_MAX_ITEMS) break;
        if (!(obj["enabled"] | true)) continue;

        bambuddy_smart_plug_t &item = parsed[count];
        item.id = obj["id"] | 0;
        if (item.id == 0) continue;

        strncpy(item.name, obj["name"] | "Smart Plug", sizeof(item.name) - 1);
        const char *last_state = obj["last_state"] | "";
        item.state_known = last_state[0] != '\0';
        item.is_on = state_is_on(last_state);
        item.reachable = true;
        count++;
    }

    // Namen und zuletzt bekannten Zustand sofort anzeigen. Die einzelnen
    // Live-Abfragen duerfen die erste Darstellung nicht verzoegern.
    publish_items(parsed, count);

    for (int i = 0; i < count && visible; i++) {
        if (fetch_status(parsed[i])) publish_items(parsed, count);
    }

    last_error_ms = 0;
}

static void control_plug(int32_t plug_id, bool turn_on)
{
    BambuddyHttp &session = smart_plug_http;
    const char *url = bambuddy_url("/smart-plugs/%d/control", (int)plug_id);
    if (!session.begin(url)) {
        set_message("Schalten fehlgeschlagen");
        return;
    }

    HTTPClient &http = session.http();
    http.addHeader("Content-Type", "application/json");

    const int code = http.POST(turn_on ? "{\"action\":\"on\"}"
                                       : "{\"action\":\"off\"}");
    session.end();

    if (code >= 200 && code < 300) {
        set_message(turn_on ? "Smart Plug eingeschaltet" : "Smart Plug ausgeschaltet");
    } else {
        char text[48];
        snprintf(text, sizeof(text), "Schalten fehlgeschlagen (HTTP %d)", code);
        set_message(text);
    }
    last_fetch_ms = 0;
}

void bambuddy_smart_plugs_set_visible(bool value)
{
    visible = value;
    if (value) {
        last_fetch_ms = 0;
        last_error_ms = 0;
    } else if (read_request_active) {
        // GET laeuft im Netzwerk-Task; Socket-Schliessen laesst den
        // blockierenden Aufruf sofort zurueckkehren.
        smart_plug_http.cancel();
    }
}

bool bambuddy_smart_plugs_visible()
{
    return visible;
}

void bambuddy_smart_plugs_update()
{
    if (WiFi.status() != WL_CONNECTED || !bambuddy_config_complete()) return;

    const int32_t control_id = pending_control_id;
    if (control_id != 0) {
        pending_control_id = 0;
        control_plug(control_id, pending_control_on);
    }

    if (!visible) return;

    const uint32_t now = millis();
    if (last_error_ms && (now - last_error_ms) < RETRY_AFTER_ERROR_MS) return;
    if (last_fetch_ms && (now - last_fetch_ms) < REFRESH_INTERVAL_MS) return;

    last_fetch_ms = now;
    fetch_all();
}

int bambuddy_smart_plugs_copy(bambuddy_smart_plug_t *out, int max_items)
{
    if (!out || !items_mutex) return 0;

    xSemaphoreTake(items_mutex, portMAX_DELAY);
    const int count = item_count < max_items ? item_count : max_items;
    memcpy(out, items, sizeof(bambuddy_smart_plug_t) * count);
    xSemaphoreGive(items_mutex);
    return count;
}

bool bambuddy_smart_plugs_has_data()
{
    return data_available;
}

bool bambuddy_smart_plugs_take_fresh()
{
    if (!list_fresh) return false;
    list_fresh = false;
    return true;
}

void bambuddy_smart_plugs_request_control(int32_t plug_id, bool turn_on)
{
    pending_control_on = turn_on;
    pending_control_id = plug_id;
}

const char *bambuddy_smart_plugs_message()
{
    return message;
}

uint32_t bambuddy_smart_plugs_message_age()
{
    return message_ms ? millis() - message_ms : UINT32_MAX;
}
