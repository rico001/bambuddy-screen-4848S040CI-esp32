#include "bambuddy_queue.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <string.h>

#include "bambuddy_config.h"
#include "bambuddy_http.h"
#include "bambuddy_status_parse.h"

static constexpr uint32_t REFRESH_INTERVAL_MS = 10000;
static constexpr uint32_t HIDDEN_REFRESH_INTERVAL_MS = 30000;
// Die AMS-Belegung aendert sich nur, wenn jemand Spulen wechselt
static constexpr uint32_t AMS_INTERVAL_MS = 60000;
#define MAX_AMS_SLOTS 16
static constexpr uint32_t RETRY_AFTER_ERROR_MS = 15000;

static bambuddy_queue_item_t items[BB_QUEUE_MAX_ITEMS];
static int item_count = 0;
static int total_count = 0;
static volatile bool list_fresh = false;
static SemaphoreHandle_t list_mutex = nullptr;

static volatile bool visible = false;
static uint32_t last_fetch_ms = 0;
static uint32_t last_error_ms = 0;

// Angeforderte Aktionen (0 = nichts zu tun)
static volatile int32_t pending_start_id = 0;
static volatile bool pending_clear_plate = false;
static volatile int32_t pending_delete_id = 0;

static char message[64] = "";
static volatile uint32_t message_ms = 0;

struct ams_slot_t {
    char label[6];   // "A1" ... "D4", mit Reserve fuer zweistellige Slots
    char type[12];   // "PLA", "PETG", ...
    uint32_t color;
};

static ams_slot_t ams_slots[MAX_AMS_SLOTS];
static int ams_slot_count = 0;
static bool ams_known = false;
static uint32_t last_ams_ms = 0;

// ============================================================
// Hilfen
// ============================================================

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

// ============================================================
// Liste holen
// ============================================================

static void fetch_queue()
{
    BambuddyHttp session;
    // Nur anstehende Auftraege dieses Druckers — laufende und erledigte
    // gehoeren nicht in eine Warteschlange.
    const char *url = bambuddy_url("/queue/?printer_id=%d&status=pending",
                                   bambuddy_printer_id());
    if (!session.begin(url)) return;

    HTTPClient &http = session.http();

    const int code = http.GET();
    if (code != 200) {
        session.end();
        Serial.printf("[Queue] HTTP %d\n", code);
        last_error_ms = millis();
        return;
    }

    // Filter: pro Eintrag stehen ueber 50 Felder in der Antwort, wir
    // brauchen sieben davon.
    JsonDocument filter;
    JsonObject item_filter = filter.add<JsonObject>();
    item_filter["id"] = true;
    item_filter["archive_id"] = true;
    item_filter["archive_name"] = true;
    item_filter["library_file_name"] = true;
    item_filter["print_time_seconds"] = true;
    item_filter["filament_used_grams"] = true;
    item_filter["filament_type"] = true;
    item_filter["filament_color"] = true;
    item_filter["manual_start"] = true;

    JsonDocument doc;
    const DeserializationError err =
        deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
    session.end();

    if (err) {
        Serial.printf("[Queue] Antwort nicht lesbar: %s\n", err.c_str());
        last_error_ms = millis();
        return;
    }

    bambuddy_queue_item_t parsed[BB_QUEUE_MAX_ITEMS];
    int count = 0;
    int total = 0;

    for (JsonObject obj : doc.as<JsonArray>()) {
        total++;
        if (count >= BB_QUEUE_MAX_ITEMS) continue; // weiterzaehlen, nicht abbrechen

        bambuddy_queue_item_t &it = parsed[count];
        memset(&it, 0, sizeof(it));

        it.id = obj["id"] | 0;
        if (it.id == 0) continue;

        // Auftraege koennen aus dem Archiv oder aus der Bibliothek kommen
        const char *name = obj["archive_name"] | obj["library_file_name"] | "Unbenannt";
        strncpy(it.name, name, sizeof(it.name) - 1);

        it.archive_id = obj["archive_id"] | 0;
        it.print_seconds = obj["print_time_seconds"] | 0;
        it.grams = obj["filament_used_grams"] | 0.0f;
        strncpy(it.filament, obj["filament_type"] | "", sizeof(it.filament) - 1);
        it.color = bambuddy_parse_hex_color(obj["filament_color"] | "");
        it.manual_start = obj["manual_start"] | false;

        count++;
    }

    ensure_mutex();
    xSemaphoreTake(list_mutex, portMAX_DELAY);
    memcpy(items, parsed, sizeof(bambuddy_queue_item_t) * count);
    item_count = count;
    total_count = total;
    list_fresh = true;
    xSemaphoreGive(list_mutex);

    last_error_ms = 0;
}

