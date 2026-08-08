#include "bambuddy_camera.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <TJpg_Decoder.h>

#include "bambuddy_config.h"
#include "bambuddy_http.h"
#include "settings_screen.h"

static constexpr uint32_t SNAPSHOT_INTERVAL_MS = 3000;
static constexpr size_t MAX_JPEG_BYTES = 96 * 1024;
static constexpr size_t FRAME_BYTES = (size_t)CAM_W * CAM_H * 2;

// Zwei Puffer: der Netzwerk-Task dekodiert in den hinteren, die UI zeigt den
// vorderen. Ohne diese Trennung wuerde LVGL mitten im Dekodieren rendern.
static uint16_t *frame_a = nullptr;
static uint16_t *frame_b = nullptr;
static uint16_t *decode_target = nullptr;
static uint16_t *ready_frame = nullptr;
static volatile bool frame_fresh = false;
static bool have_frame = false;

static uint8_t *jpeg_buf = nullptr;
static uint32_t last_fetch_ms = 0;

// Groesse des dekodierten Bildes (Quelle geteilt durch den TJpgDec-Faktor)
static uint32_t dec_w = 0;
static uint32_t dec_h = 0;
static volatile bool active = false;
static char last_error[64] = "";

// ============================================================
// Speicher
// ============================================================

static bool ensure_buffers()
{
    if (frame_a && frame_b && jpeg_buf) return true;

    // Ins PSRAM: der interne RAM wird von LVGL und dem TLS-Stack gebraucht.
    if (!frame_a) frame_a = (uint16_t *)ps_malloc(FRAME_BYTES);
    if (!frame_b) frame_b = (uint16_t *)ps_malloc(FRAME_BYTES);
    if (!jpeg_buf) jpeg_buf = (uint8_t *)ps_malloc(MAX_JPEG_BYTES);

    if (!frame_a || !frame_b || !jpeg_buf) {
        Serial.println("[Kamera] Kein Speicher fuer die Bildpuffer");
        strncpy(last_error, "Kein Speicher fuer das Bild", sizeof(last_error) - 1);
        return false;
    }

    memset(frame_a, 0, FRAME_BYTES);
    memset(frame_b, 0, FRAME_BYTES);
    return true;
}

// ============================================================
// Dekodieren
// ============================================================

// TJpgDec liefert das Bild blockweise. Wir schneiden ab, falls die Kamera ein
// anderes Format liefert als erwartet — sonst schreibt der Decoder ueber den
// Puffer hinaus.
static uint16_t block_counter = 0;

static bool jpeg_block_cb(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap)
{
    if (!decode_target) return false;

    // Regelmaessig abgeben: das Panel liest seinen Framebuffer dauerhaft aus
    // dem PSRAM, und ein Decoder, der ohne Pause hineinschreibt, nimmt ihm
    // die Bandbreite — sichtbar als kurzes Zucken im Bild.
    if ((++block_counter & 0x0F) == 0) vTaskDelay(1);

    if (dec_w == 0 || dec_h == 0) return true;

    // Quellraster auf das Zielraster abbilden. Damit ist die Groesse des
    // Originals egal — und LVGL bekommt ein Bild, das es ohne Skalierung
    // zeichnen kann.
    for (uint16_t row = 0; row < h; row++) {
        const uint32_t dy = (uint32_t)(y + row) * CAM_H / dec_h;
        if (dy >= CAM_H) continue;

        uint16_t *dst_row = &decode_target[dy * CAM_W];
        const uint16_t *src_row = &bitmap[row * w];

        for (uint16_t col = 0; col < w; col++) {
            const uint32_t dx = (uint32_t)(x + col) * CAM_W / dec_w;
            if (dx >= CAM_W) continue;
            dst_row[dx] = src_row[col];
        }
    }
    return true;
}

// ============================================================
// Abrufen
// ============================================================

