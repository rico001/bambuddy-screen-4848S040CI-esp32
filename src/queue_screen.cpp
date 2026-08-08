#include "queue_screen.h"

#include <Arduino.h>
#include <string.h>

#include "bambuddy_api.h"
#include "bambuddy_cover.h"
#include "bambuddy_queue.h"
#include "ui_layout.h"
#include "ui_util.h"

// ============================================================
// Layout
// ============================================================
static constexpr int PAD = 12;
static constexpr int CONTENT_W = SCREEN_W - 2 * PAD;
static constexpr int HEADER_H = 46;
static constexpr int ROW_H = 76; // Zeile mit zwei Textzeilen und Startknopf
static constexpr int PLAY_SIZE = 52;
static constexpr int DEL_SIZE = 44;

static constexpr uint32_t COL_MUTED = 0x9E9E9E;
static constexpr uint32_t COL_OK = 0x4CAF50;
static constexpr uint32_t COL_ACCENT = 0x2196F3;
static constexpr uint32_t COL_ERR = 0xE53935;
static constexpr uint32_t COL_WARN = 0xFFB300;

// ============================================================
// State
// ============================================================
static lv_obj_t *title_lbl;
static lv_obj_t *list_cont;
static lv_obj_t *empty_lbl;
static lv_obj_t *message_lbl;
static lv_obj_t *confirm_box = nullptr;

static bambuddy_queue_item_t shown[BB_QUEUE_MAX_ITEMS];
static int shown_count = 0;
static int32_t confirm_item_id = 0;

// Nach einem Start bleibt der Eintrag kurz gesperrt: die Liste wird erst
// alle 10 Sekunden neu geholt, und der Drucker braucht ohnehin einen
// Moment. Ohne Sperre koennte man denselben Auftrag zweimal losschicken.
static constexpr uint32_t START_LOCK_MS = 30000;
static int32_t starting_id = 0;
static uint32_t starting_ms = 0;

// Vollbild-Vorschau des Modells
static lv_obj_t *preview_overlay = nullptr;
static lv_obj_t *preview_canvas = nullptr;
static lv_obj_t *preview_hint = nullptr;
static lv_obj_t *preview_title = nullptr;

static lv_timer_t *ui_timer = nullptr;

// ============================================================
// Formatierung
// ============================================================

static void format_duration(int32_t seconds, char *out, size_t out_len)
{
    if (seconds <= 0) {
        strncpy(out, "", out_len);
        return;
    }

    const int minutes = (seconds + 30) / 60; // auf Minuten runden
    if (minutes < 60) {
        snprintf(out, out_len, "%d min", minutes);
    } else {
        snprintf(out, out_len, "%d h %02d min", minutes / 60, minutes % 60);
    }
}

// ============================================================
// Starten mit Rueckfrage
// ============================================================

static bool is_starting(int32_t item_id)
{
    return starting_id != 0 && item_id == starting_id &&
           (millis() - starting_ms) < START_LOCK_MS;
}

static void rebuild_list();

static void confirm_close()
{
    if (confirm_box) {
        lv_msgbox_close(confirm_box);
        confirm_box = nullptr;
    }
    confirm_item_id = 0;
}

static void confirm_no_cb(lv_event_t *)
{
    confirm_close();
}

static void confirm_yes_cb(lv_event_t *)
{
    const int32_t id = confirm_item_id;
    confirm_close();
    if (id == 0) return;

    // Das Ja der Rueckfrage ist zugleich die Bestaetigung an den Drucker,
    // dass die Platte frei ist — er wartet sonst und startet nicht.
    bambuddy_queue_request_start(id, bambuddy_api_awaiting_plate_clear());

    starting_id = id;
    starting_ms = millis();
    rebuild_list(); // Sperre sofort sichtbar machen
}