// ============================================================
// AMS-Belegung
// ============================================================

static void fetch_ams()
{
    BambuddyHttp session;
    const char *url = bambuddy_url("/printers/%d/status", bambuddy_printer_id());
    if (!session.begin(url)) return;

    HTTPClient &http = session.http();

    if (http.GET() != 200) {
        session.end();
        return;
    }

    // Aus der 3,6-KB-Antwort interessiert nur die AMS-Liste
    JsonDocument filter;
    JsonObject tray = filter["ams"].add<JsonObject>()["tray"].add<JsonObject>();
    tray["id"] = true;
    tray["tray_type"] = true;
    tray["tray_color"] = true;
    tray["exists"] = true;
    filter["ams"][0]["id"] = true;

    JsonDocument doc;
    const DeserializationError err =
        deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
    session.end();
    if (err) return;

    int count = 0;
    for (JsonObject unit : doc["ams"].as<JsonArray>()) {
        const int unit_id = unit["id"] | 0;
        for (JsonObject t : unit["tray"].as<JsonArray>()) {
            if (count >= MAX_AMS_SLOTS) break;
            if (!(t["exists"] | false)) continue;

            const char *type = t["tray_type"] | "";
            if (type[0] == '\0') continue; // leerer Slot

            // Beschriftung wie im Bambu-Umfeld: Einheit als Buchstabe,
            // Slot als Ziffer ab 1 — also A1 bis A4 beim ersten AMS.
            // Werte aus dem JSON sind ungeprueft, deshalb eingrenzen: sonst
            // passt die Beschriftung nicht in den Puffer.
            const int slot_no = (int)(t["id"] | 0) + 1;
            if (slot_no < 1 || slot_no > 99) continue;
            const char unit_letter = (unit_id >= 0 && unit_id < 26)
                                         ? (char)('A' + unit_id) : '?';

            ams_slot_t &slot = ams_slots[count];
            snprintf(slot.label, sizeof(slot.label), "%c%d", unit_letter, slot_no);
            strncpy(slot.type, type, sizeof(slot.type) - 1);
            slot.type[sizeof(slot.type) - 1] = '\0';

            slot.color = bambuddy_parse_hex_color(t["tray_color"] | "");
            count++;
        }
    }

    ams_slot_count = count;
    ams_known = true;
}

// ============================================================
// Starten
// ============================================================

static bool send(const char *method, const char *url)
{
    BambuddyHttp session;
    if (!session.begin(url)) return false;

    HTTPClient &http = session.http();

    const int code = http.sendRequest(method, (uint8_t *)nullptr, 0);
    session.end();

    if (code < 200 || code >= 300) {
        Serial.printf("[Queue] %s %s -> HTTP %d\n", method, url, code);
        return false;
    }
    return true;
}

static bool post(const char *url)
{
    return send("POST", url);
}

static void do_start(int32_t item_id, bool clear_plate)
{
    // Wartet der Drucker auf die Bestaetigung, dass die Platte frei ist,
    // wuerde ein Start sonst abgelehnt. Die Rueckfrage im Screen ist genau
    // diese Bestaetigung — also hier weiterreichen.
    if (clear_plate) {
        const char *url = bambuddy_url("/printers/%d/clear-plate", bambuddy_printer_id());
        if (!post(url)) Serial.println("[Queue] clear-plate fehlgeschlagen");
    }

    char url[192];
    snprintf(url, sizeof(url), "%s", bambuddy_url("/queue/%d/start", (int)item_id));

    if (post(url)) {
        set_message("Druck gestartet");
        Serial.printf("[Queue] Auftrag %d gestartet\n", (int)item_id);
        last_fetch_ms = 0; // Liste gleich neu holen
    } else {
        set_message("Start fehlgeschlagen");
    }
}

