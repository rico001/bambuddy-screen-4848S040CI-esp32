#include "bambuddy_smart_plugs.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <string.h>

#include <Preferences.h>

#include "bambuddy_api.h"
#include "bambuddy_config.h"
#include "bambuddy_hms.h"
#include "bambuddy_http.h"
#include "bambuddy_status_parse.h"

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
    if (!session.begin(url, true)) return false;

    HTTPClient &http = session.http();
    // Ein langsames einzelnes Geraet darf nicht die gesamte Liste blockieren.
    http.setTimeout(2500);
    http.setConnectTimeout(2500);

    read_request_active = true;
    if (!visible) {
        read_request_active = false;
        session.end(false);
        return false;
    }
    const int code = http.GET();
    if (code != 200) {
        session.end(false);
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
    session.end(false);
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
    if (!session.begin(url, true)) return;

    HTTPClient &http = session.http();
    http.setTimeout(2500);
    http.setConnectTimeout(2500);

    read_request_active = true;
    if (!visible) {
        read_request_active = false;
        session.end(false);
        return;
    }
    const int code = http.GET();
    if (code != 200) {
        session.end(false);
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
    session.end(false);
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

        bambuddy_copy_field(item.name, sizeof(item.name), obj["name"] | "Smart Plug");
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
    if (!session.begin(url, true)) {
        set_message("Schalten fehlgeschlagen");
        return;
    }

    HTTPClient &http = session.http();
    http.addHeader("Content-Type", "application/json");

    const int code = http.POST(turn_on ? "{\"action\":\"on\"}"
                                       : "{\"action\":\"off\"}");
    session.end(false);

    if (code >= 200 && code < 300) {
        set_message(turn_on ? "Smart Plug eingeschaltet" : "Smart Plug ausgeschaltet");
    } else {
        char text[48];
        snprintf(text, sizeof(text), "Schalten fehlgeschlagen (HTTP %d)", code);
        set_message(text);
    }
    last_fetch_ms = 0;
}

// ============================================================
// Nach dem Druck ausschalten
// ============================================================
//
// Warum das hier und nicht in Bambuddy: siehe Kopf von
// bambuddy_smart_plugs.h — beide Wege der API sind fuer einen API-Schluessel
// zu. Das Display kann die Steckdose aber schalten, und den Zustandswechsel
// sieht es ohnehin.

// Nachlauf nach FINISH. Zehn Minuten — doppelt so lang wie Bambuddys eigener
// Standard: Zeit genug, dass die Luefter die Duese herunterkuehlen und dass
// man das Werkstueck noch bei laufendem Licht abnimmt, bevor der Strom faellt.
static constexpr uint32_t AUTO_OFF_DELAY_MS = 10 * 60 * 1000;

// Darunter gilt der Drucker als kalt genug. Der Strom faellt nicht, solange
// die Duese heisser ist — sonst steht die Luft in der Kammer still, waehrend
// das Filament in der heissen Duese weiterkocht.
static constexpr float AUTO_OFF_NOZZLE_MAX_C = 50.0f;

// Sicherheitsnetz gegen einen Drucker, der nie abkuehlt (Duesenfuehler
// defekt, Wert bleibt stehen): Nach dieser Zeit wird trotzdem geschaltet.
static constexpr uint32_t AUTO_OFF_MAX_WAIT_MS = 30 * 60 * 1000;

static constexpr const char *AUTO_OFF_NS = "plugs";
static constexpr const char *AUTO_OFF_KEY = "autooff";

static volatile bool auto_off_enabled = false;
static bool auto_off_loaded = false;
static bool auto_off_saw_running = false;
static uint32_t auto_off_since_ms = 0; // seit wann steht FINISH an?
static char auto_off_last_state[16] = "";

static void auto_off_load()
{
    if (auto_off_loaded) return;
    auto_off_loaded = true;

    Preferences prefs;
    prefs.begin(AUTO_OFF_NS, true);
    auto_off_enabled = prefs.getBool(AUTO_OFF_KEY, false);
    prefs.end();
}

// Welche Steckdose versorgt den Drucker? Bambuddy beantwortet das selbst —
// bei mehreren zugeordneten Dosen (Drucker, Gehaeuseluefter, Skript) liefert
// dieser Endpunkt die, die wirklich den Strom des Druckers schaltet. Die
// Auswahl hier nachzubauen hiesse, sie beim naechsten Umbau falsch zu haben.
static int32_t fetch_printer_plug_id(char *name_out, size_t name_len)
{
    if (name_out && name_len) name_out[0] = '\0';

    BambuddyHttp &session = smart_plug_http;
    const char *url = bambuddy_url("/smart-plugs/by-printer/%d", bambuddy_printer_id());
    if (!session.begin(url, true)) return 0;

    HTTPClient &http = session.http();
    http.setTimeout(2500);
    http.setConnectTimeout(2500);

    const int code = http.GET();
    if (code != 200) {
        session.end(false);
        Serial.printf("[Smart Plugs] Drucker-Steckdose HTTP %d\n", code);
        return 0;
    }

    JsonDocument filter;
    filter["id"] = true;
    filter["name"] = true;
    filter["controls_printer_power"] = true;

    JsonDocument doc;
    const DeserializationError err =
        deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
    session.end(false);
    if (err) {
        Serial.printf("[Smart Plugs] Drucker-Steckdose nicht lesbar: %s\n", err.c_str());
        return 0;
    }

    if (!(doc["controls_printer_power"] | false)) {
        // Zugeordnet ist sie, aber sie schaltet nicht den Strom des Druckers.
        // Dann lieber nichts tun als das Falsche ausschalten.
        Serial.println("[Smart Plugs] Zugeordnete Steckdose schaltet den Drucker nicht");
        return 0;
    }

    if (name_out && name_len) {
        bambuddy_copy_field(name_out, name_len, doc["name"] | "");
    }
    return doc["id"] | 0;
}

static void auto_off_fire()
{
    char plug_name[40];
    const int32_t plug_id = fetch_printer_plug_id(plug_name, sizeof(plug_name));
    if (plug_id == 0) {
        set_message("Keine Steckdose fuer den Drucker");
        return;
    }

    // Erst ins Protokoll, dann schalten: Haengt das Display an derselben
    // Steckdose, ist danach womoeglich keine Gelegenheit mehr dazu.
    bambuddy_hms_report_auto_off(plug_name);

    control_plug(plug_id, false);
    Serial.printf("[Smart Plugs] Drucker nach Druckende ausgeschaltet (Dose %d)\n",
                  (int)plug_id);
}

static void auto_off_update()
{
    auto_off_load();

    bambuddy_status_t st;
    if (!bambuddy_api_copy_status(&st) || !st.printer_connected) return;

    const bool finished = strcasecmp(st.state, "FINISH") == 0;
    const bool running = strcasecmp(st.state, "RUNNING") == 0;

    // Zustandswechsel verfolgen — unabhaengig davon, ob die Abschaltung
    // gerade eingeschaltet ist. Wer sie mitten im Druck einschaltet, soll
    // nicht bis zum uebernaechsten Auftrag warten muessen.
    if (strcmp(auto_off_last_state, st.state) != 0) {
        bambuddy_copy_field(auto_off_last_state, sizeof(auto_off_last_state), st.state);
        auto_off_since_ms = millis();
    }

    if (running) auto_off_saw_running = true;

    if (!finished) return;
    if (!auto_off_enabled || !auto_off_saw_running) return;

    const uint32_t waiting = millis() - auto_off_since_ms;
    if (waiting < AUTO_OFF_DELAY_MS) return;

    if (st.nozzle > AUTO_OFF_NOZZLE_MAX_C && waiting < AUTO_OFF_MAX_WAIT_MS) return;

    // Nur einmal: Das Merkmal wird geloescht, bevor geschaltet wird. Bleibt
    // der Drucker nach dem Abschalten in FINISH stehen (die letzte Antwort
    // altert ja nur), darf das nicht in eine Endlosschleife laufen.
    auto_off_saw_running = false;
    auto_off_fire();
}

bool bambuddy_auto_off_enabled()
{
    auto_off_load();
    return auto_off_enabled;
}

void bambuddy_auto_off_set(bool on)
{
    auto_off_load();
    auto_off_enabled = on;

    Preferences prefs;
    prefs.begin(AUTO_OFF_NS, false);
    prefs.putBool(AUTO_OFF_KEY, on);
    prefs.end();

    Serial.printf("[Smart Plugs] Nach Druckende ausschalten: %s\n", on ? "an" : "AUS");
}

bool bambuddy_auto_off_pending()
{
    return auto_off_enabled && auto_off_saw_running &&
           strcasecmp(auto_off_last_state, "FINISH") == 0;
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

    // Vor dem Sichtbarkeits-Riegel: Die Abschaltung nach dem Druck muss auch
    // laufen, wenn niemand den Steckdosen-Screen offen hat — meistens gerade
    // dann.
    auto_off_update();

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
