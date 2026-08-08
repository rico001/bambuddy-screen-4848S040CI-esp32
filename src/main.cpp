#include <Arduino.h>
#include <esp32_smartdisplay.h>
#include <esp_heap_caps.h>

#include "bambuddy_api.h"
#include "bambuddy_config.h"
#include "queue_screen.h"
#include "settings_screen.h"
#include "status_bar.h"
#include "status_screen.h"
#include "ui_layout.h"
#include "wifi_screen.h"

// --- Globals ---
static lv_obj_t *tileview;
static lv_obj_t *queue_tile;

// Kachelwechsel: nur der sichtbare Screen soll Daten holen
static void tile_changed_cb(lv_event_t *)
{
    queue_screen_set_visible(lv_tileview_get_tile_active(tileview) == queue_tile);
}

// --- Setup & Loop ---
auto lv_last_tick = millis();

// Nach dem Aufbau einmal zeigen, wie es um den Speicher steht: der LVGL-Pool
// und der interne RAM sind die beiden Groessen, an denen dieses Board haengt.
static void log_memory(const char *stage)
{
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    Serial.printf("[Speicher] %s — LVGL benutzt %u von %u Bytes (%u%%), "
                  "intern frei %u, groesster Block %u\n",
                  stage,
                  (unsigned)(mon.total_size - mon.free_size), (unsigned)mon.total_size,
                  (unsigned)mon.used_pct,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

void setup()
{
    Serial.begin(115200);
    smartdisplay_init();

    // Theme vor dem Bau der Screens setzen, sonst blitzt kurz das falsche auf
    settings_apply_saved();
    bambuddy_config_load();

    // Statusleiste liegt fest oben, die Screens darunter
    status_bar_create(lv_screen_active());

    // Tileview for swipeable screens (horizontal)
    tileview = lv_tileview_create(lv_screen_active());
    lv_obj_set_size(tileview, SCREEN_W, CONTENT_H);
    lv_obj_align(tileview, LV_ALIGN_TOP_MID, 0, STATUS_BAR_H);
    lv_obj_set_style_border_width(tileview, 0, 0);
    lv_obj_set_style_radius(tileview, 0, 0);

    lv_obj_t *tile1 = lv_tileview_add_tile(tileview, 0, 0, (lv_dir_t)LV_DIR_RIGHT);
    lv_obj_t *tile2 = lv_tileview_add_tile(tileview, 1, 0, (lv_dir_t)(LV_DIR_LEFT | LV_DIR_RIGHT));
    lv_obj_t *tile3 = lv_tileview_add_tile(tileview, 2, 0, (lv_dir_t)(LV_DIR_LEFT | LV_DIR_RIGHT));
    lv_obj_t *tile4 = lv_tileview_add_tile(tileview, 3, 0, (lv_dir_t)LV_DIR_LEFT);

    // Startkachel ist der Druckerstatus — das ist der Grund, warum das
    // Geraet an der Wand haengt. Danach die Warteschlange, dann Technik.
    status_screen_create(tile1);
    queue_screen_create(tile2);
    wifi_screen_create(tile3);
    settings_screen_create(tile4);

    // Die Warteschlange fragt nur ab, wenn ihre Kachel sichtbar ist
    queue_tile = tile2;
    lv_obj_add_event_cb(tileview, tile_changed_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    log_memory("Screens gebaut");

    bambuddy_api_start();

    log_memory("Netzwerk-Task gestartet");
}

void loop()
{
    auto const now = millis();
    lv_tick_inc(now - lv_last_tick);
    lv_last_tick = now;
    lv_timer_handler();

    // Gibt dem WiFi/TCP-Task auf Core 0 CPU-Zeit — ohne yield kann der
    // WiFi-Stack Pakete verpassen, wenn LVGL-Rendering Core 1 blockiert.
    delay(1);
}

