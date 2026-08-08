#include "bambuddy_cover.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <pngle.h>

#include "bambuddy_config.h"
#include "bambuddy_http.h"
#include "settings_screen.h"

static constexpr size_t FRAME_BYTES = (size_t)COVER_SIZE * COVER_SIZE * 2;
static constexpr uint32_t RETRY_AFTER_ERROR_MS = 30000;
static constexpr size_t FEED_CHUNK = 1024;

// Zwei Puffer: der Netzwerk-Task dekodiert in den hinteren, die UI zeigt den
// vorderen. Ohne diese Trennung wuerde LVGL mitten im Dekodieren rendern.
static uint16_t *frame_a = nullptr;
static uint16_t *frame_b = nullptr;
static uint16_t *decode_target = nullptr;
static uint16_t *ready_frame = nullptr;
static volatile bool frame_fresh = false;
static bool have_frame = false;

static char loaded_job[64] = "";
static uint32_t last_error_ms = 0;

// Grosse Fassung fuers Vollbild — einfacher Puffer, weil sie erst angezeigt
// wird, wenn sie fertig dekodiert ist.
static uint16_t *big_frame = nullptr;
static volatile bool big_fresh = false;
static bool big_have = false;
static bool big_wanted = false;
static int32_t big_archive_id = 0; // 0 = Cover des laufenden Auftrags
static char big_key[80] = "";      // was gerade gross geladen ist

// Ziel des laufenden Dekodiervorgangs (klein oder gross)
static uint16_t target_size = COVER_SIZE;

// Quellgroesse aus dem PNG-Header — zum Herunterrechnen gebraucht
static uint32_t src_w = 0;
static uint32_t src_h = 0;

// ============================================================
// Speicher
// ============================================================

static bool ensure_buffers()
{
    if (frame_a && frame_b) return true;

    if (!frame_a) frame_a = (uint16_t *)ps_malloc(FRAME_BYTES);
    if (!frame_b) frame_b = (uint16_t *)ps_malloc(FRAME_BYTES);

    if (!frame_a || !frame_b) {
        Serial.println("[Cover] Kein Speicher fuer die Bildpuffer");
        return false;
    }
    return true;
}

// Zielpuffer mit der Hintergrundfarbe fuellen — Bildbereiche, die das PNG
// nicht abdeckt, sollen nicht als Rauschen stehenbleiben.
static void clear_target()
{
    const uint8_t r = (COVER_BG >> 16) & 0xFF;
    const uint8_t g = (COVER_BG >> 8) & 0xFF;
    const uint8_t b = COVER_BG & 0xFF;
    const uint16_t fill = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);

    const size_t pixels = (size_t)target_size * target_size;
    for (size_t i = 0; i < pixels; i++) decode_target[i] = fill;
}

// ============================================================
// PNG-Dekodierung
// ============================================================

static void on_png_init(pngle_t *, uint32_t w, uint32_t h)
{
    src_w = w;
    src_h = h;
}

// pngle ruft das pro Pixel (oder kleinem Rechteck) auf. Wir rechnen die
// Quellkoordinate direkt auf das Zielraster um — damit ist die Groesse des
// Originals egal, und es wird nie mehr Speicher gebraucht als das Ziel.
static void on_png_draw(pngle_t *, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                        const uint8_t rgba[4])
{
    if (!decode_target || src_w == 0 || src_h == 0) return;

    const uint8_t alpha = rgba[3];
    if (alpha == 0) return; // voll transparent: Hintergrund stehen lassen

    // Transparenz gegen die Kachelfarbe verrechnen
    const uint8_t bg_r = (COVER_BG >> 16) & 0xFF;
    const uint8_t bg_g = (COVER_BG >> 8) & 0xFF;
    const uint8_t bg_b = COVER_BG & 0xFF;

    const uint8_t r = (rgba[0] * alpha + bg_r * (255 - alpha)) / 255;
    const uint8_t g = (rgba[1] * alpha + bg_g * (255 - alpha)) / 255;
    const uint8_t b = (rgba[2] * alpha + bg_b * (255 - alpha)) / 255;
    const uint16_t color = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);

    for (uint32_t py = y; py < y + h; py++) {
        const uint32_t dy = py * target_size / src_h;
        if (dy >= target_size) continue;

        for (uint32_t px = x; px < x + w; px++) {
            const uint32_t dx = px * target_size / src_w;
            if (dx >= target_size) continue;
            decode_target[dy * target_size + dx] = color;
        }
    }
}

