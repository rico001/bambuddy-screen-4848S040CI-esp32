#include "archive_screen.h"

#include <Arduino.h>
#include <string.h>

#include "bambuddy_api.h"
#include "bambuddy_archive.h"
#include "bambuddy_cover.h"
#include "ui_layout.h"
#include "ui_dialog.h"
#include "ui_image_view.h"
#include "ui_theme.h"
#include "ui_util.h"
#include "ui_watch.h"

static constexpr int PAD = 12;
static constexpr int CONTENT_W = SCREEN_W - 2 * PAD;
static constexpr int HEADER_H = 46;
static constexpr int FOOTER_H = 52;
static constexpr int ROW_H = 76;
static constexpr int PLAY_SIZE = 52;
static constexpr int DELETE_SIZE = 44;

static constexpr uint32_t COL_DANGER = 0xF44336;

static lv_obj_t *title_lbl;
static lv_obj_t *list_cont;
static lv_obj_t *empty_lbl;
static lv_obj_t *message_lbl;
static lv_obj_t *page_lbl;
static lv_obj_t *prev_btn;
static lv_obj_t *next_btn;

static bambuddy_archive_item_t shown[BB_ARCHIVE_PAGE_SIZE];
static int shown_count = 0;

// Vollbild-Vorschau des Modells
static ui_image_view_t preview_view;

static constexpr uint32_t START_LOCK_MS = 25000;
static int32_t starting_id = 0;
static uint32_t starting_ms = 0;


static const char *status_text(const char *status)
{
    if (strcasecmp(status, "success") == 0 || strcasecmp(status, "finished") == 0) return "fertig";
    if (strcasecmp(status, "failed") == 0) return "fehlgeschlagen";
    if (strcasecmp(status, "cancelled") == 0) return "abgebrochen";
    if (strcasecmp(status, "running") == 0) return "laeuft";
    return status && status[0] ? status : "unbekannt";
}

static bool is_starting(int32_t archive_id)
{
    return starting_id != 0 && archive_id == starting_id &&
           (millis() - starting_ms) < START_LOCK_MS;
}

static void rebuild_list();