static void start_cb(lv_event_t *e)
{
    if (confirm_box) return;

    const int index = (int)(intptr_t)lv_event_get_user_data(e);
    if (index < 0 || index >= shown_count) return;

    confirm_item_id = shown[index].id;

    confirm_box = lv_msgbox_create(lv_layer_top());
    lv_msgbox_add_title(confirm_box, "Druck starten?");

    char text[160];
    snprintf(text, sizeof(text),
             "%s\n\nIst die Druckplatte frei und richtig eingelegt?",
             shown[index].name);
    lv_msgbox_add_text(confirm_box, text);

    lv_obj_t *no = lv_msgbox_add_footer_button(confirm_box, "Noch nicht");
    lv_obj_add_event_cb(no, confirm_no_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *yes = lv_msgbox_add_footer_button(confirm_box, "Platte frei, starten");
    lv_obj_set_style_bg_color(yes, lv_color_hex(COL_OK), 0);
    lv_obj_add_event_cb(yes, confirm_yes_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_set_width(confirm_box, 420);
    lv_obj_center(confirm_box);
}

// ============================================================
// Vorschaubild
// ============================================================

static void preview_close()
{
    bambuddy_cover_set_big_wanted(false);
    if (preview_overlay) lv_obj_add_flag(preview_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void preview_close_cb(lv_event_t *)
{
    preview_close();
}

// Tippen auf die Zeile zeigt das Modell gross — dieselbe Geste wie auf dem
// Statusscreen, und ein Tipp irgendwohin schliesst wieder.
static void preview_open_cb(lv_event_t *e)
{
    const int index = (int)(intptr_t)lv_event_get_user_data(e);
    if (index < 0 || index >= shown_count) return;
    if (shown[index].archive_id == 0) return; // kein Archiv, kein Bild

    if (!preview_overlay) {
        preview_overlay = lv_obj_create(lv_layer_top());
        lv_obj_set_size(preview_overlay, SCREEN_W, SCREEN_H);
        lv_obj_align(preview_overlay, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_remove_flag(preview_overlay, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_radius(preview_overlay, 0, 0);
        lv_obj_set_style_border_width(preview_overlay, 0, 0);
        lv_obj_set_style_pad_all(preview_overlay, 0, 0);
        lv_obj_set_style_bg_color(preview_overlay, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(preview_overlay, LV_OPA_COVER, 0);
        lv_obj_add_flag(preview_overlay, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(preview_overlay, preview_close_cb, LV_EVENT_CLICKED, nullptr);

        preview_canvas = lv_canvas_create(preview_overlay);
        lv_obj_set_size(preview_canvas, COVER_BIG_SIZE, COVER_BIG_SIZE);
        lv_obj_center(preview_canvas);
        lv_obj_add_flag(preview_canvas, LV_OBJ_FLAG_HIDDEN);

        preview_hint = lv_label_create(preview_overlay);
        lv_obj_set_style_text_color(preview_hint, lv_color_hex(COL_MUTED), 0);
        lv_obj_set_style_text_align(preview_hint, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(preview_hint);

        // Titel oben: sonst sieht man ein Modell und weiss nicht, welches
        preview_title = lv_label_create(preview_overlay);
        lv_obj_set_width(preview_title, SCREEN_W - 40);
        lv_label_set_long_mode(preview_title, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(preview_title, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(preview_title, lv_color_white(), 0);
        lv_obj_align(preview_title, LV_ALIGN_TOP_MID, 0, 18);
    }

    lv_label_set_text(preview_title, shown[index].name);

    lv_label_set_text(preview_hint, "Vorschau wird geladen ...");
    lv_obj_remove_flag(preview_hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(preview_canvas, LV_OBJ_FLAG_HIDDEN);

    lv_obj_remove_flag(preview_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(preview_overlay);

    bambuddy_cover_request_archive(shown[index].archive_id);
}

static void update_preview()
{
    if (!preview_overlay || lv_obj_has_flag(preview_overlay, LV_OBJ_FLAG_HIDDEN)) return;

    void *frame = nullptr;
    if (bambuddy_cover_take_big_frame(&frame) && frame) {
        lv_canvas_set_buffer(preview_canvas, frame, COVER_BIG_SIZE, COVER_BIG_SIZE,
                             LV_COLOR_FORMAT_RGB565);
        lv_obj_remove_flag(preview_canvas, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(preview_hint, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(preview_canvas);
    }
}

// ============================================================
// Entfernen
// ============================================================

static void delete_yes_cb(lv_event_t *)
{
    const int32_t id = confirm_item_id;
    confirm_close();
    if (id != 0) bambuddy_queue_request_delete(id);
}

// Auch das Entfernen bekommt eine Rueckfrage: der Knopf sitzt direkt neben
// Start, und auf einem Touchscreen greift man schnell danebem.
static void delete_cb(lv_event_t *e)
{
    if (confirm_box) return;

    const int index = (int)(intptr_t)lv_event_get_user_data(e);
    if (index < 0 || index >= shown_count) return;

    confirm_item_id = shown[index].id;

    confirm_box = lv_msgbox_create(lv_layer_top());
    lv_msgbox_add_title(confirm_box, "Aus der Warteschlange entfernen?");

    char text[128];
    snprintf(text, sizeof(text), "%s wird nicht gedruckt.", shown[index].name);
    lv_msgbox_add_text(confirm_box, text);

    lv_obj_t *no = lv_msgbox_add_footer_button(confirm_box, "Behalten");
    lv_obj_add_event_cb(no, confirm_no_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *yes = lv_msgbox_add_footer_button(confirm_box, "Entfernen");
    lv_obj_set_style_bg_color(yes, lv_color_hex(COL_ERR), 0);
    lv_obj_add_event_cb(yes, delete_yes_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_set_width(confirm_box, 420);
    lv_obj_center(confirm_box);
}

// ============================================================
// Liste aufbauen
// ============================================================

static void build_row(int index)
{
    const bambuddy_queue_item_t &item = shown[index];

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

    // Farbstreifen links: zeigt das Filament, ohne Platz fuer Text zu kosten
    lv_obj_t *stripe = lv_obj_create(row);
    lv_obj_set_size(stripe, 6, ROW_H - 24);
    lv_obj_align(stripe, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(stripe, 3, 0);
    lv_obj_set_style_border_width(stripe, 0, 0);
    lv_obj_set_style_bg_color(stripe, lv_color_hex(item.color), 0);
    lv_obj_set_style_bg_opa(stripe, LV_OPA_COVER, 0);

    const int text_w = CONTENT_W - 24 - 18 - PLAY_SIZE - DEL_SIZE - 20;

    lv_obj_t *name = lv_label_create(row);
    lv_label_set_text(name, item.name);
    lv_obj_set_width(name, text_w);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_align(name, LV_ALIGN_TOP_LEFT, 18, 14);

    char duration[24];
    format_duration(item.print_seconds, duration, sizeof(duration));

    // Voraussichtliche Spule: Bambuddy legt sie erst beim Start fest, wir
    // zeigen den passenden Slot. Passt keiner, ist das die wichtigere
    // Information — dann fehlt schlicht das Material.
    char slot[8];
    const bool has_slot = bambuddy_queue_match_slot(item.filament, item.color,
                                                    slot, sizeof(slot));

    char meta[96];
    char amount[24] = "";
    if (item.grams > 0.05f) snprintf(amount, sizeof(amount), "  -  %.0f g", item.grams);

    if (has_slot) {
        snprintf(meta, sizeof(meta), "%s%s  -  %s  %s",
                 duration, amount, item.filament, slot);
    } else {
        snprintf(meta, sizeof(meta), "%s%s  -  %s nicht geladen",
                 duration, amount, item.filament);
    }

    if (is_starting(item.id)) {
        snprintf(meta, sizeof(meta), "%s", "wird gestartet ...");
    }

    lv_obj_t *sub = lv_label_create(row);
    lv_label_set_text(sub, meta);
    lv_obj_set_width(sub, text_w);
    lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(sub,
                                lv_color_hex(is_starting(item.id) ? COL_OK
                                             : (has_slot ? COL_MUTED : COL_WARN)), 0);
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
    lv_obj_set_size(del, DEL_SIZE, DEL_SIZE);
    lv_obj_align(del, LV_ALIGN_RIGHT_MID, -(PLAY_SIZE + 8), 0);
    lv_obj_set_style_radius(del, 10, 0);
    lv_obj_set_style_bg_color(del, lv_color_hex(0x546E7A), 0);
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

    // Mehr Auftraege als Platz im Speicher: das gehoert sichtbar gemacht,
    // sonst fehlen sie kommentarlos.
    const int total = bambuddy_queue_total();
    if (total > shown_count) {
        lv_obj_t *more = lv_label_create(list_cont);
        lv_label_set_text_fmt(more, "... und %d weitere", total - shown_count);
        lv_obj_set_style_text_font(more, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(more, lv_color_hex(COL_MUTED), 0);
        lv_obj_set_style_pad_left(more, 18, 0);
        lv_obj_set_style_pad_top(more, 4, 0);
    }

    if (shown_count == 0) {
        lv_obj_remove_flag(empty_lbl, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(empty_lbl, LV_OBJ_FLAG_HIDDEN);
    }

    ui_set_text_fmt(title_lbl, LV_SYMBOL_LIST "  Warteschlange (%d)",
                    bambuddy_queue_total());
}

static void ui_tick_cb(lv_timer_t *)
{
    // Nur neu aufbauen, wenn sich wirklich etwas geaendert hat — sonst
    // wuerde die Liste im Sekundentakt neu gezeichnet.
    if (bambuddy_queue_take_fresh()) {
        shown_count = bambuddy_queue_copy(shown, BB_QUEUE_MAX_ITEMS);
        rebuild_list();
    }

    // Sperre abgelaufen: Zeile wieder freigeben
    if (starting_id != 0 && (millis() - starting_ms) >= START_LOCK_MS) {
        starting_id = 0;
        rebuild_list();
    }

    update_preview();

    if (bambuddy_queue_message_age() < 6000) {
        ui_set_text(message_lbl, bambuddy_queue_message());
        ui_set_text_color(message_lbl, COL_ACCENT);
    } else {
        ui_set_text(message_lbl, "");
    }
}

// ============================================================
// Aufbau
// ============================================================

void queue_screen_create(lv_obj_t *parent)
{
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    title_lbl = lv_label_create(parent);
    lv_label_set_text(title_lbl, LV_SYMBOL_LIST "  Warteschlange");
    lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_16, 0);
    lv_obj_align(title_lbl, LV_ALIGN_TOP_LEFT, PAD + 4, 14);

    list_cont = lv_obj_create(parent);
    lv_obj_set_size(list_cont, CONTENT_W, CONTENT_H - HEADER_H - 34);
    lv_obj_align(list_cont, LV_ALIGN_TOP_MID, 0, HEADER_H);
    lv_obj_set_style_bg_opa(list_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list_cont, 0, 0);
    lv_obj_set_style_pad_all(list_cont, 0, 0);
    lv_obj_set_style_pad_row(list_cont, 8, 0);
    lv_obj_set_flex_flow(list_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list_cont, LV_DIR_VER);

    empty_lbl = lv_label_create(parent);
    lv_label_set_text(empty_lbl, "Keine Auftraege in der Warteschlange.");
    lv_obj_set_style_text_color(empty_lbl, lv_color_hex(COL_MUTED), 0);
    lv_obj_align(empty_lbl, LV_ALIGN_CENTER, 0, -20);

    message_lbl = lv_label_create(parent);
    lv_label_set_text(message_lbl, "");
    lv_obj_set_width(message_lbl, CONTENT_W);
    lv_label_set_long_mode(message_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(message_lbl, &lv_font_montserrat_12, 0);
    lv_obj_align(message_lbl, LV_ALIGN_BOTTOM_LEFT, PAD + 4, -8);

    ui_timer = lv_timer_create(ui_tick_cb, 500, nullptr);
    lv_timer_set_repeat_count(ui_timer, -1);
}

void queue_screen_set_visible(bool visible)
{
    bambuddy_queue_set_visible(visible);
}
