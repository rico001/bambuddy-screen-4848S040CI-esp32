#include "ota_service.h"

#include <Arduino.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_ota_ops.h>

#include "bambuddy_version.h"
#include "build_stamp.h"

// ============================================================
// Zustand
// ============================================================

static constexpr uint16_t OTA_PORT = 80;

// Der Task laeuft ab dem ersten Einschalten dauerhaft weiter und pruegt sich
// selbst durch: Ein Task, der sich mitten in einem Upload selbst loescht,
// laesst den halb geschriebenen Flash und einen offenen Socket zurueck.
// Der Webserver dagegen wird wirklich abgeraeumt — er ist das, was Speicher
// kostet, und im ausgeschalteten Zustand soll nichts davon belegt bleiben.
static TaskHandle_t ota_task_handle = nullptr;
static WebServer *server = nullptr;

static volatile bool enabled = false;
static volatile bool online = false;
static volatile int progress_pct = -1;
static volatile uint32_t restart_at_ms = 0;

static char address_buf[32];
static char message_buf[96];

// Wieviel Bytes der Browser angekuendigt hat. Der Multipart-Rahmen macht das
// um ein paar hundert Byte groesser als die Firmware selbst — fuer eine
// Prozentanzeige ist das genau genug.
static size_t upload_expected = 0;

static void set_message(const char *text)
{
    strncpy(message_buf, text ? text : "", sizeof(message_buf) - 1);
    message_buf[sizeof(message_buf) - 1] = '\0';
}

// ============================================================
// Angaben zur laufenden Firmware
// ============================================================

static void uptime_text(char *out, size_t out_len)
{
    const uint32_t total_s = millis() / 1000;
    const uint32_t d = total_s / 86400;
    const uint32_t h = (total_s % 86400) / 3600;
    const uint32_t m = (total_s % 3600) / 60;

    if (d > 0) snprintf(out, out_len, "%ud %uh %umin", (unsigned)d, (unsigned)h, (unsigned)m);
    else if (h > 0) snprintf(out, out_len, "%uh %umin", (unsigned)h, (unsigned)m);
    else snprintf(out, out_len, "%umin", (unsigned)m);
}

// Die ersten Bytes des ELF-Hashes, den die IDF beim Erzeugen des Abbilds
// eintraegt. Version und Bauzeit sagen, was gemeint war; der Hash sagt, was
// es wirklich ist — zwei Builds derselben Minute unterscheidet nur er.
// Dieselben acht Zeichen gibt tools/release.sh fuer die abgelegte Datei aus.
static void build_hash_text(const esp_app_desc_t *desc, char *out, size_t out_len)
{
    if (!desc) {
        snprintf(out, out_len, "unbekannt");
        return;
    }
    snprintf(out, out_len, "%02x%02x%02x%02x%02x%02x%02x%02x",
             desc->app_elf_sha256[0], desc->app_elf_sha256[1],
             desc->app_elf_sha256[2], desc->app_elf_sha256[3],
             desc->app_elf_sha256[4], desc->app_elf_sha256[5],
             desc->app_elf_sha256[6], desc->app_elf_sha256[7]);
}

// ============================================================
// Seite
// ============================================================