// ============================================================
// Abrufen
// ============================================================

// Holt ein PNG und dekodiert es in decode_target (Groesse: target_size).
// Beide Quellen — Cover des laufenden Auftrags und Archiv-Vorschau —
// brauchen den Kamera-Token, nicht den API-Key.
static bool fetch_png(const char *url)
{
    BambuddyHttp session;
    if (!session.begin(url)) return false;

    HTTPClient &http = session.http();

    const int code = http.GET();
    if (code != 200) {
        session.end();
        // 404 heisst schlicht: gerade kein Auftrag mit Vorschaubild
        if (code != 404) Serial.printf("[Cover] HTTP %d (%s)\n", code, url);
        return false;
    }

    pngle_t *pngle = pngle_new();
    if (!pngle) {
        session.end();
        Serial.println("[Cover] PNG-Decoder konnte nicht angelegt werden");
        return false;
    }
    pngle_set_init_callback(pngle, on_png_init);
    pngle_set_draw_callback(pngle, on_png_draw);

    src_w = src_h = 0;
    clear_target();

    WiFiClient *stream = http.getStreamPtr();
    uint8_t chunk[FEED_CHUNK];
    const int len = http.getSize();
    int remaining = len;
    bool ok = true;
    const uint32_t deadline = millis() + 15000;

    // Haeppchenweise durch den Decoder schieben: so liegt nie das ganze PNG
    // im Speicher, nur ein Kilobyte davon.
    while (millis() < deadline && (remaining > 0 || len < 0)) {
        const int avail = stream->available();
        if (avail <= 0) {
            if (!http.connected() && stream->available() == 0) break;
            delay(5);
            continue;
        }

        const int want = avail > (int)sizeof(chunk) ? (int)sizeof(chunk) : avail;
        const int got = stream->readBytes(chunk, want);
        if (got <= 0) break;
        if (remaining > 0) remaining -= got;

        int offset = 0;
        while (offset < got) {
            const int fed = pngle_feed(pngle, chunk + offset, got - offset);
            if (fed < 0) {
                Serial.printf("[Cover] PNG-Fehler: %s\n", pngle_error(pngle));
                ok = false;
                break;
            }
            if (fed == 0) break;
            offset += fed;
        }
        if (!ok) break;

        // Nach jedem Kilobyte kurz Luft lassen. Das Panel liest seinen
        // Framebuffer dauernd aus dem PSRAM; dekodieren wir am Stueck
        // hinein, fehlt ihm die Bandbreite und das Bild zuckt sichtbar.
        // Das Cover laedt einmal pro Auftrag — die paar Millisekunden
        // Mehraufwand merkt niemand.
        vTaskDelay(1);
    }

    pngle_destroy(pngle);
    session.end();

    return ok && src_w > 0;
}

// ============================================================
// Public API
// ============================================================

void bambuddy_cover_update(const char *job_name)
{
    if (!job_name || job_name[0] == '\0') return;
    if (bambuddy_cam_token()[0] == '\0') return;

    // Das Bild aendert sich nur mit dem Auftrag — also nicht im Poll-Takt
    // nachladen, sondern genau dann, wenn ein anderer Job laeuft.
    if (have_frame && strcmp(loaded_job, job_name) == 0) return;

    // Nach einem Fehlschlag nicht sofort wieder anklopfen
    if (last_error_ms && (millis() - last_error_ms) < RETRY_AFTER_ERROR_MS) return;

    if (!ensure_buffers()) return;

    // Immer in den Puffer schreiben, den die UI gerade nicht anzeigt
    decode_target = (ready_frame == frame_a) ? frame_b : frame_a;

    const char *url = bambuddy_url("/printers/%d/cover?token=%s",
                                   bambuddy_printer_id(), bambuddy_cam_token());
    if (!fetch_png(url)) {
        last_error_ms = millis();
        return;
    }

    last_error_ms = 0;
    strncpy(loaded_job, job_name, sizeof(loaded_job) - 1);
    loaded_job[sizeof(loaded_job) - 1] = '\0';

    ready_frame = decode_target;
    have_frame = true;
    frame_fresh = true;

    Serial.printf("[Cover] Bild geladen fuer '%s'\n", loaded_job);
}

