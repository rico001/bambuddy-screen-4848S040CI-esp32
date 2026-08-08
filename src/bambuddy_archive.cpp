#include "bambuddy_archive.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <string.h>

#include "bambuddy_config.h"
#include "bambuddy_http.h"
#include "bambuddy_status_parse.h"

static constexpr uint32_t REFRESH_INTERVAL_MS = 15000;
static constexpr uint32_t RETRY_AFTER_ERROR_MS = 15000;
static constexpr int FETCH_LIMIT = BB_ARCHIVE_PAGE_SIZE + 1;

static bambuddy_archive_item_t items[BB_ARCHIVE_PAGE_SIZE];
static int item_count = 0;
static volatile bool list_fresh = false;
static SemaphoreHandle_t list_mutex = nullptr;

static volatile bool visible = false;
static uint32_t last_fetch_ms = 0;
static uint32_t last_error_ms = 0;

static int current_page = 0;
static bool has_next_page = false;

static volatile int32_t pending_reprint_id = 0;
static volatile bool pending_clear_plate = false;
static volatile int32_t pending_delete_id = 0;

static char message[64] = "";
static volatile uint32_t message_ms = 0;

static void ensure_mutex()
{
    if (!list_mutex) list_mutex = xSemaphoreCreateMutex();
}

static void set_message(const char *text)
{
    strncpy(message, text, sizeof(message) - 1);
    message[sizeof(message) - 1] = '\0';
    message_ms = millis();
}

static void fetch_page()
{
    const int offset = current_page * BB_ARCHIVE_PAGE_SIZE;

    BambuddyHttp &session = bambuddy_http_shared();
    const char *url = bambuddy_url("/archives/?printer_id=%d&limit=%d&offset=%d",
                                   bambuddy_printer_id(), FETCH_LIMIT, offset);
    if (!session.begin(url, true)) return;

    HTTPClient &http = session.http();

    const int code = http.GET();
    if (code != 200) {
        session.end(false);
        Serial.printf("[Archiv] HTTP %d\n", code);
        last_error_ms = millis();
        return;
    }

    JsonDocument filter;
    JsonObject item_filter = filter.add<JsonObject>();
    item_filter["id"] = true;
    item_filter["print_name"] = true;
    item_filter["filename"] = true;
    item_filter["print_time_seconds"] = true;
    item_filter["filament_used_grams"] = true;
    item_filter["filament_type"] = true;
    item_filter["filament_color"] = true;
    item_filter["status"] = true;
    item_filter["run_count"] = true;

    JsonDocument doc;
    const DeserializationError err =
        deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
    session.end(false);

    if (err) {
        Serial.printf("[Archiv] Antwort nicht lesbar: %s\n", err.c_str());
        last_error_ms = millis();
        return;
    }

    bambuddy_archive_item_t parsed[BB_ARCHIVE_PAGE_SIZE];
    int count = 0;
    int total = 0;

    for (JsonObject obj : doc.as<JsonArray>()) {
        total++;
        if (count >= BB_ARCHIVE_PAGE_SIZE) continue;

        bambuddy_archive_item_t &it = parsed[count];
        memset(&it, 0, sizeof(it));

        it.id = obj["id"] | 0;
        if (it.id == 0) continue;

        const char *name = obj["print_name"] | obj["filename"] | "Unbenannt";
        bambuddy_copy_field(it.name, sizeof(it.name), name);
        it.print_seconds = obj["print_time_seconds"] | 0;
        it.grams = obj["filament_used_grams"] | 0.0f;
        strncpy(it.filament, obj["filament_type"] | "", sizeof(it.filament) - 1);
        it.color = bambuddy_parse_hex_color(obj["filament_color"] | "");
        strncpy(it.status, obj["status"] | "", sizeof(it.status) - 1);
        it.run_count = obj["run_count"] | 0;
        count++;
    }

    ensure_mutex();
    xSemaphoreTake(list_mutex, portMAX_DELAY);
    memset(items, 0, sizeof(items));
    memcpy(items, parsed, sizeof(bambuddy_archive_item_t) * count);
    item_count = count;
    has_next_page = total > BB_ARCHIVE_PAGE_SIZE;
    list_fresh = true;
    xSemaphoreGive(list_mutex);

    last_error_ms = 0;
}

