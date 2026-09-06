#include "bambuddy_skip.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <string.h>

#include "bambuddy_config.h"
#include "bambuddy_http.h"
#include "bambuddy_status_parse.h"

// Die Liste aendert sich waehrend eines Drucks nur, wenn jemand etwas
// ueberspringt — schnell muss das nicht gehen. Fuenf Sekunden reichen, damit
// die Ansicht nach dem eigenen Befehl von selbst nachzieht.
static constexpr uint32_t REFRESH_INTERVAL_MS = 5000;
static constexpr uint32_t RETRY_AFTER_ERROR_MS = 15000;

static bambuddy_skip_object_t objects[BB_SKIP_MAX_OBJECTS];
static int object_count = 0;
static int total_count = 0;
static float bbox[4] = {0, 0, 0, 0};
static bool bbox_known = false;
static volatile bool list_fresh = false;
static volatile bool loaded = false;
static volatile bool printing = false;
static SemaphoreHandle_t list_mutex = nullptr;

static volatile bool visible = false;
static uint32_t last_fetch_ms = 0;
static uint32_t last_error_ms = 0;

// Angeforderte Befehle. Die Anzahl ist zugleich das Signal: Sie wird als
// letztes gesetzt und als erstes zurueckgenommen, damit der Netzwerk-Task
// nie eine halb gefuellte Liste sieht.
static int32_t pending_ids[BB_SKIP_MAX_OBJECTS];
static volatile int pending_count = 0;

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

// Klartext aus der Fehlerantwort. Bambuddy schreibt dort hinein, warum es
// nicht ging ("No active print") — das ist mehr wert als ein HTTP-Code.
static void read_error_detail(HTTPClient &http, char *out, size_t out_len)
{
    if (out_len) out[0] = '\0';

    JsonDocument filter;
    filter["detail"] = true;

    JsonDocument doc;
    if (deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter))) return;

    const char *detail = doc["detail"] | "";
    if (detail[0]) bambuddy_copy_field(out, out_len, detail);
}

// ============================================================
// Liste holen
// ============================================================

static void fetch_objects()
{
    BambuddyHttp &session = bambuddy_http_shared();
    const char *url = bambuddy_url("/printers/%d/print/objects", bambuddy_printer_id());
    if (!session.begin(url, true)) return;

    HTTPClient &http = session.http();
    const int code = http.GET();

    if (code != 200) {
        Serial.printf("[Objekte] GET -> HTTP %d\n", code);
        session.end(false);
        last_error_ms = millis();
        return;
    }

    JsonDocument filter;
    JsonObject item = filter["objects"].add<JsonObject>();
    item["id"] = true;
    item["name"] = true;
    item["skipped"] = true;
    // Mittelpunkte und der umschliessende Rahmen: Daraus zeichnet die
    // Ansicht die Draufsicht. Ohne sie waeren mehrfach gesetzte Koerper
    // nicht auseinanderzuhalten — sie tragen alle denselben Namen.
    item["x"] = true;
    item["y"] = true;
    filter["bbox_all"] = true;
    filter["total"] = true;
    filter["is_printing"] = true;

    JsonDocument doc;
    const DeserializationError err =
        deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
    session.end(false);

    if (err) {
        Serial.printf("[Objekte] Antwort nicht lesbar: %s\n", err.c_str());
        last_error_ms = millis();
        return;
    }

    // Genullt, nicht nur gefuellt: Weiter unten wird byteweise mit der alten
    // Liste verglichen, und Fuellbytes der Struktur haetten sonst zufaellige
    // Werte vom Stack — jeder Abruf saehe dann nach einer Aenderung aus.
    bambuddy_skip_object_t fresh_list[BB_SKIP_MAX_OBJECTS] = {};
    int fresh_count = 0;

    for (JsonObject obj : doc["objects"].as<JsonArray>()) {
        if (fresh_count >= BB_SKIP_MAX_OBJECTS) break;

        bambuddy_skip_object_t &o = fresh_list[fresh_count];
        o.id = obj["id"] | 0;
        bambuddy_copy_field(o.name, sizeof(o.name), obj["name"] | "");
        o.skipped = obj["skipped"] | false;
        // Fehlt eine Koordinate, ist sie nicht 0 — dort waere die vordere
        // linke Ecke der Platte, und das Objekt saesse falsch in der
        // Draufsicht statt gar nicht.
        o.pos_known = !obj["x"].isNull() && !obj["y"].isNull();
        o.x = obj["x"] | 0.0f;
        o.y = obj["y"] | 0.0f;
        fresh_count++;
    }

    const int fresh_total = doc["total"] | fresh_count;

    float fresh_bbox[4] = {0, 0, 0, 0};
    bool fresh_bbox_known = false;
    JsonArray box = doc["bbox_all"].as<JsonArray>();
    if (box.size() == 4) {
        for (int i = 0; i < 4; i++) fresh_bbox[i] = box[i] | 0.0f;
        fresh_bbox_known = true;
    }

    ensure_mutex();
    xSemaphoreTake(list_mutex, portMAX_DELAY);

    // Nur bei echter Aenderung melden. Die Ansicht baut sich beim Melden neu
    // auf — taete sie das alle fuenf Sekunden, waeren die gesetzten Haken
    // jedes Mal wieder weg, mitten im Auswaehlen.
    const bool changed = fresh_count != object_count || fresh_total != total_count ||
                         !loaded ||
                         memcmp(fresh_list, objects,
                                sizeof(bambuddy_skip_object_t) * fresh_count) != 0;

    memcpy(objects, fresh_list, sizeof(bambuddy_skip_object_t) * fresh_count);
    object_count = fresh_count;
    total_count = fresh_total;
    memcpy(bbox, fresh_bbox, sizeof(bbox));
    bbox_known = fresh_bbox_known;

    xSemaphoreGive(list_mutex);

    printing = doc["is_printing"] | false;
    loaded = true;
    if (changed) list_fresh = true;
    last_fetch_ms = millis();
    last_error_ms = 0;
}

