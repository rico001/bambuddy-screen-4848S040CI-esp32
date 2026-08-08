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
#include "bambuddy_archive.h"
#include "bambuddy_config.h"
#include "bambuddy_http.h"
#include "bambuddy_cover.h"
#include "bambuddy_mqtt.h"
#include "bambuddy_queue.h"
#include "bambuddy_smart_plugs.h"
#include "bambuddy_status_parse.h"
#include "settings_screen.h"
#include "ui_watch.h"

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

struct command_request_t {
    bambuddy_cmd_t type;
    float first;
    float second;
};

static char cmd_message[64] = "";
static volatile uint32_t cmd_message_ms = 0;

static constexpr uint32_t RETRY_AFTER_ERROR_MS = 5000;
static constexpr uint32_t AMS_REFRESH_MS = 30000;
static volatile bool ams_visible = false;
static uint32_t last_ams_fetch_ms = 0;

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

static void add_ams_filter(JsonDocument &filter)
{
    filter["ams_exists"] = true;
    filter["tray_now"] = true;
    JsonObject ams_filter = filter["ams"].add<JsonObject>();
    ams_filter["id"] = true;
    ams_filter["humidity"] = true;
    ams_filter["temp"] = true;
    ams_filter["is_ams_ht"] = true;
    JsonObject tray_filter = ams_filter["tray"].add<JsonObject>();
    tray_filter["id"] = true;
    tray_filter["tray_color"] = true;
    tray_filter["tray_type"] = true;
    tray_filter["remain"] = true;
    tray_filter["exists"] = true;
}

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

    BambuddyHttp &session = bambuddy_http_shared();
    const char *url = bambuddy_url("/printers/%d/status", bambuddy_printer_id());
    if (!session.begin(url, true)) {
        set_error(BB_LINK_NO_SERVER, "Ungueltige Server-URL");
        return;
    }

    HTTPClient &http = session.http();

    const int code = http.GET();

    if (code == 401 || code == 403) {
        session.end(false);
        set_error(BB_LINK_UNAUTHORIZED, "API-Key abgelehnt");
        return;
    }
    if (code != 200) {
        session.end(false);
        char msg[64];
        if (code < 0) {
            snprintf(msg, sizeof(msg), "Server nicht erreichbar (%d)", code);
        } else {
            snprintf(msg, sizeof(msg), "Server antwortet mit HTTP %d", code);
        }
        // Bei Transportfehlern (negativer Code) den internen Heap
        // mitschreiben: geht der aus, scheitern Sockets, und das sieht von
        // aussen aus wie ein haengender Server.
        if (code < 0) {
            Serial.printf("[Bambuddy] Status HTTP %d, intern frei=%u\n", code,
                          (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
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
    filter["chamber_light"] = true;
    add_ams_filter(filter);
    filter["awaiting_plate_clear"] = true;
    filter["speed_level"] = true;

    JsonDocument doc;
    const DeserializationError err =
        deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
    session.end(false);

    if (err) {
        set_error(BB_LINK_NO_SERVER, "Antwort nicht lesbar");
        return;
    }

    bambuddy_status_t s;
    bambuddy_status_from_json(doc, &s);
    s.updated_ms = millis();

    bambuddy_api_publish_status(&s);
}

// MQTT-Statusmeldungen enthalten je nach Bambuddy-Version nicht den ganzen
// AMS-Block. Dieser kleine HTTP-Abruf ergaenzt nur die AMS-Felder und laesst
// den restlichen, aktuelleren MQTT-Status unangetastet.
static void fetch_ams_status()
{
    last_ams_fetch_ms = millis();
    if (WiFi.status() != WL_CONNECTED || !bambuddy_config_complete()) return;

    BambuddyHttp &session = bambuddy_http_shared();
    const char *url = bambuddy_url("/printers/%d/status", bambuddy_printer_id());
    if (!session.begin(url, true)) return;

    HTTPClient &http = session.http();

    const int code = http.GET();
    if (code != 200) {
        session.end(false);
        Serial.printf("[AMS] Status HTTP %d\n", code);
        return;
    }

    JsonDocument filter;
    add_ams_filter(filter);

    JsonDocument doc;
    const DeserializationError err =
        deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
    session.end(false);
    if (err) {
        Serial.printf("[AMS] Status nicht lesbar: %s\n", err.c_str());
        return;
    }

    bambuddy_status_t fetched;
    bambuddy_status_from_json(doc, &fetched);
    if (!fetched.ams_data_present) return;

    xSemaphoreTake(status_mutex, portMAX_DELAY);
    shared_status.ams_data_present = true;
    shared_status.ams_exists = fetched.ams_exists;
    shared_status.ams_count = fetched.ams_count;
    shared_status.tray_now = fetched.tray_now;
    memcpy(shared_status.ams, fetched.ams, sizeof(shared_status.ams));
    shared_fresh = true;
    xSemaphoreGive(status_mutex);

    Serial.printf("[AMS] %d Einheit(en) aktualisiert\n", (int)fetched.ams_count);
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

static void execute_command(const command_request_t &cmd)
{
    if (cmd.type == BB_CMD_REFRESH_AMS) {
        fetch_ams_status();
        return;
    }
    if (cmd.type == BB_CMD_REFRESH_SMART_PLUGS) {
        bambuddy_smart_plugs_update();
        return;
    }

    struct cmd_info_t {
        const char *ok_text;
        const char *fail_text;
    };

    char path[112];
    cmd_info_t info;
    switch (cmd.type) {
    case BB_CMD_PAUSE:
        snprintf(path, sizeof(path), "/printers/%d/print/pause", bambuddy_printer_id());
        info = {"Pause gesendet", "Pause fehlgeschlagen"};
        break;
    case BB_CMD_RESUME:
        snprintf(path, sizeof(path), "/printers/%d/print/resume", bambuddy_printer_id());
        info = {"Fortsetzen gesendet", "Fortsetzen fehlgeschlagen"};
        break;
    case BB_CMD_LIGHT_ON:
        snprintf(path, sizeof(path), "/printers/%d/chamber-light?on=true", bambuddy_printer_id());
        info = {"Licht eingeschaltet", "Licht konnte nicht eingeschaltet werden"};
        break;
    case BB_CMD_LIGHT_OFF:
        snprintf(path, sizeof(path), "/printers/%d/chamber-light?on=false", bambuddy_printer_id());
        info = {"Licht ausgeschaltet", "Licht konnte nicht ausgeschaltet werden"};
        break;
    case BB_CMD_SPEED:
        snprintf(path, sizeof(path), "/printers/%d/print-speed?mode=%d",
                 bambuddy_printer_id(), (int)cmd.first);
        info = {"Geschwindigkeit gesendet", "Geschwindigkeit fehlgeschlagen"};
        break;
    case BB_CMD_JOG_XY:
        snprintf(path, sizeof(path), "/printers/%d/xy-jog?x=%.2f&y=%.2f",
                 bambuddy_printer_id(), cmd.first, cmd.second);
        info = {"XY-Bewegung gesendet", "XY-Bewegung fehlgeschlagen"};
        break;
    case BB_CMD_JOG_Z:
        snprintf(path, sizeof(path), "/printers/%d/bed-jog?distance=%.2f",
                 bambuddy_printer_id(), cmd.first);
        info = {"Z-Bewegung gesendet", "Z-Bewegung fehlgeschlagen"};
        break;
    case BB_CMD_JOG_EXTRUDER:
        snprintf(path, sizeof(path), "/printers/%d/extruder-jog?distance=%.2f",
                 bambuddy_printer_id(), cmd.first);
        info = {"Extruderbewegung gesendet", "Extruderbewegung fehlgeschlagen"};
        break;
    case BB_CMD_HOME:
        snprintf(path, sizeof(path), "/printers/%d/home-axes?axes=all", bambuddy_printer_id());
        info = {"Homing gestartet", "Homing fehlgeschlagen"};
        break;
    case BB_CMD_STOP:
    default:
        snprintf(path, sizeof(path), "/printers/%d/print/stop", bambuddy_printer_id());
        info = {"Abbruch gesendet", "Abbruch fehlgeschlagen"};
        break;
    }

    if (WiFi.status() != WL_CONNECTED || !bambuddy_config_complete()) {
        set_command_message("Kein Kontakt zum Server");
        return;
    }

    // Steuerbefehle gehen immer ueber die REST-API — auch wenn der Status
    // per MQTT kommt. Der API-Key muss dafuer "Drucker steuern" duerfen.
    BambuddyHttp &session = bambuddy_http_shared();
    const char *url = bambuddy_url("%s", path);
    if (!session.begin(url, true)) {
        set_command_message(info.fail_text);
        return;
    }

    HTTPClient &http = session.http();

    const int code = http.POST("");
    session.end(false);

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

    uint32_t next_heap_log_ms = 0;

    bool ui_hang_reported = false;

    for (;;) {
        task_heartbeat_ms = millis();

        // Der UI-Thread laeuft auf dem anderen Kern. Bleibt sein
        // Lebenszeichen aus, ist er haengengeblieben — dann schreiben wir
        // von hier aus heraus, was er zuletzt getan hat.
        if (ui_watch_alive_ms && millis() - ui_watch_alive_ms > 3000) {
            if (!ui_hang_reported) {
                ui_hang_reported = true;
                Serial.printf("\n*** UI haengt seit %u ms — letzter Schritt: %s ***\n",
                              (unsigned)(millis() - ui_watch_alive_ms),
                              ui_watch_step ? ui_watch_step : "?");
            }
        } else {
            ui_hang_reported = false;
        }

        // Alle 60 Sekunden den internen Heap protokollieren. Sinkt der Wert
        // ueber Stunden, liegt ein Leck vor — bei zufaelligen Neustarts ist
        // das die erste Frage, und ohne Verlauf kann man sie nicht
        // beantworten.
        if (millis() >= next_heap_log_ms) {
            next_heap_log_ms = millis() + 60000;
            // Stack-Reserve mitloggen: zu klein bedeutet Absturz, zu gross
            // verschenkt internen RAM. Ohne Messwert ist beides Raten.
            Serial.printf("[Speicher] intern frei=%u groesster Block=%u "
                          "Stack-Reserve api=%u\n",
                          (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                          (unsigned)uxTaskGetStackHighWaterMark(nullptr));
        }
        const bool use_mqtt = bambuddy_source_mqtt();

        uint32_t wait_ms;

        if (use_mqtt) {
            // MQTT laeuft in einem eigenen Task und liefert von selbst.
            // Hier ist dann nichts abzufragen — nur die Bildabrufe unten.
            wait_ms = 1000;
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
        // Bewusst NICHT an bambuddy_api_link() gekoppelt: Diese Abrufe gehen
        // ueber HTTP und haben mit der Statusquelle nichts zu tun. Haengte
        // man sie daran, wuerde ein MQTT-Aussetzer auch Archiv, Warteschlange
        // und Smart Plugs lahmlegen — sie laden dann "ewig", obwohl der
        // Server erreichbar ist.
        if (WiFi.status() == WL_CONNECTED && bambuddy_config_complete()) {
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
            bambuddy_archive_update();
            bambuddy_smart_plugs_update();

            if (use_mqtt && ams_visible &&
                (!last_ams_fetch_ms || millis() - last_ams_fetch_ms >= AMS_REFRESH_MS)) {
                fetch_ams_status();
            }
        }

        // Bei offenem Kamera-Vollbild oefter aufwachen — sonst haenge der
        // 3-Sekunden-Takt am Leerlaufintervall von bis zu 30 Sekunden.
        if ((bambuddy_camera_active() || bambuddy_queue_visible() ||
             bambuddy_archive_visible() || bambuddy_smart_plugs_visible()) && wait_ms > 500) {
            wait_ms = 500;
        }

        // Warten, aber auf Tastendruck sofort reagieren: kommt ein Befehl
        // herein, wird er ausgefuehrt und direkt danach der Status neu
        // geholt — sonst haengt die Anzeige dem Knopfdruck hinterher.
        command_request_t cmd;
        if (cmd_queue && xQueueReceive(cmd_queue, &cmd, pdMS_TO_TICKS(wait_ms)) == pdTRUE) {
            execute_command(cmd);
        }
    }
}

// ============================================================
// Public API
// ============================================================

// Der Stack muss in den internen RAM — dort ist es eng, weil LVGL einen
// statischen Pool und das Display einen 128-KB-DMA-Puffer belegen.
//
// Gemessen bleiben von 12 KB ueber 11 KB unbenutzt (Stack-Reserve im
// [Speicher]-Log), der Task braucht also gut 1 KB. 8 KB sind auch mit
// TLS-Handshake reichlich und geben 4 KB zurueck.
static constexpr uint32_t TASK_STACK_BYTES = 8192;

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

    cmd_queue = xQueueCreate(8, sizeof(command_request_t));

    if (!status_mutex || !cmd_queue) {
        Serial.println("[Bambuddy] Mutex oder Befehlsschlange konnte nicht angelegt werden");
        return;
    }

    mbedtls_platform_set_calloc_free(psram_calloc, psram_free);

    bambuddy_mqtt_start();

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
        // Bewusst ohne %f: Die Fliesskomma-Formatierung von printf braucht
        // ueber _dtoa_r mehrere Kilobyte Stack und allokiert dabei. Im
        // MQTT-Task mit seinem kleinen Stack reicht das nicht — und fuer
        // eine Prozentangabe ist die Nachkommastelle ohnehin belanglos.
        Serial.printf("[Bambuddy] state=%s progress=%d%% remaining_time=%d (roh)\n",
                      status->state, (int)(status->progress + 0.5f),
                      (int)status->remaining_min);
    }

    xSemaphoreTake(status_mutex, portMAX_DELAY);
    bambuddy_status_t merged = *status;
    if (!status->ams_data_present && shared_status.ams_data_present) {
        merged.ams_data_present = true;
        merged.ams_exists = shared_status.ams_exists;
        merged.ams_count = shared_status.ams_count;
        merged.tray_now = shared_status.tray_now;
        memcpy(merged.ams, shared_status.ams, sizeof(merged.ams));
    }
    shared_status = merged;
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

bool bambuddy_api_copy_status(bambuddy_status_t *out)
{
    if (!status_mutex || !out) return false;

    xSemaphoreTake(status_mutex, portMAX_DELAY);
    *out = shared_status;
    const bool available = shared_status.updated_ms != 0;
    xSemaphoreGive(status_mutex);
    return available;
}

void bambuddy_api_set_ams_visible(bool visible)
{
    ams_visible = visible;
    if (visible && cmd_queue) {
        const command_request_t cmd = {BB_CMD_REFRESH_AMS, 0.0f, 0.0f};
        xQueueSend(cmd_queue, &cmd, 0);
    }
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
    const command_request_t request = {cmd, 0.0f, 0.0f};
    return xQueueSend(cmd_queue, &request, 0) == pdTRUE;
}

static bool send_motion_command(bambuddy_cmd_t type, float first, float second = 0.0f)
{
    if (!cmd_queue) return false;
    const command_request_t request = {type, first, second};
    return xQueueSend(cmd_queue, &request, 0) == pdTRUE;
}

bool bambuddy_api_send_speed(int mode)
{
    if (mode < 1 || mode > 4) return false;
    return send_motion_command(BB_CMD_SPEED, (float)mode);
}

bool bambuddy_api_send_xy_jog(float x, float y)
{
    return send_motion_command(BB_CMD_JOG_XY, x, y);
}

bool bambuddy_api_send_z_jog(float distance)
{
    return send_motion_command(BB_CMD_JOG_Z, distance);
}

bool bambuddy_api_send_extruder_jog(float distance)
{
    return send_motion_command(BB_CMD_JOG_EXTRUDER, distance);
}

bool bambuddy_api_send_home()
{
    return send_motion_command(BB_CMD_HOME, 0.0f);
}

bool bambuddy_api_refresh_smart_plugs()
{
    if (!cmd_queue) return false;
    const command_request_t request = {BB_CMD_REFRESH_SMART_PLUGS, 0.0f, 0.0f};
    return xQueueSend(cmd_queue, &request, 0) == pdTRUE;
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
