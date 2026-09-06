#include "skip_objects_view.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bambuddy_skip.h"
#include "ui_dialog.h"
#include "ui_font.h"
#include "ui_fullscreen.h"
#include "ui_kit.h"
#include "ui_layout.h"
#include "ui_theme.h"
#include "ui_util.h"

static constexpr int PAD = 12;
static constexpr int HEADER_H = 54;
static constexpr int FOOTER_H = 56;
static constexpr int CONTENT_TOP = HEADER_H;
static constexpr int CONTENT_BOT = SCREEN_H - FOOTER_H; // 424

// Links die Draufsicht, rechts die Liste — dieselbe Aufteilung wie im
// Bambuddy-Frontend. Nebeneinander und nicht uebereinander, weil die belegte
// Flaeche quadratisch gezeigt wird: Ueber die volle Breite gelegt bliebe
// rechts und links nichts als Rand, und der Liste fehlte die Hoehe.
static constexpr int PLATE = 196;
static constexpr int PLATE_X = PAD;
static constexpr int PLATE_Y = CONTENT_TOP;

// Abstand vom Plattenrand. Die Kacheln sitzen mittig auf ihrem Punkt; ohne
// diesen Rand haengte ein Objekt am aeusseren Rand halb ausserhalb.
static constexpr int PLATE_INSET = 20;
static constexpr int TILE = 28;

static constexpr int LIST_X = PLATE_X + PLATE + PAD;
static constexpr int LIST_W = SCREEN_W - LIST_X - PAD;
static constexpr int ROW_H = 52;
static constexpr int BOX = 26;
static constexpr int TEXT_X = 6 + BOX + GAP_S;

static lv_obj_t *plate_cont = nullptr;
static lv_obj_t *plate_hint = nullptr;
static lv_obj_t *list_cont = nullptr;
static lv_obj_t *empty_lbl = nullptr;
static lv_obj_t *count_lbl = nullptr;
static lv_obj_t *skip_btn = nullptr;
static lv_obj_t *all_btn_lbl = nullptr;
static lv_timer_t *ui_timer = nullptr;

static bambuddy_skip_object_t objects[BB_SKIP_MAX_OBJECTS];
static int object_count = 0;

// Die Auswahl haengt an der Kennung, nicht am Listenplatz: Die Liste wird
// alle paar Sekunden neu geholt, und wenn dabei ein Objekt wegfaellt,
// duerfen die Haken nicht auf den Nachbarn rutschen.
static int32_t selected_ids[BB_SKIP_MAX_OBJECTS];
static int selected_count = 0;

static char last_shown_message[64] = "";

static void rebuild();

// Der Umbau loescht die Zeile oder Kachel, aus deren Klick-Rueckruf er
// kommt. Direkt aufgerufen zoege LVGL dem laufenden Ereignis den Boden weg,
// deshalb erst im naechsten Durchlauf — fuer das Auge derselbe Moment.
static void rebuild_async(void *)
{
    rebuild();
}

// ============================================================
// Auswahl
// ============================================================

static bool is_selected(int32_t id)
{
    for (int i = 0; i < selected_count; i++)
        if (selected_ids[i] == id) return true;
    return false;
}

static void select_add(int32_t id)
{
    if (is_selected(id) || selected_count >= BB_SKIP_MAX_OBJECTS) return;
    selected_ids[selected_count++] = id;
}

static void select_remove(int32_t id)
{
    for (int i = 0; i < selected_count; i++) {
        if (selected_ids[i] != id) continue;
        selected_ids[i] = selected_ids[--selected_count];
        return;
    }
}

// Auswahl auf das eindampfen, was es noch gibt und noch nicht uebersprungen
// ist. Sonst schickte ein spaeter Tipp auf "Überspringen" Kennungen mit, die
// der Drucker gar nicht mehr kennt.
static void prune_selection()
{
    int kept = 0;
    for (int i = 0; i < selected_count; i++) {
        for (int j = 0; j < object_count; j++) {
            if (objects[j].id != selected_ids[i] || objects[j].skipped) continue;
            selected_ids[kept++] = selected_ids[i];
            break;
        }
    }
    selected_count = kept;
}

static int selectable_count()
{
    int n = 0;
    for (int i = 0; i < object_count; i++)
        if (!objects[i].skipped) n++;
    return n;
}

static int skipped_count()
{
    int n = 0;
    for (int i = 0; i < object_count; i++)
        if (objects[i].skipped) n++;
    return n;
}