static int send_code(const char *method, const char *url)
{
    BambuddyHttp &session = bambuddy_http_shared();
    if (!session.begin(url, true)) return -1;

    HTTPClient &http = session.http();

    const int code = http.sendRequest(method, (uint8_t *)nullptr, 0);
    session.end(false);

    if (code < 200 || code >= 300) {
        Serial.printf("[Archiv] %s %s -> HTTP %d\n", method, url, code);
    }
    return code;
}

static bool send(const char *method, const char *url)
{
    const int code = send_code(method, url);
    return code >= 200 && code < 300;
}

static bool post(const char *url)
{
    return send("POST", url);
}

static void do_reprint(int32_t archive_id, bool clear_plate)
{
    if (clear_plate) {
        const char *plate_url = bambuddy_url("/printers/%d/clear-plate", bambuddy_printer_id());
        if (!post(plate_url)) Serial.println("[Archiv] clear-plate fehlgeschlagen");
    }

    char url[224];
    snprintf(url, sizeof(url), "%s",
             bambuddy_url("/archives/%d/reprint?printer_id=%d",
                          (int)archive_id, bambuddy_printer_id()));

    if (post(url)) {
        set_message("Archivdruck gestartet");
        Serial.printf("[Archiv] Auftrag %d gestartet\n", (int)archive_id);
    } else {
        set_message("Archivdruck fehlgeschlagen");
    }
}

static void do_delete(int32_t archive_id)
{
    char url[192];
    snprintf(url, sizeof(url), "%s", bambuddy_url("/archives/%d", (int)archive_id));

    const int code = send_code("DELETE", url);
    if (code >= 200 && code < 300) {
        set_message("Archiv geloescht");
        Serial.printf("[Archiv] Eintrag %d geloescht\n", (int)archive_id);

        // Nach dem letzten Eintrag einer Folgeseite direkt zur vorherigen
        // Seite wechseln, statt eine leere Seite anzuzeigen.
        if (item_count <= 1 && current_page > 0) current_page--;
    } else if (code == 409) {
        set_message("Loeschen waehrend Druck nicht moeglich");
    } else {
        set_message("Archiv konnte nicht geloescht werden");
    }

    last_fetch_ms = 0;
}

void bambuddy_archive_set_visible(bool value)
{
    visible = value;
    if (value) last_fetch_ms = 0;
}

bool bambuddy_archive_visible()
{
    return visible;
}

void bambuddy_archive_update()
{
    if (WiFi.status() != WL_CONNECTED || !bambuddy_config_complete()) return;

    const int32_t archive_id = pending_reprint_id;
    if (archive_id != 0) {
        pending_reprint_id = 0;
        do_reprint(archive_id, pending_clear_plate);
    }

    const int32_t delete_id = pending_delete_id;
    if (delete_id != 0) {
        pending_delete_id = 0;
        do_delete(delete_id);
    }

    if (!visible) return;

    const uint32_t now = millis();
    if (last_error_ms && (now - last_error_ms) < RETRY_AFTER_ERROR_MS) return;
    if (last_fetch_ms && (now - last_fetch_ms) < REFRESH_INTERVAL_MS) return;

    last_fetch_ms = now;
    fetch_page();
}

int bambuddy_archive_copy(bambuddy_archive_item_t *out, int max_items)
{
    if (!out || !list_mutex) return 0;

    xSemaphoreTake(list_mutex, portMAX_DELAY);
    const int n = item_count < max_items ? item_count : max_items;
    memcpy(out, items, sizeof(bambuddy_archive_item_t) * n);
    xSemaphoreGive(list_mutex);
    return n;
}

bool bambuddy_archive_take_fresh()
{
    if (!list_fresh) return false;
    list_fresh = false;
    return true;
}

int bambuddy_archive_current_page()
{
    return current_page;
}

bool bambuddy_archive_has_prev_page()
{
    return current_page > 0;
}

bool bambuddy_archive_has_next_page()
{
    return has_next_page;
}

void bambuddy_archive_prev_page()
{
    if (current_page <= 0) return;
    current_page--;
    last_fetch_ms = 0;
}

void bambuddy_archive_next_page()
{
    if (!has_next_page) return;
    current_page++;
    last_fetch_ms = 0;
}

void bambuddy_archive_request_reprint(int32_t archive_id, bool clear_plate)
{
    pending_clear_plate = clear_plate;
    pending_reprint_id = archive_id;
}

void bambuddy_archive_request_delete(int32_t archive_id)
{
    pending_delete_id = archive_id;
}

const char *bambuddy_archive_message()
{
    return message;
}

uint32_t bambuddy_archive_message_age()
{
    return message_ms ? (millis() - message_ms) : UINT32_MAX;
}