static const char PAGE_HEAD[] PROGMEM =
    "<!doctype html><html lang=\"de\"><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>Bambuddy Display</title><style>"
    "body{background:#15171c;color:#e6e8ee;font:15px/1.5 system-ui,sans-serif;"
    "margin:0;padding:24px 16px;display:flex;justify-content:center}"
    "main{width:100%;max-width:560px}"
    "h1{font-size:20px;margin:0 0 4px}"
    "p.sub{color:#8b91a1;margin:0 0 24px}"
    "h2{font-size:13px;letter-spacing:.08em;text-transform:uppercase;"
    "color:#8b91a1;margin:28px 0 8px}"
    "table{width:100%;border-collapse:collapse}"
    "td{padding:7px 0;border-bottom:1px solid #262a33;vertical-align:top}"
    "td:first-child{color:#8b91a1;width:45%}"
    "td:last-child{text-align:right;font-variant-numeric:tabular-nums}"
    "form{margin-top:10px;display:flex;gap:10px;flex-wrap:wrap}"
    "input[type=file]{flex:1;min-width:200px;color:#8b91a1}"
    "button{background:#2f6fed;color:#fff;border:0;border-radius:8px;"
    "padding:11px 20px;font-size:15px;cursor:pointer}"
    "button:disabled{background:#39404e;cursor:default}"
    "#bar{height:8px;border-radius:4px;background:#262a33;margin-top:16px;"
    "overflow:hidden}"
    "#fill{height:100%;width:0;background:#2f6fed}"
    "#msg{color:#8b91a1;margin-top:10px;min-height:1.5em}"
    ".warn{color:#e0a336}"
    "</style></head><body><main>"
    "<h1>Bambuddy Display</h1>"
    "<p class=\"sub\">Firmware-Update über das Netzwerk</p>"
    "<h2>Laufende Firmware</h2><table>";

static const char PAGE_FORM[] PROGMEM =
    "</table>"
    "<h2>Update</h2>"
    "<form id=\"f\">"
    "<input type=\"file\" id=\"file\" name=\"firmware\" accept=\".bin\" required>"
    "<button id=\"go\" type=\"submit\">Aufspielen</button>"
    "</form>"
    "<div id=\"bar\"><div id=\"fill\"></div></div>"
    "<p id=\"msg\">Datei <code>firmware.bin</code> aus "
    "<code>.pio/build/esp32-4848S040CIY1/</code> wählen.</p>"
    "<p class=\"warn\">Während des Schreibens flackert das Display und der "
    "Drucker wird nicht abgefragt. Nach dem letzten Byte startet das Gerät "
    "von selbst neu.</p>"
    "<script>"
    "var f=document.getElementById('f'),msg=document.getElementById('msg'),"
    "fill=document.getElementById('fill'),go=document.getElementById('go');"
    "f.onsubmit=function(e){e.preventDefault();"
    "var fd=new FormData();fd.append('firmware',document.getElementById('file').files[0]);"
    "var x=new XMLHttpRequest();go.disabled=true;msg.textContent='Übertrage ...';"
    "x.upload.onprogress=function(ev){if(!ev.lengthComputable)return;"
    "var p=Math.round(ev.loaded*100/ev.total);fill.style.width=p+'%';"
    "msg.textContent='Übertrage ... '+p+' %';};"
    "x.onload=function(){msg.textContent=x.responseText;"
    "if(x.status==200){fill.style.width='100%';"
    "msg.textContent=x.responseText+' Seite lädt in 20 Sekunden neu.';"
    "setTimeout(function(){location.reload();},20000);}else{go.disabled=false;}};"
    "x.onerror=function(){msg.textContent='Verbindung abgebrochen.';go.disabled=false;};"
    "x.open('POST','/update');x.send(fd);};"
    "</script></main></body></html>";

static void send_row(const char *label, const char *value)
{
    char row[192];
    snprintf(row, sizeof(row), "<tr><td>%s</td><td>%s</td></tr>", label, value);
    server->sendContent(row);
}

