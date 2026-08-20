#include "queue_screen.h"

#include <Arduino.h>
#include <string.h>

#include "bambuddy_api.h"
#include "bambuddy_cover.h"
#include "bambuddy_queue.h"
#include "settings_screen.h"
#include "ui_layout.h"
#include "ui_dialog.h"
#include "ui_image_view.h"
#include "ui_kit.h"
#include "ui_theme.h"
#include "ui_util.h"
#include "ui_watch.h"
#include "ui_font.h"

// ============================================================
// Layout
// ============================================================
static constexpr int PAD = 12;
static constexpr int CONTENT_W = SCREEN_W - 2 * PAD;
static constexpr int HEADER_H = 46;
static constexpr int ROW_H = 76; // Zeile mit zwei Textzeilen und Startknopf
static constexpr int PLAY_SIZE = 52;
static constexpr int DEL_SIZE = 44;


// ============================================================
// State
// ============================================================
static lv_obj_t *title_lbl;
static lv_obj_t *list_cont;
static lv_obj_t *empty_lbl;
static lv_obj_t *message_lbl;

static bambuddy_queue_item_t shown[BB_QUEUE_MAX_ITEMS];
static int shown_count = 0;

// Nach einem Start bleibt der Eintrag kurz gesperrt: die Liste wird erst
// alle 10 Sekunden neu geholt, und der Drucker braucht ohnehin einen
// Moment. Ohne Sperre koennte man denselben Auftrag zweimal losschicken.
static constexpr uint32_t START_LOCK_MS = 45000;
static int32_t starting_id = 0;
static uint32_t starting_ms = 0;

// Vollbild-Vorschau des Modells
static ui_image_view_t preview_view;

static lv_timer_t *ui_timer = nullptr;

// ============================================================
// Formatierung
// ============================================================

// ============================================================
// Starten mit Rueckfrage
// ============================================================

static bool is_starting(int32_t item_id)
{
    return starting_id != 0 && item_id == starting_id &&
           (millis() - starting_ms) < START_LOCK_MS;
}

static void rebuild_list();

// Die bestaetigte Kennung wandert durch user_data — so braucht der Screen
// keinen eigenen Merker fuer "worueber wird gerade gefragt".
static void start_confirmed(void *user_data)
{
    const int32_t id = (int32_t)(intptr_t)user_data;
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
    const int index = (int)(intptr_t)lv_event_get_user_data(e);
    if (index < 0 || index >= shown_count) return;

    // Ist der Drucker beschaeftigt, gar nicht erst nach der Druckplatte
    // fragen. Wer dort "Platte frei" bestaetigt, schickt clear-plate an
    // einen laufenden Druck — und im schlimmsten Fall faehrt der Kopf in
    // ein Werkstueck, das noch auf der Platte steht.
    //
    // Abschaltbar ueber die Einstellungen: Die Sperre kann auch im Weg
    // stehen, wenn Bambuddy einen Zustand noch meldet, den der Drucker
    // laengst hinter sich hat.
    const char *blocked =
        settings_start_guard() ? bambuddy_api_start_blocked_reason() : "";
    if (blocked[0]) {
        char warning[220];
        snprintf(warning, sizeof(warning),
                 "%s\n\n%s\nEin Start ist erst möglich, wenn der Drucker fertig "
                 "und die Platte frei ist.",
                 shown[index].name, blocked);
        ui_info("Start derzeit nicht möglich", warning, "Verstanden");
        return;
    }

    char text[160];
    snprintf(text, sizeof(text),
             "%s\n\nIst die Druckplatte frei und richtig eingelegt?",
             shown[index].name);

    ui_confirm("Druck starten?", text,
               "Noch nicht", "Platte frei, starten", COL_OK,
               start_confirmed, (void *)(intptr_t)shown[index].id);
}

// ============================================================
// Vorschaubild
// ============================================================

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
    if (shown[index].archive_id == 0) return; // kein Archiv, kein Bild

    ui_image_view_open(&preview_view, COVER_BIG_SIZE, COVER_BIG_SIZE,
                       shown[index].name, "Vorschau wird geladen ...", preview_closed);

    bambuddy_cover_request_archive(shown[index].archive_id);
}