// Grosse Fassung nur holen, wenn das Vollbild wirklich offen ist — 320 KB
// Puffer und ein zweiter Abruf lohnen sich nicht auf Vorrat.
void bambuddy_cover_update_big(const char *job_name)
{
    if (!big_wanted) return;

    // Was soll gross angezeigt werden: ein Archiv-Eintrag aus der
    // Warteschlange oder das Cover des laufenden Auftrags?
    char key[sizeof(big_key)];
    char url[224];

    if (big_archive_id != 0) {
        snprintf(key, sizeof(key), "archiv:%d", (int)big_archive_id);
        snprintf(url, sizeof(url), "%s",
                 bambuddy_url("/archives/%d/thumbnail?token=%s",
                              (int)big_archive_id, bambuddy_cam_token()));
    } else {
        if (!job_name || job_name[0] == '\0') return;
        snprintf(key, sizeof(key), "auftrag:%s", job_name);
        snprintf(url, sizeof(url), "%s",
                 bambuddy_url("/printers/%d/cover?token=%s",
                              bambuddy_printer_id(), bambuddy_cam_token()));
    }

    if (big_have && strcmp(big_key, key) == 0) return;

    // Ohne Bremse liefe ein Fehlschlag im Takt der Task-Schleife weiter —
    // also mehrmals pro Sekunde gegen denselben Server.
    static uint32_t big_error_ms = 0;
    if (big_error_ms && (millis() - big_error_ms) < RETRY_AFTER_ERROR_MS) return;

    if (!big_frame) {
        big_frame = (uint16_t *)ps_malloc((size_t)COVER_BIG_SIZE * COVER_BIG_SIZE * 2);
        if (!big_frame) {
            Serial.println("[Cover] Kein Speicher fuer das grosse Bild");
            return;
        }
    }

    decode_target = big_frame;
    target_size = COVER_BIG_SIZE;
    const bool ok = fetch_png(url);
    target_size = COVER_SIZE;

    if (!ok) {
        big_error_ms = millis();
        return;
    }
    big_error_ms = 0;

    strncpy(big_key, key, sizeof(big_key) - 1);
    big_key[sizeof(big_key) - 1] = '\0';
    big_have = true;
    big_fresh = true;

    Serial.printf("[Cover] Grossbild geladen: %s\n", big_key);
}

void bambuddy_cover_request_archive(int32_t archive_id)
{
    if (big_archive_id != archive_id) {
        big_archive_id = archive_id;
        big_have = false; // anderes Motiv: neu laden
        big_fresh = false;
    }
    big_wanted = true;
}

bool bambuddy_cover_take_frame(void **buf)
{
    if (!frame_fresh || !ready_frame) return false;

    frame_fresh = false;
    if (buf) *buf = ready_frame;
    return true;
}

bool bambuddy_cover_has_frame()
{
    return have_frame;
}

void bambuddy_cover_set_big_wanted(bool wanted)
{
    big_wanted = wanted;
    if (!wanted) {
        // Zurueck auf den laufenden Auftrag, damit das naechste Oeffnen
        // nicht die alte Archiv-Vorschau zeigt.
        if (big_archive_id != 0) {
            big_archive_id = 0;
            big_have = false;
        }
    }
}

bool bambuddy_cover_take_big_frame(void **buf)
{
    if (!big_fresh || !big_frame) return false;

    big_fresh = false;
    if (buf) *buf = big_frame;
    return true;
}

bool bambuddy_cover_has_big_frame()
{
    return big_have;
}

void bambuddy_cover_reset()
{
    have_frame = false;
    frame_fresh = false;
    ready_frame = nullptr;
    loaded_job[0] = '\0';
    last_error_ms = 0;

    big_have = false;
    big_fresh = false;
    big_key[0] = '\0';
}