static int fetch_snapshot()
{
    // Der Snapshot braucht den Kamera-Token, nicht den API-Key.
    const char *url = bambuddy_url("/printers/%d/camera/snapshot?token=%s",
                                   bambuddy_printer_id(), bambuddy_cam_token());
    // Gemeinsame, offen gehaltene Verbindung: lwip hat nur 16 TCP-Plaetze,
    // und jeder geschlossene bleibt eine Minute belegt. Ein eigener Aufbau
    // alle drei Sekunden wuerde den Vorrat aufbrauchen.
    BambuddyHttp &cam_session = bambuddy_http_shared();
    if (!cam_session.begin(url, true)) return -1;

    HTTPClient &http = cam_session.http();

    const int code = http.GET();
    if (code != 200) {
        cam_session.end(false);
        return code;
    }

    const int len = http.getSize();
    if (len > 0 && (size_t)len > MAX_JPEG_BYTES) {
        cam_session.end(false);
        return -2;
    }

    WiFiClient *stream = http.getStreamPtr();
    size_t read_total = 0;
    const uint32_t deadline = millis() + 8000;

    while (millis() < deadline && read_total < MAX_JPEG_BYTES) {
        if (len > 0 && read_total >= (size_t)len) break;

        const size_t avail = stream->available();
        if (avail == 0) {
            if (len <= 0 && !http.connected()) break; // chunked und fertig
            delay(5);
            continue;
        }

        const size_t chunk = min(avail, MAX_JPEG_BYTES - read_total);
        const int got = stream->readBytes(jpeg_buf + read_total, chunk);
        if (got <= 0) break;
        read_total += got;
    }
    // Verbindung offen lassen — der naechste Snapshot kommt in drei Sekunden
    cam_session.end(false);

    return read_total > 0 ? (int)read_total : -3;
}

// ============================================================
// Public API
// ============================================================

void bambuddy_camera_set_active(bool value)
{
    active = value;
    if (value) {
        last_fetch_ms = 0; // beim Oeffnen sofort holen
        last_error[0] = '\0';
    } else {
        // Verbindung bleibt offen — sie gehoert jetzt allen Abrufen
        // gemeinsam und wird gleich vom naechsten gebraucht.
    }
}

bool bambuddy_camera_active()
{
    return active;
}

void bambuddy_camera_update()
{
    if (!active) return;

    const uint32_t now = millis();
    if (last_fetch_ms && (now - last_fetch_ms) < SNAPSHOT_INTERVAL_MS) return;
    if (bambuddy_cam_token()[0] == '\0') {
        strncpy(last_error, "Kein Kamera-Token hinterlegt", sizeof(last_error) - 1);
        return;
    }
    if (!ensure_buffers()) return;

    last_fetch_ms = now;

    const int size = fetch_snapshot();
    if (size <= 0 || size == 401 || size == 403) {
        snprintf(last_error, sizeof(last_error), "Kamera nicht erreichbar (%d)", size);
        Serial.printf("[Kamera] Snapshot fehlgeschlagen (%d)\n", size);
        return;
    }

    // Immer in den Puffer schreiben, den die UI gerade nicht anzeigt
    decode_target = (ready_frame == frame_a) ? frame_b : frame_a;

    // Originalgroesse lesen, damit das Ausduennen unabhaengig von der
    // Kameraaufloesung stimmt.
    uint16_t src_w = 0, src_h = 0;
    if (TJpgDec.getJpgSize(&src_w, &src_h, jpeg_buf, size) != JDR_OK || src_w == 0) {
        strncpy(last_error, "Bild nicht lesbar", sizeof(last_error) - 1);
        return;
    }
    dec_w = src_w / 2;
    dec_h = src_h / 2;

    block_counter = 0;
    TJpgDec.setJpgScale(2);      // 1280x720 -> 640x360, danach auf 480x270
    TJpgDec.setSwapBytes(false); // LVGL erwartet RGB565 in nativer Byte-Reihenfolge
    TJpgDec.setCallback(jpeg_block_cb);

    if (TJpgDec.drawJpg(0, 0, jpeg_buf, size) != JDR_OK) {
        strncpy(last_error, "Bild nicht dekodierbar", sizeof(last_error) - 1);
        Serial.println("[Kamera] JPEG nicht dekodierbar");
        return;
    }

    last_error[0] = '\0';
    ready_frame = decode_target;
    have_frame = true;
    frame_fresh = true;
}

bool bambuddy_camera_take_frame(void **buf)
{
    if (!frame_fresh || !ready_frame) return false;

    frame_fresh = false;
    if (buf) *buf = ready_frame;
    return true;
}

bool bambuddy_camera_has_frame()
{
    return have_frame;
}

const char *bambuddy_camera_error()
{
    return last_error;
}
