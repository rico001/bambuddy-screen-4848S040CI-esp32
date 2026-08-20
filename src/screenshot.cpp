#include "screenshot.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <lvgl.h>
#include <core/lv_refr.h>
#include <string.h>

// Der private Header wird gebraucht, weil LVGL 9.2 zwar
// lv_display_set_flush_cb() anbietet, aber keinen Weg, die bestehende
// Funktion zu erfragen. Ohne sie liesse sie sich nicht weiterrufen — und das
// Panel bekaeme waehrend der Aufnahme keine Pixel mehr. Die LVGL-Fassung ist
// in platformio.ini auf 9.2.2 festgenagelt; bei einem Sprung darauf achten.
#include "display/lv_display_private.h"

#include "ui_layout.h"

static constexpr uint16_t SHOT_W = SCREEN_W;
static constexpr uint16_t SHOT_H = SCREEN_H;
static constexpr size_t SHOT_SIZE = (size_t)SHOT_W * SHOT_H * 2; // RGB565

static uint8_t *buffer = nullptr;
static lv_display_flush_cb_t original_flush = nullptr;
static bool capturing = false;
static bool complete = false;

static void stop_capture()
{
    if (!capturing) return;

    lv_display_t *disp = lv_display_get_default();
    if (disp && original_flush) disp->flush_cb = original_flush;

    original_flush = nullptr;
    capturing = false;
}

// Haengt zwischen LVGL und dem Panel: Erst die Kachel in den Puffer kopieren,
// dann unveraendert weiterreichen. Das Bild auf dem Panel bleibt davon
// unberuehrt — es wird nur zusaetzlich mitgeschrieben.
static void capture_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    if (buffer && area) {
        const int32_t x1 = area->x1 < 0 ? 0 : area->x1;
        const int32_t y1 = area->y1 < 0 ? 0 : area->y1;
        const int32_t x2 = area->x2 >= SHOT_W ? SHOT_W - 1 : area->x2;
        const int32_t y2 = area->y2 >= SHOT_H ? SHOT_H - 1 : area->y2;

        const int32_t src_w = lv_area_get_width(area);
        const int32_t copy_w = x2 - x1 + 1;

        if (copy_w > 0) {
            for (int32_t y = y1; y <= y2; y++) {
                const uint8_t *src = px_map + ((y - area->y1) * src_w +
                                               (x1 - area->x1)) * 2;
                uint8_t *dst = buffer + ((size_t)y * SHOT_W + x1) * 2;
                memcpy(dst, src, (size_t)copy_w * 2);
            }
        }
    }

    if (original_flush) {
        original_flush(disp, area, px_map);
    } else {
        lv_display_flush_ready(disp);
    }
}

bool screenshot_request()
{
    if (capturing || complete) return false;

    lv_display_t *disp = lv_display_get_default();
    if (!disp || !disp->flush_cb) return false;

    if (!buffer) {
        // Ausdruecklich ins PSRAM: 450 KB aus dem internen RAM waeren das
        // Ende jeder Netzwerkverbindung.
        buffer = (uint8_t *)heap_caps_malloc(SHOT_SIZE, MALLOC_CAP_SPIRAM);
        if (!buffer) {
            // Mit Zahlen statt bloss "geht nicht": Ist genug frei, aber der
            // groesste Block zu klein, ist der Speicher zerstueckelt — dann
            // hilft nur ein Neustart, kein weiterer Versuch.
            Serial.printf("[Screenshot] Kein Block von %u KB im PSRAM "
                          "(frei %u KB, groesster Block %u KB)\n",
                          (unsigned)(SHOT_SIZE / 1024),
                          (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                          (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) /
                                     1024));
            return false;
        }
        Serial.printf("[Screenshot] %u KB im PSRAM belegt\n",
                      (unsigned)(SHOT_SIZE / 1024));
    }

    memset(buffer, 0, SHOT_SIZE);

    original_flush = disp->flush_cb;
    disp->flush_cb = capture_flush;
    capturing = true;

    // Alles fuer ungueltig erklaeren und den Neuaufbau sofort erzwingen.
    //
    // lv_refr_now() zeichnet und flusht in diesem Aufruf, nicht irgendwann
    // im naechsten Durchlauf. Danach steht das Bild vollstaendig im Puffer —
    // ohne Buchfuehrung darueber, welche Kacheln schon da waren, und ohne
    // Zeitlimit fuer den Fall, dass eine ausbleibt.
    //
    // Genau daran scheiterte die erste Fassung: Sie wartete darauf, dass jede
    // Zeile einmal in voller Breite vorbeikommt. Auf Screens mit einem
    // Ladekreis oder dem Zeichenregen liefert LVGL aber staendig schmale
    // Ausschnitte, und die Bedingung ging nie auf.
    lv_obj_invalidate(lv_screen_active());
    lv_obj_invalidate(lv_layer_top());
    lv_obj_invalidate(lv_layer_sys());
    lv_refr_now(disp);

    stop_capture();
    complete = true;

    Serial.println("[Screenshot] Aufnahme fertig");
    return true;
}

bool screenshot_ready()
{
    return complete && buffer != nullptr;
}

const uint8_t *screenshot_data() { return buffer; }
size_t screenshot_size() { return SHOT_SIZE; }
uint16_t screenshot_width() { return SHOT_W; }
uint16_t screenshot_height() { return SHOT_H; }

void screenshot_release()
{
    stop_capture();
    complete = false;

    // Der Puffer bleibt absichtlich liegen.
    //
    // Im PSRAM kommen und gehen Modellbilder, Kamerabilder und Vorschauen.
    // Wer 450 KB freigibt und spaeter neu anfordert, findet nach einer Weile
    // keinen zusammenhaengenden Block dieser Groesse mehr — das Foto gelang
    // dann nur direkt nach einem Neustart. Einmal belegt und behalten kostet
    // dagegen nichts ausser Platz, den sonst niemand am Stueck braucht.
}