static void update_preview()
{
    if (!ui_image_view_is_open(&preview_view)) return;

    void *frame = nullptr;
    if (bambuddy_cover_take_big_frame(&frame) && frame) {
        ui_image_view_set_frame(&preview_view, frame, COVER_BIG_SIZE, COVER_BIG_SIZE);
    }
}

// ============================================================
// Entfernen
// ============================================================

static void delete_confirmed(void *user_data)
{
    const int32_t id = (int32_t)(intptr_t)user_data;
    if (id != 0) bambuddy_queue_request_delete(id);
}

// Auch das Entfernen bekommt eine Rueckfrage: der Knopf sitzt direkt neben
// Start, und auf einem Touchscreen greift man schnell daneben.
static void delete_cb(lv_event_t *e)
{
    const int index = (int)(intptr_t)lv_event_get_user_data(e);
    if (index < 0 || index >= shown_count) return;

    char text[128];
    snprintf(text, sizeof(text), "%s wird nicht gedruckt.", shown[index].name);

    ui_confirm("Aus der Warteschlange entfernen?", text,
               "Behalten", "Entfernen", COL_ERR,
               delete_confirmed, (void *)(intptr_t)shown[index].id);
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
    // Karte statt Streifen: derselbe Radius, derselbe Rand und derselbe
    // Flaechenton wie ueberall sonst (ui_kit.h). Der feine Rand ist das, was
    // eine Liste aus Karten von einer Liste aus Farbfeldern unterscheidet.
    lv_obj_set_style_radius(row, RADIUS_CARD, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(COL_SURFACE), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(COL_LINE), 0);
    lv_obj_set_style_border_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(row, GAP_M, 0);
    lv_obj_set_style_pad_ver(row, 0, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(row, lv_color_hex(COL_RAISED), LV_STATE_PRESSED);
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
    ui_format_duration(item.print_seconds, duration, sizeof(duration));

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
    lv_obj_set_style_text_font(sub, &bb_font_12, 0);
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
    lv_obj_set_style_radius(del, RADIUS_CTRL, 0);
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

    // Mehr Auftraege als Platz im Speicher: das gehoert sichtbar gemacht,
    // sonst fehlen sie kommentarlos.
    const int total = bambuddy_queue_total();
    if (total > shown_count) {
        lv_obj_t *more = lv_label_create(list_cont);
        lv_label_set_text_fmt(more, "... und %d weitere", total - shown_count);
        lv_obj_set_style_text_font(more, &bb_font_12, 0);
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
    // Nicht umbauen, solange eine Rueckfrage offen ist: Der Umbau loescht
    // die Zeile samt dem Knopf, den der Finger gerade beruehrt hat. Ein
    // Objekt zu entfernen, waehrend LVGL die Beruehrung darauf noch
    // verarbeitet, fuehrt in einen haengenden Bildaufbau.
    if (ui_confirm_is_open()) return;

    if (bambuddy_queue_take_fresh()) {
        shown_count = bambuddy_queue_copy(shown, BB_QUEUE_MAX_ITEMS);
        rebuild_list();
    }

    // Sperre abgelaufen: Zeile wieder freigeben
    if (starting_id != 0 && (millis() - starting_ms) >= START_LOCK_MS) {
        starting_id = 0;
        rebuild_list();
    }

    ui_watch("queue:preview");
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
    lv_obj_set_style_text_font(title_lbl, &bb_font_16, 0);
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
    lv_label_set_text(empty_lbl, "Keine Aufträge in der Warteschlange.");
    lv_obj_set_style_text_color(empty_lbl, lv_color_hex(COL_MUTED), 0);
    lv_obj_align(empty_lbl, LV_ALIGN_CENTER, 0, -20);

    message_lbl = lv_label_create(parent);
    lv_label_set_text(message_lbl, "");
    lv_obj_set_width(message_lbl, CONTENT_W);
    lv_label_set_long_mode(message_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(message_lbl, &bb_font_12, 0);
    lv_obj_align(message_lbl, LV_ALIGN_BOTTOM_LEFT, PAD + 4, -8);

    ui_timer = lv_timer_create(ui_tick_cb, 500, nullptr);
    lv_timer_set_repeat_count(ui_timer, -1);
}

void queue_screen_set_visible(bool visible)
{
    bambuddy_queue_set_visible(visible);
}
