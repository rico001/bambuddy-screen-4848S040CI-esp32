#include "bambuddy_api.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <lvgl.h>
#include <mbedtls/platform.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "bambuddy_camera.h"
#include "bambuddy_config.h"
#include "bambuddy_http.h"
#include "bambuddy_cover.h"
#include "bambuddy_mqtt.h"
#include "bambuddy_queue.h"
#include "bambuddy_status_parse.h"
#include "settings_screen.h"

// ============================================================
// Geteilter Zustand zwischen Netzwerk-Task und UI
// ============================================================
static bambuddy_status_t shared_status;
static volatile bool shared_fresh = false;
static volatile bambuddy_link_t shared_link = BB_LINK_STARTING;
static volatile uint32_t task_heartbeat_ms = 0;
static char shared_error[64] = "";
static SemaphoreHandle_t status_mutex = nullptr;

static TaskHandle_t api_task_handle = nullptr;
static QueueHandle_t cmd_queue = nullptr;

static char cmd_message[64] = "";
static volatile uint32_t cmd_message_ms = 0;

static constexpr uint32_t HTTP_TIMEOUT_MS = 8000;
static constexpr uint32_t RETRY_AFTER_ERROR_MS = 5000;

// ============================================================
// TLS-Speicher ins PSRAM verlagern
// ============================================================

// Ein TLS-Handshake belegt schnell 40-50 KB. Der interne RAM wird aber von
// LVGL, dem WiFi-Stack und den Task-Stacks gebraucht — und wir bauen pro
// Statusabfrage eine neue Verbindung auf. Deshalb bekommt mbedTLS seinen
// Speicher aus dem PSRAM.
static void *psram_calloc(size_t n, size_t size)
{
    void *ptr = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ptr) ptr = calloc(n, size); // PSRAM voll: lieber intern als gar nicht
    return ptr;
}

static void psram_free(void *ptr)
{
    free(ptr); // free() kennt beide Heaps
}

// ============================================================
// Hilfen
// ============================================================

static void set_error(bambuddy_link_t link, const char *msg)
{
    xSemaphoreTake(status_mutex, portMAX_DELAY);
    shared_link = link;
    strncpy(shared_error, msg ? msg : "", sizeof(shared_error) - 1);
    shared_error[sizeof(shared_error) - 1] = '\0';
    xSemaphoreGive(status_mutex);
}

// Zustaende, in denen ein Auftrag auf dem Drucker liegt. PREPARE deckt das
// Vorbereiten ab (Bett heizen, Kalibrieren) — da gibt es schon ein Modell,
// aber noch keinen Fortschritt.
static bool state_has_job(const char *state)
{
    static const char *active[] = {"RUNNING", "printing", "PREPARE", "preparing",
                                   "PAUSE", "paused"};
    for (const char *s : active) {
        if (strcasecmp(state, s) == 0) return true;
    }
    return false;
}

static bool state_is_printing(const char *state)
{
    // Der Rohwert kommt vom Drucker durch — je nach Firmware gross- oder
    // kleingeschrieben, deshalb ohne Ruecksicht auf Schreibweise vergleichen.
    return strcasecmp(state, "RUNNING") == 0 || strcasecmp(state, "printing") == 0;
}

// ============================================================
// HTTP
// ============================================================

