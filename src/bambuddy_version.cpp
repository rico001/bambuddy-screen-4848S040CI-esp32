#include "bambuddy_version.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <string.h>

#include "bambuddy_config.h"
#include "bambuddy_http.h"

// Die Version aendert sich hoechstens beim Aktualisieren der Instanz — ein
// Abgleich alle sechs Stunden genuegt. Beim Oeffnen des System-Screens wird
// ausserdem gezielt nachgefragt.
static constexpr uint32_t REFRESH_INTERVAL_MS = 6UL * 60 * 60 * 1000;
static constexpr uint32_t RETRY_AFTER_ERROR_MS = 60000;
static constexpr uint32_t MIN_REQUEST_GAP_MS = 15000;

static char current_version[24] = "";
static char latest_version[24] = "";
static volatile bool update_available = false;
static volatile bool known = false;
static volatile bool fresh = false;
static char error_text[64] = "";

static uint32_t last_fetch_ms = 0;
static uint32_t last_error_ms = 0;
static volatile bool refresh_requested = true; // beim Start einmal holen

static void set_error(const char *text)
{
    strncpy(error_text, text ? text : "", sizeof(error_text) - 1);
    error_text[sizeof(error_text) - 1] = '\0';
    last_error_ms = millis();
    fresh = true;
}

static void publish(const char *current, const char *latest, bool available)
{
    // Die UI liest diese Puffer aus ihrem eigenen Task. Bleibt die Antwort
    // gleich — und das ist der Normalfall, die Version aendert sich nur beim
    // Aktualisieren der Instanz — wird gar nicht erst geschrieben. Damit gibt
    // es im Dauerbetrieb keinen Schreibzugriff, der mit einem Lesevorgang
    // zusammentreffen koennte.
    if (known && strcmp(current_version, current ? current : "") == 0 &&
        strcmp(latest_version, latest ? latest : "") == 0 &&
        update_available == available && !error_text[0]) {
        last_error_ms = 0;
        return;
    }

    strncpy(current_version, current ? current : "", sizeof(current_version) - 1);
    current_version[sizeof(current_version) - 1] = '\0';
    strncpy(latest_version, latest ? latest : "", sizeof(latest_version) - 1);
    latest_version[sizeof(latest_version) - 1] = '\0';
    update_available = available;
    known = current_version[0] != '\0';
    error_text[0] = '\0';
    last_error_ms = 0;
    fresh = true;
}

// /updates/check braucht den API-Key und liefert zusaetzlich, ob die Instanz
// hinterherhinkt. Die Antwort enthaelt die kompletten Release Notes — ohne
// Filter waeren das etliche Kilobyte im Speicher, mit Filter ueberliest
// ArduinoJson sie direkt aus dem Stream.
static bool fetch_check()
{
    BambuddyHttp &session = bambuddy_http_shared();
    if (!session.begin(bambuddy_url("/updates/check"), true)) return false;

    HTTPClient &http = session.http();
    http.setTimeout(4000);
    http.setConnectTimeout(4000);

    const int code = http.GET();
    if (code != 200) {
        session.end(false);
        Serial.printf("[Version] /updates/check HTTP %d\n", code);
        return false;
    }

    JsonDocument filter;
    filter["current_version"] = true;
    filter["latest_version"] = true;
    filter["update_available"] = true;

    JsonDocument doc;
    const DeserializationError err =
        deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
    session.end(false);
    if (err) {
        Serial.printf("[Version] /updates/check nicht lesbar: %s\n", err.c_str());
        return false;
    }

    const char *current = doc["current_version"] | "";
    if (current[0] == '\0') return false;

    publish(current, doc["latest_version"] | "", doc["update_available"] | false);
    return true;
}

// Ohne gueltigen API-Key bleibt /updates/version: liefert ebenfalls die
// LAUFENDE Fassung, nur ohne Aussage zu verfuegbaren Aktualisierungen.
static bool fetch_version()
{
    BambuddyHttp &session = bambuddy_http_shared();
    if (!session.begin(bambuddy_url("/updates/version"), true)) return false;

    HTTPClient &http = session.http();
    http.setTimeout(4000);
    http.setConnectTimeout(4000);

    const int code = http.GET();
    if (code != 200) {
        session.end(false);
        Serial.printf("[Version] /updates/version HTTP %d\n", code);
        return false;
    }

    JsonDocument filter;
    filter["version"] = true;

    JsonDocument doc;
    const DeserializationError err =
        deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
    session.end(false);
    if (err) return false;

    const char *version = doc["version"] | "";
    if (version[0] == '\0') return false;

    publish(version, "", false);
    return true;
}

void bambuddy_version_update()
{
    if (WiFi.status() != WL_CONNECTED || !bambuddy_base_url()[0]) return;

    const uint32_t now = millis();

    // Ein Fehlversuch muss selbst zum Grund fuer den naechsten werden. Sonst
    // haengt die Wiederholung am Sechs-Stunden-Takt: Wer beim Einschalten
    // schneller ist als das WLAN, saehe bis zum Abend ein graues Fragezeichen.
    const bool retry_due = last_error_ms && (now - last_error_ms) >= RETRY_AFTER_ERROR_MS;
    const bool wanted = refresh_requested || retry_due ||
                        !last_fetch_ms || (now - last_fetch_ms) >= REFRESH_INTERVAL_MS;
    if (!wanted) return;
    if (last_error_ms && !retry_due) return;
    if (last_fetch_ms && (now - last_fetch_ms) < MIN_REQUEST_GAP_MS) return;

    refresh_requested = false;
    last_fetch_ms = now;

    if (fetch_check()) return;
    if (fetch_version()) return;

    set_error("Version nicht abrufbar");
}

void bambuddy_version_request_refresh()
{
    refresh_requested = true;
}

bool bambuddy_version_known()
{
    return known;
}

const char *bambuddy_version_current()
{
    return current_version;
}

const char *bambuddy_version_latest()
{
    return latest_version;
}

bool bambuddy_version_update_available()
{
    return update_available;
}

bool bambuddy_version_matches_tested()
{
    return known && strcmp(current_version, BB_TESTED_VERSION) == 0;
}

const char *bambuddy_version_error()
{
    return error_text;
}

bool bambuddy_version_take_fresh()
{
    if (!fresh) return false;
    fresh = false;
    return true;
}