static void set_enabled(lv_obj_t *btn, bool enabled)
{
    if (enabled) {
        lv_obj_remove_state(btn, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(btn, LV_STATE_DISABLED);
    }
}

static void update_pager()
{
    if (shown_count == 0) {
        ui_set_text(page_lbl, "0 Eintraege");
    } else {
        const int first = bambuddy_archive_current_page() * BB_ARCHIVE_PAGE_SIZE + 1;
        const int last = first + shown_count - 1;
        ui_set_text_fmt(page_lbl, "%d-%d", first, last);
    }
    set_enabled(prev_btn, bambuddy_archive_has_prev_page());
    set_enabled(next_btn, bambuddy_archive_has_next_page());
}

static void start_confirmed(void *user_data)
{
    const int32_t id = (int32_t)(intptr_t)user_data;
    if (id == 0) return;

    bambuddy_archive_request_reprint(id, bambuddy_api_awaiting_plate_clear());
    starting_id = id;
    starting_ms = millis();
    rebuild_list();
}

static void start_cb(lv_event_t *e)
{
    const int index = (int)(intptr_t)lv_event_get_user_data(e);
    if (index < 0 || index >= shown_count) return;

    char text[160];
    snprintf(text, sizeof(text),
             "%s\n\nIst die Druckplatte frei und richtig eingelegt?",
             shown[index].name);

    ui_confirm("Archivdruck starten?", text,
               "Noch nicht", "Platte frei, starten", COL_OK,
               start_confirmed, (void *)(intptr_t)shown[index].id);
}

static void delete_confirmed(void *user_data)
{
    const int32_t id = (int32_t)(intptr_t)user_data;
    if (id != 0) bambuddy_archive_request_delete(id);
}

static void delete_cb(lv_event_t *e)
{
    const int index = (int)(intptr_t)lv_event_get_user_data(e);
    if (index < 0 || index >= shown_count) return;

    char text[160];
    snprintf(text, sizeof(text),
             "%s\n\nDie Dateien werden entfernt, vorhandene Statistikwerte bleiben erhalten.",
             shown[index].name);

    ui_confirm("Archiv loeschen?", text,
               "Behalten", "Loeschen", COL_DANGER,
               delete_confirmed, (void *)(intptr_t)shown[index].id);
}

static void preview_closed()
{
    bambuddy_cover_set_big_wanted(false);
}

// Tippen auf die Zeile zeigt das Modell gross — dieselbe Geste wie auf dem
// Statusscreen, und ein Tipp irgendwohin schliesst wieder.
static void preview_open_cb(lv_event_t *e)
{
    const int index = (int)(intptr_t)lv_event_get_user_data(e);
    if (index < 0 || index >= shown_count) return;

    ui_image_view_open(&preview_view, COVER_BIG_SIZE, COVER_BIG_SIZE,
                       shown[index].name, "Vorschau wird geladen ...",
                       preview_closed);

    bambuddy_cover_request_archive(shown[index].id);
}

static void update_preview()
{
    if (!ui_image_view_is_open(&preview_view)) return;

    void *frame = nullptr;
    if (bambuddy_cover_take_big_frame(&frame) && frame) {
        ui_image_view_set_frame(&preview_view, frame, COVER_BIG_SIZE, COVER_BIG_SIZE);
    }
}

static void build_row(int index)
{
    const bambuddy_archive_item_t &item = shown[index];

    lv_obj_t *row = lv_obj_create(list_cont);
    lv_obj_set_size(row, LV_PCT(100), ROW_H);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(row, 10, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_hor(row, 12, 0);
    lv_obj_set_style_pad_ver(row, 0, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(row, lv_color_hex(COL_ACCENT), LV_STATE_PRESSED);
    lv_obj_add_event_cb(row, preview_open_cb, LV_EVENT_CLICKED, (void *)(intptr_t)index);

    lv_obj_t *stripe = lv_obj_create(row);
    lv_obj_set_size(stripe, 6, ROW_H - 24);
    lv_obj_align(stripe, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(stripe, 3, 0);
    lv_obj_set_style_border_width(stripe, 0, 0);
    lv_obj_set_style_bg_color(stripe, lv_color_hex(item.color), 0);
    lv_obj_set_style_bg_opa(stripe, LV_OPA_COVER, 0);

    const int text_w = CONTENT_W - 24 - 18 - PLAY_SIZE - DELETE_SIZE - 20;

    lv_obj_t *name = lv_label_create(row);
    lv_label_set_text(name, item.name);
    lv_obj_set_width(name, text_w);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_align(name, LV_ALIGN_TOP_LEFT, 18, 14);

    char duration[24];
    ui_format_duration(item.print_seconds, duration, sizeof(duration));

    char amount[24] = "";
    if (item.grams > 0.05f) snprintf(amount, sizeof(amount), "  -  %.0f g", item.grams);

    char meta[96];
    if (is_starting(item.id)) {
        snprintf(meta, sizeof(meta), "wird gestartet ...");
    } else if (item.run_count > 0) {
        snprintf(meta, sizeof(meta), "%s%s  -  %s  -  %dx %s",
                 duration, amount, item.filament,
                 (int)item.run_count, status_text(item.status));
    } else {
        snprintf(meta, sizeof(meta), "%s%s  -  %s  -  %s",
                 duration, amount, item.filament, status_text(item.status));
    }

    lv_obj_t *sub = lv_label_create(row);
    lv_label_set_text(sub, meta);
    lv_obj_set_width(sub, text_w);
    lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(sub,
                                lv_color_hex(is_starting(item.id) ? COL_OK : COL_MUTED), 0);
    lv_obj_align(sub, LV_ALIGN_TOP_LEFT, 18, 42);

    const bool locked = is_starting(item.id);

    lv_obj_t *play = lv_button_create(row);
    lv_obj_set_size(play, PLAY_SIZE, PLAY_SIZE);
    lv_obj_align(play, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_radius(play, PLAY_SIZE / 2, 0);
    lv_obj_set_style_bg_color(play, lv_color_hex(COL_OK), 0);
    lv_obj_add_event_cb(play, start_cb, LV_EVENT_CLICKED, (void *)(intptr_t)index);
    if (locked) lv_obj_add_state(play, LV_STATE_DISABLED);

    lv_obj_t *play_lbl = lv_label_create(play);
    lv_label_set_text(play_lbl, LV_SYMBOL_PLAY);
    lv_obj_center(play_lbl);

    lv_obj_t *del = lv_button_create(row);
    lv_obj_set_size(del, DELETE_SIZE, DELETE_SIZE);
    lv_obj_align(del, LV_ALIGN_RIGHT_MID, -(PLAY_SIZE + 8), 0);
    lv_obj_set_style_radius(del, 10, 0);
    lv_obj_set_style_bg_color(del, lv_color_hex(COL_NEUTRAL), 0);
    lv_obj_add_event_cb(del, delete_cb, LV_EVENT_CLICKED, (void *)(intptr_t)index);
    if (locked) lv_obj_add_state(del, LV_STATE_DISABLED);

    lv_obj_t *del_lbl = lv_label_create(del);
    lv_label_set_text(del_lbl, LV_SYMBOL_TRASH);
    lv_obj_center(del_lbl);
}

static void rebuild_list()
{
    lv_obj_clean(list_cont);

    for (int i = 0; i < shown_count; i++) build_row(i);

    if (shown_count == 0) {
        lv_obj_remove_flag(empty_lbl, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(empty_lbl, LV_OBJ_FLAG_HIDDEN);
    }

    ui_set_text(title_lbl, LV_SYMBOL_SAVE "  Archiv");
    update_pager();
}

static void prev_cb(lv_event_t *)
{
    bambuddy_archive_prev_page();
    shown_count = 0;
    rebuild_list();
}

static void next_cb(lv_event_t *)
{
    bambuddy_archive_next_page();
    shown_count = 0;
    rebuild_list();
}

static void ui_tick_cb(lv_timer_t *)
{
    // Nicht umbauen, solange eine Rueckfrage offen ist: Der Umbau loescht
    // die Zeile samt dem Knopf, den der Finger gerade beruehrt hat. Ein
    // Objekt zu entfernen, waehrend LVGL die Beruehrung darauf noch
    // verarbeitet, fuehrt in einen haengenden Bildaufbau.
    if (ui_confirm_is_open()) return;

    if (bambuddy_archive_take_fresh()) {
        shown_count = bambuddy_archive_copy(shown, BB_ARCHIVE_PAGE_SIZE);
        ui_watch("archiv:rebuild");
        rebuild_list();
    }

    if (starting_id != 0 && (millis() - starting_ms) >= START_LOCK_MS) {
        starting_id = 0;
        rebuild_list();
    }

    ui_watch("archiv:preview");
    update_preview();
    ui_watch("archiv:pager");
    update_pager();
    ui_watch("archiv:tick-ende");

    if (bambuddy_archive_message_age() < 6000) {
        ui_set_text(message_lbl, bambuddy_archive_message());
        ui_set_text_color(message_lbl, COL_ACCENT);
    } else {
        ui_set_text(message_lbl, "");
    }
}

void archive_screen_create(lv_obj_t *parent)
{
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    title_lbl = lv_label_create(parent);
    lv_label_set_text(title_lbl, LV_SYMBOL_SAVE "  Archiv");
    lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_16, 0);
    lv_obj_align(title_lbl, LV_ALIGN_TOP_LEFT, PAD + 4, 14);

    list_cont = lv_obj_create(parent);
    lv_obj_set_size(list_cont, CONTENT_W, CONTENT_H - HEADER_H - FOOTER_H - 18);
    lv_obj_align(list_cont, LV_ALIGN_TOP_MID, 0, HEADER_H);
    lv_obj_set_style_bg_opa(list_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list_cont, 0, 0);
    lv_obj_set_style_pad_all(list_cont, 0, 0);
    lv_obj_set_style_pad_row(list_cont, 8, 0);
    lv_obj_set_flex_flow(list_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list_cont, LV_DIR_VER);

    empty_lbl = lv_label_create(parent);
    lv_label_set_text(empty_lbl, "Keine Archiv-Eintraege gefunden.");
    lv_obj_set_style_text_color(empty_lbl, lv_color_hex(COL_MUTED), 0);
    lv_obj_align(empty_lbl, LV_ALIGN_CENTER, 0, -20);

    prev_btn = lv_button_create(parent);
    lv_obj_set_size(prev_btn, 84, 38);
    lv_obj_align(prev_btn, LV_ALIGN_BOTTOM_LEFT, PAD, -30);
    lv_obj_add_event_cb(prev_btn, prev_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *prev_lbl = lv_label_create(prev_btn);
    lv_label_set_text(prev_lbl, LV_SYMBOL_LEFT " Zurueck");
    lv_obj_center(prev_lbl);

    next_btn = lv_button_create(parent);
    lv_obj_set_size(next_btn, 84, 38);
    lv_obj_align(next_btn, LV_ALIGN_BOTTOM_RIGHT, -PAD, -30);
    lv_obj_add_event_cb(next_btn, next_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *next_lbl = lv_label_create(next_btn);
    lv_label_set_text(next_lbl, "Weiter " LV_SYMBOL_RIGHT);
    lv_obj_center(next_lbl);

    page_lbl = lv_label_create(parent);
    lv_label_set_text(page_lbl, "0 Eintraege");
    lv_obj_set_style_text_color(page_lbl, lv_color_hex(COL_MUTED), 0);
    lv_obj_align(page_lbl, LV_ALIGN_BOTTOM_MID, 0, -40);

    message_lbl = lv_label_create(parent);
    lv_label_set_text(message_lbl, "");
    lv_obj_set_width(message_lbl, CONTENT_W);
    lv_label_set_long_mode(message_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(message_lbl, &lv_font_montserrat_12, 0);
    lv_obj_align(message_lbl, LV_ALIGN_BOTTOM_LEFT, PAD + 4, -6);

    lv_timer_t *timer = lv_timer_create(ui_tick_cb, 500, nullptr);
    lv_timer_set_repeat_count(timer, -1);
    update_pager();
}

void archive_screen_set_visible(bool visible)
{
    bambuddy_archive_set_visible(visible);
}