static void handle_root()
{
    const esp_app_desc_t *desc = esp_ota_get_app_description();
    const esp_partition_t *running = esp_ota_get_running_partition();

    // Stueckweise senden: die Seite als String zusammenzubauen kostet auf
    // einen Schlag mehrere Kilobyte Heap — waehrend nebenher die Bildpuffer
    // und der Netzwerk-Task am selben RAM haengen.
    server->setContentLength(CONTENT_LENGTH_UNKNOWN);
    server->send(200, "text/html", "");
    server->sendContent_P(PAGE_HEAD);

    char value[96];

    send_row("Version", build_stamp_version());
    send_row("Gebaut", build_stamp_datetime());

    build_hash_text(desc, value, sizeof(value));
    send_row("Build-Kennung", value);

    const size_t sketch = ESP.getSketchSize();
    const size_t slot = running ? running->size : 0;
    if (slot > 0) {
        snprintf(value, sizeof(value), "%u KB von %u KB (%u %%)",
                 (unsigned)(sketch / 1024), (unsigned)(slot / 1024),
                 (unsigned)(sketch * 100 / slot));
    } else {
        snprintf(value, sizeof(value), "%u KB", (unsigned)(sketch / 1024));
    }
    send_row("Größe", value);

    snprintf(value, sizeof(value), "%s", running ? running->label : "unbekannt");
    send_row("Aktive Partition", value);

    snprintf(value, sizeof(value), "%u KB frei",
             (unsigned)(ESP.getFreeSketchSpace() / 1024));
    send_row("Platz für das Update", value);

    send_row("Gebaut gegen Bambuddy", BB_TESTED_VERSION);

    if (bambuddy_version_known()) {
        send_row("Instanz läuft", bambuddy_version_current());
    }

    uptime_text(value, sizeof(value));
    send_row("Laufzeit", value);

    snprintf(value, sizeof(value), "%s (%d dBm)",
             WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
    send_row("Netzwerk", value);

    snprintf(value, sizeof(value), "%u KB", (unsigned)(ESP.getFreeHeap() / 1024));
    send_row("Freier Speicher", value);

    if (message_buf[0]) send_row("Letzter Versuch", message_buf);

    server->sendContent_P(PAGE_FORM);
    server->sendContent("");
}

// ============================================================
// Upload
// ============================================================

static void handle_upload()
{
    HTTPUpload &up = server->upload();

    switch (up.status) {
    case UPLOAD_FILE_START: {
        upload_expected = (size_t)server->clientContentLength();
        progress_pct = 0;
        set_message("");
        Serial.printf("[OTA] Upload gestartet: %s (%u Bytes angekuendigt)\n",
                      up.filename.c_str(), (unsigned)upload_expected);

        // UPDATE_SIZE_UNKNOWN: Der Browser meldet die Groesse inklusive
        // Multipart-Rahmen, das waere als Ziel zu gross. Update nimmt dann
        // die freie Partition als Obergrenze und meldet selbst, wenn das
        // Abbild nicht hineinpasst.
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
            progress_pct = -1;
            set_message(Update.errorString());
            Serial.printf("[OTA] Start abgelehnt: %s\n", Update.errorString());
        }
        break;
    }

    case UPLOAD_FILE_WRITE:
        if (Update.isRunning()) {
            if (Update.write(up.buf, up.currentSize) != up.currentSize) {
                set_message(Update.errorString());
                Serial.printf("[OTA] Schreibfehler: %s\n", Update.errorString());
                Update.abort();
                progress_pct = -1;
                break;
            }
            if (upload_expected > 0) {
                const size_t done = up.totalSize + up.currentSize;
                int pct = (int)(done * 100 / upload_expected);
                progress_pct = pct > 100 ? 100 : pct;
            }
        }
        break;

    case UPLOAD_FILE_END:
        if (Update.isRunning() && Update.end(true)) {
            progress_pct = 100;
            set_message("Update geschrieben, Neustart folgt");
            Serial.printf("[OTA] %u Bytes geschrieben, Neustart\n",
                          (unsigned)up.totalSize);
        } else if (Update.hasError()) {
            progress_pct = -1;
            set_message(Update.errorString());
            Serial.printf("[OTA] Fehlgeschlagen: %s\n", Update.errorString());
        }
        break;

    case UPLOAD_FILE_ABORTED:
    default:
        if (Update.isRunning()) Update.abort();
        progress_pct = -1;
        set_message("Upload abgebrochen");
        Serial.println("[OTA] Upload abgebrochen");
        break;
    }
}