static void do_delete(int32_t item_id)
{
    char url[192];
    snprintf(url, sizeof(url), "%s", bambuddy_url("/queue/%d", (int)item_id));

    if (send("DELETE", url)) {
        set_message("Aus der Warteschlange entfernt");
        Serial.printf("[Queue] Auftrag %d entfernt\n", (int)item_id);
        last_fetch_ms = 0;
    } else {
        set_message("Entfernen fehlgeschlagen");
    }
}

// ============================================================
// Public API
// ============================================================

void bambuddy_queue_set_visible(bool value)
{
    visible = value;
    if (value) last_fetch_ms = 0; // beim Aufschlagen sofort holen
}

bool bambuddy_queue_visible()
{
    return visible;
}

void bambuddy_queue_update()
{
    if (WiFi.status() != WL_CONNECTED || !bambuddy_config_complete()) return;

    // Befehle auch dann ausfuehren, wenn der Screen inzwischen weg ist
    const int32_t start_id = pending_start_id;
    if (start_id != 0) {
        pending_start_id = 0;
        do_start(start_id, pending_clear_plate);
    }

    const int32_t delete_id = pending_delete_id;
    if (delete_id != 0) {
        pending_delete_id = 0;
        do_delete(delete_id);
    }

    const uint32_t now = millis();
    if (last_error_ms && (now - last_error_ms) < RETRY_AFTER_ERROR_MS) return;
    const uint32_t interval = visible ? REFRESH_INTERVAL_MS : HIDDEN_REFRESH_INTERVAL_MS;
    if (last_fetch_ms && (now - last_fetch_ms) < interval) return;

    // AMS-Belegung deutlich seltener holen als die Liste
    if (visible && (!last_ams_ms || (now - last_ams_ms) >= AMS_INTERVAL_MS)) {
        last_ams_ms = now;
        fetch_ams();
    }

    last_fetch_ms = now;
    fetch_queue();
}

int bambuddy_queue_copy(bambuddy_queue_item_t *out, int max_items)
{
    if (!out || !list_mutex) return 0;

    xSemaphoreTake(list_mutex, portMAX_DELAY);
    const int n = item_count < max_items ? item_count : max_items;
    memcpy(out, items, sizeof(bambuddy_queue_item_t) * n);
    xSemaphoreGive(list_mutex);

    return n;
}

bool bambuddy_queue_match_slot(const char *filament_type, uint32_t color,
                               char *out, size_t out_len)
{
    if (out && out_len) out[0] = '\0';
    if (!ams_known || !filament_type || filament_type[0] == '\0') return false;

    int best = -1;
    long best_distance = 0;

    for (int i = 0; i < ams_slot_count; i++) {
        if (strcasecmp(ams_slots[i].type, filament_type) != 0) continue;

        // Farbabstand im RGB-Wuerfel — grob, aber es geht nur darum, unter
        // mehreren gleichen Materialien das naheliegendste zu waehlen.
        const long dr = (long)((ams_slots[i].color >> 16) & 0xFF) - (long)((color >> 16) & 0xFF);
        const long dg = (long)((ams_slots[i].color >> 8) & 0xFF) - (long)((color >> 8) & 0xFF);
        const long db = (long)(ams_slots[i].color & 0xFF) - (long)(color & 0xFF);
        const long distance = dr * dr + dg * dg + db * db;

        if (best < 0 || distance < best_distance) {
            best = i;
            best_distance = distance;
        }
    }

    if (best < 0) return false;
    if (out && out_len) {
        strncpy(out, ams_slots[best].label, out_len - 1);
        out[out_len - 1] = '\0';
    }
    return true;
}

int bambuddy_queue_total()
{
    return total_count;
}

bool bambuddy_queue_take_fresh()
{
    if (!list_fresh) return false;
    list_fresh = false;
    return true;
}

void bambuddy_queue_request_start(int32_t item_id, bool clear_plate)
{
    pending_clear_plate = clear_plate;
    pending_start_id = item_id;
}

void bambuddy_queue_request_delete(int32_t item_id)
{
    pending_delete_id = item_id;
}

const char *bambuddy_queue_message()
{
    return message;
}

uint32_t bambuddy_queue_message_age()
{
    return message_ms ? (millis() - message_ms) : UINT32_MAX;
}