// Holt den Druckerstatus und schreibt ihn in den geteilten Zustand.
static void poll_status()
{
    if (WiFi.status() != WL_CONNECTED) {
        set_error(BB_LINK_NO_WIFI, "Kein WLAN");
        return;
    }
    if (!bambuddy_config_complete()) {
        set_error(BB_LINK_NO_CONFIG, "URL oder API-Key fehlen");
        return;
    }

    BambuddyHttp session;
    const char *url = bambuddy_url("/printers/%d/status", bambuddy_printer_id());
    if (!session.begin(url)) {
        set_error(BB_LINK_NO_SERVER, "Ungueltige Server-URL");
        return;
    }

    HTTPClient &http = session.http();
    http.addHeader("X-API-Key", bambuddy_api_key());

    const int code = http.GET();

    if (code == 401 || code == 403) {
        session.end();
        set_error(BB_LINK_UNAUTHORIZED, "API-Key abgelehnt");
        return;
    }
    if (code != 200) {
        session.end();
        char msg[64];
        if (code < 0) {
            snprintf(msg, sizeof(msg), "Server nicht erreichbar (%d)", code);
        } else {
            snprintf(msg, sizeof(msg), "Server antwortet mit HTTP %d", code);
        }
        set_error(BB_LINK_NO_SERVER, msg);
        return;
    }

    // Filter: von 3,6 KB Antwort landen nur diese Felder im Speicher.
    JsonDocument filter;
    filter["name"] = true;
    filter["connected"] = true;
    filter["state"] = true;
    filter["subtask_name"] = true;
    filter["progress"] = true;
    filter["remaining_time"] = true;
    filter["layer_num"] = true;
    filter["total_layers"] = true;
    filter["temperatures"] = true;
    filter["awaiting_plate_clear"] = true;

    JsonDocument doc;
    const DeserializationError err =
        deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
    session.end();

    if (err) {
        set_error(BB_LINK_NO_SERVER, "Antwort nicht lesbar");
        return;
    }

    bambuddy_status_t s;
    bambuddy_status_from_json(doc, &s);
    s.updated_ms = millis();

    bambuddy_api_publish_status(&s);
}

// ============================================================
// Steuerbefehle
// ============================================================

static void set_command_message(const char *msg)
{
    strncpy(cmd_message, msg, sizeof(cmd_message) - 1);
    cmd_message[sizeof(cmd_message) - 1] = '\0';
    cmd_message_ms = millis();
}

static void execute_command(bambuddy_cmd_t cmd)
{
    struct cmd_info_t {
        const char *path;
        const char *ok_text;
        const char *fail_text;
    };

    cmd_info_t info;
    switch (cmd) {
    case BB_CMD_PAUSE:
        info = {"/printers/%d/print/pause", "Pause gesendet", "Pause fehlgeschlagen"};
        break;
    case BB_CMD_RESUME:
        info = {"/printers/%d/print/resume", "Fortsetzen gesendet", "Fortsetzen fehlgeschlagen"};
        break;
    case BB_CMD_STOP:
    default:
        info = {"/printers/%d/print/stop", "Abbruch gesendet", "Abbruch fehlgeschlagen"};
        break;
    }

    if (WiFi.status() != WL_CONNECTED || !bambuddy_config_complete()) {
        set_command_message("Kein Kontakt zum Server");
        return;
    }

    // Steuerbefehle gehen immer ueber die REST-API — auch wenn der Status
    // per MQTT kommt. Der API-Key muss dafuer "Drucker steuern" duerfen.
    BambuddyHttp session;
    const char *url = bambuddy_url(info.path, bambuddy_printer_id());
    if (!session.begin(url)) {
        set_command_message(info.fail_text);
        return;
    }

    HTTPClient &http = session.http();
    http.addHeader("X-API-Key", bambuddy_api_key());

    const int code = http.POST("");
    session.end();

    if (code >= 200 && code < 300) {
        Serial.printf("[Bambuddy] Befehl OK: %s\n", url);
        set_command_message(info.ok_text);
    } else if (code == 401 || code == 403) {
        Serial.printf("[Bambuddy] Befehl abgelehnt (%d): %s\n", code, url);
        set_command_message("API-Key darf nicht steuern");
    } else {
        Serial.printf("[Bambuddy] Befehl fehlgeschlagen (%d): %s\n", code, url);
        char msg[64];
        snprintf(msg, sizeof(msg), "%s (HTTP %d)", info.fail_text, code);
        set_command_message(msg);
    }
}

// ============================================================
// Task
// ============================================================