// ============================================================
// Ueberspringen
// ============================================================

static void do_skip(const int32_t *ids, int count)
{
    BambuddyHttp &session = bambuddy_http_shared();
    const char *url = bambuddy_url("/printers/%d/print/skip-objects",
                                   bambuddy_printer_id());
    if (!session.begin(url, true)) {
        set_message("Keine Verbindung");
        return;
    }

    HTTPClient &http = session.http();
    http.addHeader("Content-Type", "application/json");

    // Der Rumpf ist die blosse Liste der Kennungen — kein Objekt darum
    // herum. Bei 32 Objekten mit je hoechstens zehn Stellen bleibt das weit
    // unter der Puffergroesse.
    char body[BB_SKIP_MAX_OBJECTS * 12 + 4];
    int len = snprintf(body, sizeof(body), "[");
    for (int i = 0; i < count; i++) {
        len += snprintf(body + len, sizeof(body) - len, "%s%d",
                        i ? "," : "", (int)ids[i]);
    }
    snprintf(body + len, sizeof(body) - len, "]");

    const int code = http.POST(body);

    if (code < 200 || code >= 300) {
        char detail[96];
        read_error_detail(http, detail, sizeof(detail));
        Serial.printf("[Objekte] Ueberspringen -> HTTP %d%s%s\n", code,
                      detail[0] ? " | " : "", detail);
        set_message(detail[0] ? detail : "Überspringen fehlgeschlagen");
        session.end(false);
        return;
    }

    session.end(false);

    if (count == 1) {
        set_message("Objekt wird übersprungen");
    } else {
        char text[48];
        snprintf(text, sizeof(text), "%d Objekte werden übersprungen", count);
        set_message(text);
    }
    Serial.printf("[Objekte] %d uebersprungen\n", count);

    last_fetch_ms = 0; // Liste gleich neu holen, damit die Haken stimmen
}

// ============================================================
// Public API
// ============================================================

void bambuddy_skip_set_visible(bool value)
{
    visible = value;
    if (!value) return;

    last_fetch_ms = 0; // beim Aufschlagen sofort holen
    last_error_ms = 0;

    // Alte Liste verwerfen: Sie gehoert zum vorigen Druck, und die Namen
    // sehen von einem zum naechsten oft gleich aus. Lieber kurz "Lade ..."
    // als eine Liste, die zu etwas anderem gehoert.
    ensure_mutex();
    xSemaphoreTake(list_mutex, portMAX_DELAY);
    object_count = 0;
    total_count = 0;
    bbox_known = false;
    xSemaphoreGive(list_mutex);

    loaded = false;
    list_fresh = true;
}

bool bambuddy_skip_visible()
{
    return visible;
}

void bambuddy_skip_update()
{
    if (WiFi.status() != WL_CONNECTED || !bambuddy_config_complete()) return;

    // Befehle auch dann ausfuehren, wenn die Ansicht inzwischen zu ist: Wer
    // die Rueckfrage bestaetigt hat, erwartet, dass es geschieht.
    const int count = pending_count;
    if (count > 0) {
        int32_t ids[BB_SKIP_MAX_OBJECTS];
        memcpy(ids, pending_ids, sizeof(int32_t) * count);
        pending_count = 0;
        do_skip(ids, count);
    }

    if (!visible) return;

    if (last_error_ms && millis() - last_error_ms < RETRY_AFTER_ERROR_MS) return;
    if (last_fetch_ms && millis() - last_fetch_ms < REFRESH_INTERVAL_MS) return;

    fetch_objects();
}

int bambuddy_skip_copy(bambuddy_skip_object_t *out, int max_items)
{
    ensure_mutex();
    xSemaphoreTake(list_mutex, portMAX_DELAY);

    const int n = object_count < max_items ? object_count : max_items;
    memcpy(out, objects, sizeof(bambuddy_skip_object_t) * n);

    xSemaphoreGive(list_mutex);
    return n;
}

bool bambuddy_skip_take_fresh()
{
    const bool value = list_fresh;
    list_fresh = false;
    return value;
}

int bambuddy_skip_total()
{
    return total_count;
}

bool bambuddy_skip_bbox(float *out_x0, float *out_y0, float *out_x1, float *out_y1)
{
    ensure_mutex();
    xSemaphoreTake(list_mutex, portMAX_DELAY);

    const bool known = bbox_known;
    if (known) {
        if (out_x0) *out_x0 = bbox[0];
        if (out_y0) *out_y0 = bbox[1];
        if (out_x1) *out_x1 = bbox[2];
        if (out_y1) *out_y1 = bbox[3];
    }

    xSemaphoreGive(list_mutex);
    return known;
}

bool bambuddy_skip_loaded()
{
    return loaded;
}

bool bambuddy_skip_is_printing()
{
    return printing;
}

void bambuddy_skip_request(const int32_t *ids, int count)
{
    if (count <= 0) return;
    if (count > BB_SKIP_MAX_OBJECTS) count = BB_SKIP_MAX_OBJECTS;

    memcpy(pending_ids, ids, sizeof(int32_t) * count);
    pending_count = count;
}

bool bambuddy_skip_pending_work()
{
    return pending_count > 0;
}

const char *bambuddy_skip_message()
{
    return message;
}

uint32_t bambuddy_skip_message_age()
{
    return message_ms ? millis() - message_ms : UINT32_MAX;
}
