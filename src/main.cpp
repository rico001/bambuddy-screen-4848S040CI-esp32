#include <Arduino.h>
#include <esp32_smartdisplay.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_task_wdt.h>

#include "ams_screen.h"
#include "bambuddy_api.h"
#include "archive_screen.h"
#include "bambuddy_config.h"
#include "general_screen.h"
#include "queue_screen.h"
#include "settings_screen.h"
#include "status_bar.h"
#include "status_screen.h"
#include "ui_layout.h"
#include "ui_watch.h"
#include "wifi_screen.h"

// --- Globals ---
static lv_obj_t *tileview;
static lv_obj_t *ams_tile;
static lv_obj_t *queue_tile;
static lv_obj_t *archive_tile;
static lv_obj_t *general_tile;

// Kachelwechsel: nur der sichtbare Screen soll Daten holen
static void tile_changed_cb(lv_event_t *)
{
    const bool ams_visible = lv_tileview_get_tile_active(tileview) == ams_tile;
    const bool queue_visible = lv_tileview_get_tile_active(tileview) == queue_tile;
    const bool archive_visible = lv_tileview_get_tile_active(tileview) == archive_tile;
    const bool general_visible = lv_tileview_get_tile_active(tileview) == general_tile;
    ams_screen_set_visible(ams_visible);
    queue_screen_set_visible(queue_visible);
    archive_screen_set_visible(archive_visible);
    general_screen_set_visible(general_visible);
}

// Beim Start sagen, warum zuletzt neu gestartet wurde. Ohne diese Zeile
// bleibt nach einem unbeobachteten Reset nur Raten: Absturz, Watchdog und
// Spannungseinbruch sehen im Nachhinein identisch aus.
static void log_reset_reason()
{
    const char *text;
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  text = "Einschalten"; break;
    case ESP_RST_SW:       text = "Neustart durch Software"; break;
    case ESP_RST_PANIC:    text = "ABSTURZ (Exception/Panic)"; break;
    case ESP_RST_TASK_WDT: text = "Task-Watchdog: ein Task hat blockiert"; break;
    case ESP_RST_INT_WDT:  text = "Interrupt-Watchdog"; break;
    case ESP_RST_WDT:      text = "sonstiger Watchdog"; break;
    case ESP_RST_BROWNOUT: text = "SPANNUNGSEINBRUCH (Netzteil/USB-Kabel)"; break;
    case ESP_RST_DEEPSLEEP:text = "Aufwachen aus Deep Sleep"; break;
    case ESP_RST_EXT:      text = "Reset-Pin"; break;
    default:               text = "unbekannt"; break;
    }
    Serial.printf("\n[Start] Letzter Neustart: %s\n", text);
}

// --- Setup & Loop ---
auto lv_last_tick = millis();

// Nach dem Aufbau einmal zeigen, wie es um den Speicher steht: der LVGL-Pool
// und der interne RAM sind die beiden Groessen, an denen dieses Board haengt.
static void log_memory(const char *stage)
{
    // LVGL benutzt jetzt den System-Heap, hat also keinen eigenen Pool mehr,
    // ueber den sich berichten liesse. Entscheidend ist ohnehin der interne
    // RAM: an ihm haengen Sockets, Task-Stacks und die Zeichenpuffer.
    Serial.printf("[Speicher] %s — intern frei %u, groesster Block %u, "
                  "PSRAM frei %u\n",
                  stage,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

void setup()
{
    Serial.begin(115200);
    delay(200); // kurz warten, sonst verschluckt der Monitor die erste Zeile
    log_reset_reason();

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
    lv_obj_t *tile4 = lv_tileview_add_tile(tileview, 3, 0, (lv_dir_t)(LV_DIR_LEFT | LV_DIR_RIGHT));
    lv_obj_t *tile5 = lv_tileview_add_tile(tileview, 4, 0, (lv_dir_t)LV_DIR_LEFT);

    // AMS liegt links vom Druckerstatus. Der Status bleibt die Startkachel,
    // rechts folgen Warteschlange, Archiv und System.
    ams_screen_create(tile1);
    status_screen_create(tile2);
    queue_screen_create(tile3);
    archive_screen_create(tile4);
    general_screen_create(tile5);
    lv_tileview_set_tile(tileview, tile2, LV_ANIM_OFF);

    // Daten-Screens und die dynamische Systemansicht bekommen ihre
    // Sichtbarkeit ueber den Tilewechsel gemeldet.
    ams_tile = tile1;
    queue_tile = tile3;
    archive_tile = tile4;
    general_tile = tile5;
    lv_obj_add_event_cb(tileview, tile_changed_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    log_memory("Screens gebaut");

    wifi_service_start();
    bambuddy_api_start();

    log_memory("Netzwerk-Task gestartet");

    // Den UI-Thread ueberwachen. Bleibt lv_timer_handler laenger als zehn
    // Sekunden haengen, erzwingt der Watchdog einen Panic mit Backtrace —
    // damit wird aus einem stummen Freeze eine Zeilennummer. Zehn Sekunden
    // sind reichlich: ein voller Bildaufbau dauert Millisekunden.
    esp_task_wdt_init(10, true);
    esp_task_wdt_add(nullptr);
}

void loop()
{
    auto const now = millis();
    lv_tick_inc(now - lv_last_tick);
    lv_last_tick = now;
    ui_watch_alive_ms = now;
    ui_watch("lvgl");
    lv_timer_handler();
    ui_watch("loop");
    esp_task_wdt_reset();

    // Gibt dem WiFi/TCP-Task auf Core 0 CPU-Zeit — ohne yield kann der
    // WiFi-Stack Pakete verpassen, wenn LVGL-Rendering Core 1 blockiert.
    delay(1);
}