// Antwort auf den abgeschlossenen POST. Laeuft erst, wenn der Upload-Handler
// mit allen Teilen durch ist — deshalb steht hier das Ergebnis fest.
static void handle_upload_done()
{
    if (Update.hasError() || progress_pct < 100) {
        const char *reason = message_buf[0] ? message_buf : "Update fehlgeschlagen";
        server->send(500, "text/plain; charset=utf-8", reason);
        return;
    }

    server->sendHeader("Connection", "close");
    server->send(200, "text/plain; charset=utf-8",
                 "Update geschrieben. Das Gerät startet neu.");

    // Nicht sofort neu starten: Die Antwort muss erst ueber die Leitung sein,
    // sonst sieht der Browser nur einen Abbruch und niemand weiss, ob es
    // geklappt hat.
    restart_at_ms = millis() + 1500;
}

// ============================================================
// Server an/aus
// ============================================================

static void server_start()
{
    if (server) return;

    server = new WebServer(OTA_PORT);
    if (!server) {
        set_message("Zu wenig Speicher für den Webserver");
        Serial.println("[OTA] Webserver konnte nicht angelegt werden");
        return;
    }

    server->on("/", HTTP_GET, handle_root);
    server->on("/update", HTTP_POST, handle_upload_done, handle_upload);
    server->onNotFound(handle_root);
    server->begin();

    snprintf(address_buf, sizeof(address_buf), "http://%s",
             WiFi.localIP().toString().c_str());
    online = true;
    Serial.printf("[OTA] Webserver laeuft auf %s\n", address_buf);
}

static void server_stop()
{
    online = false;
    address_buf[0] = '\0';
    if (!server) return;

    server->stop();
    delete server;
    server = nullptr;
    Serial.println("[OTA] Webserver beendet");
}

static void ota_task(void *)
{
    for (;;) {
        const bool want = enabled;
        const bool connected = (WiFi.status() == WL_CONNECTED);

        if (want && connected) {
            server_start();
            if (server) server->handleClient();
        } else if (server) {
            // Auch bei WLAN-Verlust abraeumen: der Socket haengt sonst an
            // einer Adresse, die es nicht mehr gibt, und nach dem
            // Wiederverbinden meldet sich der Server nie zurueck.
            server_stop();
        }

        if (restart_at_ms != 0 && (int32_t)(millis() - restart_at_ms) >= 0) {
            Serial.println("[OTA] Neustart nach Update");
            Serial.flush();
            ESP.restart();
        }

        // 5 ms: schnell genug, dass ein Upload nicht ausgebremst wird, und
        // traege genug, dass der Task im Leerlauf nichts kostet.
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

// ============================================================
// Public API
// ============================================================

void ota_service_set_enabled(bool on)
{
    enabled = on;
    if (!on) {
        // Ein laufender Upload wird nicht mitten im Schreiben gekappt — das
        // hinterliesse eine halb beschriebene Partition. Der Task raeumt ab,
        // sobald er wieder an der Reihe ist.
        Serial.println("[OTA] Web-Update ausgeschaltet");
        return;
    }

    Serial.println("[OTA] Web-Update eingeschaltet");
    if (ota_task_handle) return;

    // 8 KB: Der Webserver legt Header und Multipart-Puffer auf dem Stack ab,
    // dazu kommt der TLS-freie, aber nicht sparsame String-Umgang von
    // WebServer. Mit 4 KB laeuft er beim ersten Upload ueber.
    const BaseType_t ok = xTaskCreatePinnedToCore(ota_task, "ota", 8192, nullptr, 1,
                                                  &ota_task_handle, 0);
    if (ok != pdPASS) {
        ota_task_handle = nullptr;
        enabled = false;
        set_message("Zu wenig Speicher für den Update-Dienst");
        Serial.println("[OTA] Task konnte nicht starten");
    }
}

bool ota_service_enabled() { return enabled; }
bool ota_service_online() { return online; }
const char *ota_service_address() { return address_buf; }
int ota_service_progress() { return progress_pct; }
const char *ota_service_message() { return message_buf; }