static void api_task(void *)
{
    bool was_mqtt = false;
    uint32_t next_camera_check_ms = 0;

    for (;;) {
        task_heartbeat_ms = millis();
        const bool use_mqtt = bambuddy_source_mqtt();

        // Beim Umschalten die alte Quelle sauber schliessen
        if (was_mqtt && !use_mqtt) bambuddy_mqtt_stop();
        was_mqtt = use_mqtt;

        uint32_t wait_ms;

        if (use_mqtt) {
            // MQTT liefert von selbst — wir muessen nur die Verbindung
            // bedienen und koennen entsprechend eng takten.
            bambuddy_mqtt_loop();
            wait_ms = 50;
        } else {
            poll_status();
            if (bambuddy_api_link() != BB_LINK_OK) {
                wait_ms = RETRY_AFTER_ERROR_MS;
            } else if (bambuddy_api_is_printing()) {
                wait_ms = settings_poll_interval_ms();
            } else {
                wait_ms = settings_poll_interval_idle_ms();
            }
        }

        // Modellbild des Auftrags — laedt sich nur nach, wenn ein anderer Job
        // laeuft, nicht im Poll-Takt. Der Abruf geht immer ueber HTTP, auch
        // wenn der Status per MQTT kommt.
        if (bambuddy_api_link() == BB_LINK_OK) {
            char job[sizeof(shared_status.job)];
            bool active;

            xSemaphoreTake(status_mutex, portMAX_DELAY);
            active = shared_status.printer_connected && state_has_job(shared_status.state);
            strncpy(job, shared_status.job, sizeof(job) - 1);
            job[sizeof(job) - 1] = '\0';
            xSemaphoreGive(status_mutex);

            if (active) {
                bambuddy_cover_update(job);
            } else if (bambuddy_cover_has_frame()) {
                bambuddy_cover_reset();
            }

            // Grossbild ausserhalb der Auftragspruefung: die Vorschau aus
            // der Warteschlange wird gerade dann gebraucht, wenn nichts
            // laeuft. Holt nur, wenn ein Vollbild offen ist.
            bambuddy_cover_update_big(job);

            // Kamerabild nur, solange das Vollbild offen ist
            bambuddy_camera_update();

            // Warteschlange nur, solange ihr Screen sichtbar ist —
            // Startbefehle werden aber immer ausgefuehrt.
            bambuddy_queue_update();
        }

        // Bei offenem Kamera-Vollbild oefter aufwachen — sonst haenge der
        // 3-Sekunden-Takt am Leerlaufintervall von bis zu 30 Sekunden.
        if ((bambuddy_camera_active() || bambuddy_queue_visible()) && wait_ms > 500) wait_ms = 500;

        // Warten, aber auf Tastendruck sofort reagieren: kommt ein Befehl
        // herein, wird er ausgefuehrt und direkt danach der Status neu
        // geholt — sonst haengt die Anzeige dem Knopfdruck hinterher.
        bambuddy_cmd_t cmd;
        if (cmd_queue && xQueueReceive(cmd_queue, &cmd, pdMS_TO_TICKS(wait_ms)) == pdTRUE) {
            execute_command(cmd);
        }
    }
}

// ============================================================
// Public API
// ============================================================

// Der Stack muss in den internen RAM — dort ist es eng, weil LVGL einen
// statischen Pool belegt. Die TLS-Puffer liegen dank der mbedTLS-Umleitung
// im PSRAM, deshalb reichen 8 KB fuer HTTP-Client und Handshake.
static constexpr uint32_t TASK_STACK_BYTES = 10240;

static bool try_start_task()
{
    if (api_task_handle) return true;

    const size_t free_8bit = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    const size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    // Core 0: dort laeuft auch der WiFi-Stack. Core 1 bleibt fuer LVGL frei.
    const BaseType_t ok = xTaskCreatePinnedToCore(api_task, "bambuddy", TASK_STACK_BYTES,
                                                  nullptr, 1, &api_task_handle, 0);

    Serial.printf("[Bambuddy] Netzwerk-Task %s (Heap frei=%u intern=%u groesster Block=%u)\n",
                  ok == pdPASS ? "gestartet" : "KONNTE NICHT STARTEN",
                  (unsigned)free_8bit, (unsigned)free_internal, (unsigned)largest);

    if (ok != pdPASS) {
        api_task_handle = nullptr;
        set_error(BB_LINK_NO_SERVER, "Zu wenig Speicher fuer den Netzwerk-Dienst");
        return false;
    }
    return true;
}