// Ein Tipp — auf die Zeile oder auf die Kachel in der Draufsicht — macht
// dasselbe. Beide zeigen dieselben Objekte, also darf es auch nur eine
// Bedienung geben.
static void toggle_cb(lv_event_t *e)
{
    const int32_t id = (int32_t)(intptr_t)lv_event_get_user_data(e);

    if (is_selected(id)) {
        select_remove(id);
    } else {
        select_add(id);
    }

    lv_async_call(rebuild_async, nullptr);
}

// ============================================================
// Draufsicht
// ============================================================

// Welche Farbe traegt ein Objekt? An einer Stelle entschieden, damit Kachel
// und Listeneintrag nie auseinanderlaufen.
static uint32_t object_color(const bambuddy_skip_object_t &o)
{
    if (o.skipped) return COL_MUTED;
    return is_selected(o.id) ? COL_ERR : COL_ACCENT;
}

static void build_plate()
{
    lv_obj_clean(plate_cont);

    float x0, y0, x1, y1;
    if (!bambuddy_skip_bbox(&x0, &y0, &x1, &y1) || object_count == 0) {
        lv_obj_remove_flag(plate_hint, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_add_flag(plate_hint, LV_OBJ_FLAG_HIDDEN);

    const float cx = (x0 + x1) / 2.0f;
    const float cy = (y0 + y1) / 2.0f;

    // Die groessere der beiden Ausdehnungen bestimmt den Massstab, damit die
    // Ansicht nicht verzerrt. Die Untergrenze verhindert, dass zwei eng
    // benachbarte Koerper auf Bildschirmbreite auseinandergezogen werden und
    // eine Genauigkeit vortaeuschen, die die Daten nicht hergeben.
    float span = (x1 - x0) > (y1 - y0) ? (x1 - x0) : (y1 - y0);
    if (span < 40.0f) span = 40.0f;

    const float scale = (float)(PLATE - 2 * PLATE_INSET) / span;

    for (int i = 0; i < object_count; i++) {
        const bambuddy_skip_object_t &o = objects[i];
        if (!o.pos_known) continue;

        lv_obj_t *tile = lv_obj_create(plate_cont);
        lv_obj_set_size(tile, TILE, TILE);
        lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_radius(tile, GAP_XS, 0);
        lv_obj_set_style_border_width(tile, 2, 0);
        lv_obj_set_style_pad_all(tile, 0, 0);

        const uint32_t color = object_color(o);
        lv_obj_set_style_bg_color(tile, lv_color_hex(color), 0);
        lv_obj_set_style_bg_opa(tile, o.skipped ? LV_OPA_20 : LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(tile, lv_color_hex(color), 0);

        // Die Y-Achse des Druckers zeigt nach hinten, die des Bildschirms
        // nach unten. Ohne Spiegelung staende die Ansicht auf dem Kopf, und
        // man griffe zuverlaessig das falsche Objekt.
        const int dx = (int)((o.x - cx) * scale);
        const int dy = (int)(-(o.y - cy) * scale);
        lv_obj_align(tile, LV_ALIGN_CENTER, dx, dy);

        if (o.skipped) {
            lv_obj_remove_flag(tile, LV_OBJ_FLAG_CLICKABLE);
        } else {
            lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(tile, toggle_cb, LV_EVENT_CLICKED,
                                (void *)(intptr_t)o.id);
        }

        // Die Nummer ist das Band zwischen beiden Ansichten: in der Liste vor
        // dem Namen, hier in der Kachel. Namen taugen dafuer nicht —
        // mehrfach gesetzte Koerper heissen alle gleich.
        lv_obj_t *num = lv_label_create(tile);
        lv_label_set_text_fmt(num, "%d", i + 1);
        lv_obj_set_style_text_font(num, &bb_font_12, 0);
        lv_obj_set_style_text_color(
            num, lv_color_hex(o.skipped ? COL_MUTED : COL_TEXT), 0);
        lv_obj_center(num);
    }
}

// ============================================================
// Liste
// ============================================================

static void build_row(const bambuddy_skip_object_t &o, int index)
{
    const bool chosen = is_selected(o.id);

    lv_obj_t *row = lv_obj_create(list_cont);
    lv_obj_set_size(row, LV_PCT(100), ROW_H);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(row, 0, 0);
    ui_card_style(row);

    if (o.skipped) {
        // Schon ausgelassen: bleibt sichtbar, aber ohne Griff. Wer sich
        // vertan hat, soll sehen, was er getan hat — rueckgaengig machen
        // laesst es sich ohnehin nicht.
        lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_opa(row, LV_OPA_40, 0);
    } else {
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, toggle_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)o.id);
        if (chosen) lv_obj_set_style_border_color(row, lv_color_hex(COL_ERR), 0);
    }

    // Kaestchen links — gefuellt, wenn gewaehlt. Ein Haken allein waere bei
    // gedaempfter Helligkeit zu leicht zu uebersehen; die Flaeche sieht man
    // auch aus zwei Metern.
    lv_obj_t *box = lv_obj_create(row);
    lv_obj_set_size(box, BOX, BOX);
    lv_obj_align(box, LV_ALIGN_LEFT_MID, 6, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(box, GAP_S, 0);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(COL_LINE), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);

    if (o.skipped || chosen) {
        const uint32_t fill = o.skipped ? COL_MUTED : COL_ERR;
        lv_obj_set_style_bg_color(box, lv_color_hex(fill), 0);
        lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(box, lv_color_hex(fill), 0);

        lv_obj_t *check = lv_label_create(box);
        lv_label_set_text(check, o.skipped ? LV_SYMBOL_CLOSE : LV_SYMBOL_OK);
        lv_obj_set_style_text_color(check, lv_color_white(), 0);
        lv_obj_set_style_text_font(check, &bb_font_12, 0);
        lv_obj_center(check);
    }

    // Nummer und Name in einer Zeile. Die Nummer traegt die Farbe des
    // Objekts, damit das Auge sie in der Draufsicht wiederfindet.
    lv_obj_t *num = lv_label_create(row);
    lv_label_set_text_fmt(num, "%d", index + 1);
    lv_obj_set_style_text_font(num, &bb_font_12, 0);
    lv_obj_set_style_text_color(num, lv_color_hex(object_color(o)), 0);
    lv_obj_align(num, LV_ALIGN_TOP_LEFT, TEXT_X, 9);

    lv_obj_t *name = lv_label_create(row);
    lv_label_set_text(name, o.name[0] ? o.name : "Ohne Namen");
    lv_obj_set_style_text_font(name, &bb_font_14, 0);
    lv_obj_set_style_text_color(name, lv_color_hex(o.skipped ? COL_MUTED : COL_TEXT), 0);
    lv_obj_set_width(name, LIST_W - TEXT_X - 22 - GAP_S);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_align(name, LV_ALIGN_TOP_LEFT, TEXT_X + 22, 8);

    // Darunter die Kennung — dieselbe Zahl, die das Bambuddy-Frontend zeigt.
    // Bei gleichen Namen ist sie der einzige harte Bezug zwischen beiden
    // Anzeigen.
    lv_obj_t *meta = lv_label_create(row);
    if (o.skipped) {
        lv_label_set_text_fmt(meta, "ID %d  ·  übersprungen", (int)o.id);
    } else {
        lv_label_set_text_fmt(meta, "ID %d", (int)o.id);
    }
    lv_obj_set_style_text_font(meta, &bb_font_12, 0);
    lv_obj_set_style_text_color(meta, lv_color_hex(COL_MUTED), 0);
    lv_obj_align(meta, LV_ALIGN_BOTTOM_LEFT, TEXT_X, -8);
}

// ============================================================
// Fusszeile
// ============================================================

static void update_footer()
{
    if (!count_lbl) return;

    const int selectable = selectable_count();
    const int gone = skipped_count();

    if (selected_count > 0) {
        lv_label_set_text_fmt(count_lbl, "%d von %d ausgewählt", selected_count,
                              selectable);
    } else if (gone > 0) {
        // Ohne Auswahl sagt die Zeile, was bisher geschah. "Nichts
        // ausgewaehlt" waere hier die unwichtigere der beiden Aussagen.
        lv_label_set_text_fmt(count_lbl, "%d von %d schon übersprungen", gone,
                              object_count);
    } else {
        lv_label_set_text(count_lbl, "Nichts ausgewählt");
    }

    const bool ready = selected_count > 0;
    lv_obj_set_style_bg_opa(skip_btn, ready ? LV_OPA_COVER : LV_OPA_40, 0);
    if (ready) {
        lv_obj_add_flag(skip_btn, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_remove_flag(skip_btn, LV_OBJ_FLAG_CLICKABLE);
    }

    if (all_btn_lbl) {
        const bool all = selectable > 0 && selected_count >= selectable;
        lv_label_set_text(all_btn_lbl, all ? "Keins" : "Alle");
    }
}

static void rebuild()
{
    if (!list_cont) return;

    lv_obj_clean(list_cont);
    build_plate();

    if (object_count == 0) {
        lv_obj_remove_flag(empty_lbl, LV_OBJ_FLAG_HIDDEN);
        ui_set_text(empty_lbl, bambuddy_skip_loaded()
                                   ? "Für diesen Druck sind keine einzelnen "
                                     "Objekte bekannt."
                                   : "Lade ...");
        update_footer();
        return;
    }

    lv_obj_add_flag(empty_lbl, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < object_count; i++) build_row(objects[i], i);

    update_footer();
}

// ============================================================
// Ueberspringen
// ============================================================

static void skip_confirmed(void *)
{
    bambuddy_skip_request(selected_ids, selected_count);
    selected_count = 0;
    rebuild();
}

static void skip_cb(lv_event_t *)
{
    if (selected_count == 0 || ui_confirm_is_open()) return;

    char text[160];
    if (selected_count == 1) {
        // Nummer und Name zusammen: Die Nummer zeigt auf die Kachel in der
        // Draufsicht, der Name auf die Datei. Erst beides sagt, was gleich
        // verloren geht.
        int index = 0;
        const char *name = "";
        for (int i = 0; i < object_count; i++) {
            if (objects[i].id != selected_ids[0]) continue;
            index = i + 1;
            name = objects[i].name;
        }

        snprintf(text, sizeof(text),
                 "Objekt %d (\"%s\") wird für den Rest des Drucks ausgelassen. "
                 "Das lässt sich nicht rückgängig machen.",
                 index, name[0] ? name : "ohne Namen");
    } else {
        snprintf(text, sizeof(text),
                 "%d Objekte werden für den Rest des Drucks ausgelassen. "
                 "Das lässt sich nicht rückgängig machen.",
                 selected_count);
    }

    ui_confirm("Objekte überspringen?", text, "Abbrechen", "Überspringen",
               COL_ERR, skip_confirmed, nullptr);
}

static void all_cb(lv_event_t *)
{
    const int selectable = selectable_count();
    const bool all = selectable > 0 && selected_count >= selectable;

    selected_count = 0;
    if (!all) {
        for (int i = 0; i < object_count; i++)
            if (!objects[i].skipped) select_add(objects[i].id);
    }

    // Derselbe Weg wie beim Antippen einer Zeile — ein Weg, ein Verhalten.
    lv_async_call(rebuild_async, nullptr);
}

// ============================================================
// Takt
// ============================================================

static void ui_tick_cb(lv_timer_t *)
{
    // Solange eine Rueckfrage offen steht, nicht umbauen: Der Dialog zeigt
    // auf eine Auswahl, die dabei verschwinden koennte.
    if (ui_confirm_is_open()) return;

    if (bambuddy_skip_take_fresh()) {
        object_count = bambuddy_skip_copy(objects, BB_SKIP_MAX_OBJECTS);
        prune_selection();
        rebuild();
    }

    // Rueckmeldung des Netzwerk-Task durchreichen — vor allem der Klartext
    // aus einer Fehlerantwort ("No active print") gehoert vor die Augen.
    const char *msg = bambuddy_skip_message();
    if (msg[0] && bambuddy_skip_message_age() < 6000 &&
        strcmp(msg, last_shown_message) != 0) {
        strncpy(last_shown_message, msg, sizeof(last_shown_message) - 1);
        last_shown_message[sizeof(last_shown_message) - 1] = '\0';
        lv_label_set_text(count_lbl, msg);
    }
}

// ============================================================
// Aufbau
// ============================================================

void skip_objects_view_create(lv_obj_t *parent)
{
    bambuddy_skip_set_visible(true);

    selected_count = 0;
    object_count = bambuddy_skip_copy(objects, BB_SKIP_MAX_OBJECTS);
    last_shown_message[0] = '\0';

    // --- Draufsicht -------------------------------------------------------
    plate_cont = ui_card(parent, PLATE_X, PLATE_Y, PLATE, PLATE);
    lv_obj_remove_flag(plate_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(plate_cont, 0, 0);

    plate_hint = lv_label_create(parent);
    lv_label_set_text(plate_hint, "Keine Positionen gemeldet");
    lv_obj_set_style_text_font(plate_hint, &bb_font_12, 0);
    lv_obj_set_style_text_color(plate_hint, lv_color_hex(COL_MUTED), 0);
    lv_obj_set_width(plate_hint, PLATE - 2 * GAP_M);
    lv_label_set_long_mode(plate_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(plate_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(plate_hint, LV_ALIGN_TOP_LEFT, PLATE_X + GAP_M,
                 PLATE_Y + PLATE / 2 - 10);

    // Der Hinweis unter der Ansicht erklaert den Ausschnitt. Ohne ihn haelt
    // man sie fuer die ganze Druckplatte und wundert sich, dass zwei Teile
    // in der Mitte die halbe Flaeche fuellen.
    lv_obj_t *caption = lv_label_create(parent);
    lv_label_set_text(caption, "Draufsicht auf die belegte Fläche.\n"
                               "Nummern wie in der Liste.");
    lv_obj_set_style_text_font(caption, &bb_font_12, 0);
    lv_obj_set_style_text_color(caption, lv_color_hex(COL_MUTED), 0);
    lv_obj_set_width(caption, PLATE);
    lv_label_set_long_mode(caption, LV_LABEL_LONG_WRAP);
    lv_obj_align(caption, LV_ALIGN_TOP_LEFT, PLATE_X + 2, PLATE_Y + PLATE + GAP_M);

    // --- Liste ------------------------------------------------------------
    list_cont = lv_obj_create(parent);
    lv_obj_set_size(list_cont, LIST_W, CONTENT_BOT - CONTENT_TOP);
    lv_obj_align(list_cont, LV_ALIGN_TOP_LEFT, LIST_X, CONTENT_TOP);
    lv_obj_set_style_bg_opa(list_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list_cont, 0, 0);
    lv_obj_set_style_pad_all(list_cont, 0, 0);
    lv_obj_set_style_pad_row(list_cont, GAP_S, 0);
    lv_obj_set_flex_flow(list_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list_cont, LV_DIR_VER);

    empty_lbl = lv_label_create(parent);
    ui_set_text(empty_lbl, "Lade ...");
    lv_obj_set_width(empty_lbl, LIST_W);
    lv_label_set_long_mode(empty_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(empty_lbl, lv_color_hex(COL_MUTED), 0);
    lv_obj_align(empty_lbl, LV_ALIGN_TOP_LEFT, LIST_X, CONTENT_TOP + 20);

    // --- Kopf und Fuss ----------------------------------------------------
    lv_obj_t *all_btn = lv_button_create(parent);
    lv_obj_set_size(all_btn, 76, 38);
    lv_obj_align(all_btn, LV_ALIGN_TOP_RIGHT, -PAD, 7);
    lv_obj_set_style_radius(all_btn, RADIUS_CTRL, 0);
    lv_obj_set_style_bg_color(all_btn, lv_color_hex(COL_NEUTRAL), 0);
    lv_obj_add_event_cb(all_btn, all_cb, LV_EVENT_CLICKED, nullptr);

    all_btn_lbl = lv_label_create(all_btn);
    lv_label_set_text(all_btn_lbl, "Alle");
    lv_obj_center(all_btn_lbl);

    count_lbl = lv_label_create(parent);
    lv_label_set_text(count_lbl, "Nichts ausgewählt");
    lv_obj_set_style_text_font(count_lbl, &bb_font_12, 0);
    lv_obj_set_style_text_color(count_lbl, lv_color_hex(COL_MUTED), 0);
    lv_obj_set_width(count_lbl, SCREEN_W - 2 * PAD - 150);
    lv_label_set_long_mode(count_lbl, LV_LABEL_LONG_DOT);
    lv_obj_align(count_lbl, LV_ALIGN_BOTTOM_LEFT, PAD + 2, -20);

    skip_btn = ui_button(parent, COL_ERR, 140, 44);
    lv_obj_align(skip_btn, LV_ALIGN_BOTTOM_RIGHT, -PAD, -8);
    lv_obj_add_event_cb(skip_btn, skip_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *skip_btn_lbl = lv_label_create(skip_btn);
    lv_label_set_text(skip_btn_lbl, "Überspringen");
    lv_obj_set_style_text_font(skip_btn_lbl, &bb_font_12, 0);
    lv_obj_center(skip_btn_lbl);

    ui_timer = lv_timer_create(ui_tick_cb, 500, nullptr);
    lv_timer_set_repeat_count(ui_timer, -1);

    rebuild();
}

void skip_objects_view_destroy()
{
    ui_confirm_close();

    if (ui_timer) {
        lv_timer_delete(ui_timer);
        ui_timer = nullptr;
    }

    bambuddy_skip_set_visible(false);

    plate_cont = nullptr;
    plate_hint = nullptr;
    list_cont = nullptr;
    empty_lbl = nullptr;
    count_lbl = nullptr;
    skip_btn = nullptr;
    all_btn_lbl = nullptr;
    object_count = 0;
    selected_count = 0;
}

void skip_objects_view_open()
{
    ui_fullscreen_open("Objekte überspringen", COL_ERR, skip_objects_view_create,
                       skip_objects_view_destroy);
}