// Scheitert der Start an einer Speicherspitze beim Booten, wird es spaeter
// noch einmal versucht — statt bis zum Neustart tot in der Ecke zu liegen.
static void retry_start_cb(lv_timer_t *timer)
{
    if (try_start_task()) lv_timer_delete(timer);
}

void bambuddy_api_start()
{
    if (api_task_handle) return;

    status_mutex = xSemaphoreCreateMutex();
    memset(&shared_status, 0, sizeof(shared_status));

    cmd_queue = xQueueCreate(4, sizeof(bambuddy_cmd_t));

    if (!status_mutex || !cmd_queue) {
        Serial.println("[Bambuddy] Mutex oder Befehlsschlange konnte nicht angelegt werden");
        return;
    }

    mbedtls_platform_set_calloc_free(psram_calloc, psram_free);

    if (!try_start_task()) {
        lv_timer_t *retry = lv_timer_create(retry_start_cb, 5000, nullptr);
        lv_timer_set_repeat_count(retry, -1);
    }
}

void bambuddy_api_publish_status(const bambuddy_status_t *status)
{
    if (!status_mutex || !status) return;

    // Die Einheit von remaining_time ist weder in der OpenAPI-Spec noch im
    // Wiki festgelegt. Wir rechnen mit Minuten (so liefert es der Drucker
    // per MQTT). Beim ersten echten Druck zeigt dieser Log, ob das stimmt:
    // steht hier 90 und es sind noch anderthalb Stunden, passt es.
    if (state_is_printing(status->state)) {
        Serial.printf("[Bambuddy] state=%s progress=%.1f%% remaining_time=%d (roh)\n",
                      status->state, status->progress, (int)status->remaining_min);
    }

    xSemaphoreTake(status_mutex, portMAX_DELAY);
    shared_status = *status;
    shared_fresh = true;
    shared_link = BB_LINK_OK;
    shared_error[0] = '\0';
    xSemaphoreGive(status_mutex);
}

void bambuddy_api_report_link(bambuddy_link_t link, const char *message)
{
    set_error(link, message);
}

bool bambuddy_api_take(bambuddy_status_t *out)
{
    if (!status_mutex || !out) return false;

    xSemaphoreTake(status_mutex, portMAX_DELAY);
    const bool fresh = shared_fresh;
    *out = shared_status;
    shared_fresh = false;
    xSemaphoreGive(status_mutex);

    return fresh;
}

bambuddy_link_t bambuddy_api_link()
{
    return shared_link;
}

uint32_t bambuddy_api_heartbeat()
{
    return task_heartbeat_ms;
}

bool bambuddy_api_send_command(bambuddy_cmd_t cmd)
{
    if (!cmd_queue) return false;
    return xQueueSend(cmd_queue, &cmd, 0) == pdTRUE;
}

const char *bambuddy_api_command_message()
{
    return cmd_message;
}

uint32_t bambuddy_api_command_message_age()
{
    return cmd_message_ms ? (millis() - cmd_message_ms) : UINT32_MAX;
}

const char *bambuddy_api_error()
{
    return shared_error;
}

bool bambuddy_api_is_printing()
{
    if (!status_mutex) return false;

    xSemaphoreTake(status_mutex, portMAX_DELAY);
    const bool printing = shared_status.printer_connected && state_is_printing(shared_status.state);
    xSemaphoreGive(status_mutex);

    return printing;
}

bool bambuddy_api_awaiting_plate_clear()
{
    if (!status_mutex) return false;

    xSemaphoreTake(status_mutex, portMAX_DELAY);
    const bool awaiting = shared_status.awaiting_plate_clear;
    xSemaphoreGive(status_mutex);

    return awaiting;
}

bool bambuddy_api_has_active_job()
{
    if (!status_mutex) return false;

    xSemaphoreTake(status_mutex, portMAX_DELAY);
    const bool active = shared_status.printer_connected && state_has_job(shared_status.state);
    xSemaphoreGive(status_mutex);

    return active;
}
