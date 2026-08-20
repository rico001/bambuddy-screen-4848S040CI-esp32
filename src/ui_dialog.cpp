#include "ui_dialog.h"

#include "ui_font.h"
#include "ui_kit.h"
#include "ui_theme.h"
#include "ui_watch.h"

static lv_obj_t *box = nullptr;
static ui_confirm_cb_t on_ok_cb = nullptr;

// Aussehen der Box an einer Stelle: Radius, Flaeche, Rand und Innenabstand
// wie bei den Karten der Screens (ui_kit.h). Ein Dialog, der anders aussieht
// als der Screen darunter, wirkt wie ein Fremdkoerper aus einer anderen App.
static void style_box(lv_obj_t *obj)
{
    ui_card_style(obj);
    lv_obj_set_style_pad_all(obj, GAP_L, 0);
    lv_obj_set_style_pad_row(obj, GAP_S, 0);

    // Der Grund darunter bleibt sichtbar, aber gedaempft: So bleibt klar,
    // wo man herkam, ohne dass es vom Dialog ablenkt.
    lv_obj_set_style_bg_color(lv_layer_top(), lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(lv_layer_top(), LV_OPA_40, 0);
}

// Kopfzeile des Dialogs.
//
// LVGLs eigener Kopf ist ein Balken mit eigener Flaeche, eigener Hoehe und
// eigenem Innenabstand — im neuen Bild ein Fremdkoerper: eine graue Leiste
// quer durch eine Karte. Hier bleibt davon nur der Titel uebrig, dafuer
// bekommt er einen schmalen farbigen Streifen davor. Der traegt dieselbe
// Farbe wie der bestaetigende Knopf: Rot bei etwas Zerstoerendem, Orange bei
// den Steckdosen, sonst der Akzent. Damit ist die Art des Dialogs schon zu
// sehen, bevor man den Text gelesen hat.
static void add_header(lv_obj_t *obj, const char *title, uint32_t accent)
{
    lv_obj_t *lbl = lv_msgbox_add_title(obj, title);
    lv_obj_set_style_text_font(lbl, &bb_font_16, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(COL_TEXT), 0);

    lv_obj_t *header = lv_msgbox_get_header(obj);
    if (!header) return;

    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_set_style_pad_bottom(header, GAP_M, 0);
    lv_obj_set_style_pad_column(header, GAP_M, 0);
    lv_obj_set_height(header, LV_SIZE_CONTENT);

    lv_obj_t *bar = lv_obj_create(header);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(bar, 4, 22);
    lv_obj_set_style_radius(bar, 2, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(accent), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    // Vor den Titel: angelegt wurde er danach, gezeichnet werden soll er
    // davor.
    lv_obj_move_to_index(bar, 0);
}

// Fliesstext des Dialogs: eine Stufe leiser als der Titel. Er erklaert, der
// Titel fragt.
static void add_body(lv_obj_t *obj, const char *text)
{
    if (!text || !text[0]) return;

    lv_obj_t *lbl = lv_msgbox_add_text(obj, text);
    lv_obj_set_style_text_color(lbl, lv_color_hex(COL_MUTED), 0);
    lv_obj_set_style_pad_bottom(lbl, GAP_S, 0);
}

// Inhalt und Fussleiste tragen von LVGL aus eigene Flaechen und Abstaende —
// im Ergebnis drei sichtbare Baender uebereinander. Hier wird daraus eine
// Karte: Die Teile werden durchsichtig, den Rand gibt die Box vor.
//
// Muss nach dem Anlegen der Knoepfe laufen: Die Fussleiste entsteht erst mit
// dem ersten Knopf.
static void polish_parts(lv_obj_t *obj)
{
    lv_obj_t *content = lv_msgbox_get_content(obj);
    if (content) {
        lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(content, 0, 0);
        lv_obj_set_style_pad_all(content, 0, 0);
        lv_obj_set_style_pad_row(content, GAP_S, 0);
    }

    lv_obj_t *footer = lv_msgbox_get_footer(obj);
    if (footer) {
        lv_obj_set_style_bg_opa(footer, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(footer, 0, 0);
        lv_obj_set_style_pad_all(footer, 0, 0);
        lv_obj_set_style_pad_top(footer, GAP_L, 0);
        lv_obj_set_style_pad_column(footer, GAP_S, 0);
        lv_obj_set_height(footer, LV_SIZE_CONTENT);
    }
}

static void clear_dim()
{
    lv_obj_set_style_bg_opa(lv_layer_top(), LV_OPA_TRANSP, 0);
}
static ui_choice_cb_t on_choice_cb = nullptr;
static void *on_ok_data = nullptr;

static void close_box()
{
    if (box) {
        // ASYNCHRON schliessen, nicht sofort: Wir stecken hier im
        // Klick-Callback eines Knopfes, der ein Kind dieser Box ist.
        // lv_msgbox_close() wuerde ihn mitten im Dispatch loeschen — LVGL
        // greift danach auf freigegebenen Speicher zu, und das Geraet
        // startet neu. Genau dieses Muster erklaert Abstuerze "beim
        // Anklicken". lv_msgbox_close_async() haengt das Loeschen hinter
        // die laufende Ereignisverarbeitung.
        lv_msgbox_close_async(box);
        box = nullptr;
        clear_dim();
    }
    on_ok_cb = nullptr;
    on_choice_cb = nullptr;
    on_ok_data = nullptr;
}

static void cancel_cb(lv_event_t *)
{
    close_box();
}

static void ok_cb(lv_event_t *)
{
    // Erst merken, dann schliessen: close_box() raeumt die Zeiger auf, und
    // der Rueckruf darf seinerseits einen neuen Dialog oeffnen.
    ui_confirm_cb_t cb = on_ok_cb;
    void *data = on_ok_data;

    close_box();
    if (cb) cb(data);
}

void ui_confirm(const char *title, const char *text,
                const char *cancel_label,
                const char *ok_label, uint32_t ok_color,
                ui_confirm_cb_t on_ok, void *user_data)
{
    if (box) return; // es laeuft schon eine Rueckfrage

    on_ok_cb = on_ok;
    on_ok_data = user_data;

    ui_watch("dialog:create");
    box = lv_msgbox_create(lv_layer_top());

    // Breite VOR dem Text setzen. Sonst ist die Box beim Einfuegen noch
    // LV_SIZE_CONTENT breit: Der umbrechende Text richtet sich dann nach der
    // Boxbreite, waehrend die Boxbreite sich nach dem Text richtet. Aus
    // dieser gegenseitigen Abhaengigkeit kommt LVGL bei langen Texten nicht
    // mehr heraus — die Oberflaeche steht, ohne abzustuerzen.
    lv_obj_set_width(box, 420);

    add_header(box, title, ok_color);
    add_body(box, text);
    style_box(box);

    // Abbrechen zurueckhaltend, die Handlung farbig: Wer den Dialog
    // ueberfliegt, soll am Farbgewicht sehen, welcher Knopf etwas tut.
    lv_obj_t *no = lv_msgbox_add_footer_button(box, cancel_label);
    lv_obj_set_style_bg_color(no, lv_color_hex(COL_RAISED), 0);
    lv_obj_set_style_text_color(no, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_radius(no, RADIUS_CTRL, 0);
    lv_obj_add_event_cb(no, cancel_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *yes = lv_msgbox_add_footer_button(box, ok_label);
    lv_obj_set_style_bg_color(yes, lv_color_hex(ok_color), 0);
    lv_obj_set_style_text_color(yes, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_radius(yes, RADIUS_CTRL, 0);
    lv_obj_add_event_cb(yes, ok_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_set_height(no, 44);
    lv_obj_set_height(yes, 44);
    polish_parts(box);

    lv_obj_center(box);
    ui_watch("dialog:offen");
}

void ui_info(const char *title, const char *text, const char *close_label)
{
    if (box) return;

    on_ok_cb = nullptr;
    on_ok_data = nullptr;

    box = lv_msgbox_create(lv_layer_top());
    lv_obj_set_width(box, 420); // vor dem Text, siehe ui_confirm()
    add_header(box, title, COL_ACCENT);
    add_body(box, text);
    style_box(box);

    lv_obj_t *close = lv_msgbox_add_footer_button(box, close_label ? close_label : "OK");
    lv_obj_set_style_bg_color(close, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_text_color(close, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_radius(close, RADIUS_CTRL, 0);
    lv_obj_add_event_cb(close, cancel_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_set_height(close, 44);
    polish_parts(box);

    lv_obj_center(box);
}

static void choice_cb(lv_event_t *e)
{
    // Erst merken, dann schliessen: close_box() raeumt die Zeiger auf.
    ui_choice_cb_t cb = on_choice_cb;
    void *data = on_ok_data;
    const int index = (int)(intptr_t)lv_event_get_user_data(e);

    close_box();
    if (cb) cb(index, data);
}

void ui_choice(const char *title, const char *const *options, int count,
               int current, ui_choice_cb_t on_choose, void *user_data)
{
    if (box || !options || count <= 0) return;

    on_choice_cb = on_choose;
    on_ok_data = user_data;

    box = lv_msgbox_create(lv_layer_top());
    lv_obj_set_width(box, 380);
    style_box(box);
    add_header(box, title, COL_ACCENT);

    // Volle Zeilen statt Knoepfe nebeneinander: Auf einem Touchscreen ist
    // ein breites Ziel schneller und sicherer getroffen als vier schmale.
    lv_obj_t *content = lv_msgbox_get_content(box);

    for (int i = 0; i < count; i++) {
        lv_obj_t *btn = lv_button_create(content);
        lv_obj_set_size(btn, LV_PCT(100), 48);
        lv_obj_set_style_radius(btn, RADIUS_CTRL, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(i == current ? COL_ACCENT : COL_RAISED), 0);
        lv_obj_set_style_text_color(btn, lv_color_hex(COL_TEXT), 0);
        lv_obj_add_event_cb(btn, choice_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, options[i]);
        lv_obj_center(lbl);
    }

    // Derselbe zurueckhaltende Knopf wie in der Rueckfrage — die Auswahl
    // selbst steht ja schon als Liste darueber.
    lv_obj_t *cancel = lv_msgbox_add_footer_button(box, "Abbrechen");
    lv_obj_set_style_bg_color(cancel, lv_color_hex(COL_RAISED), 0);
    lv_obj_set_style_text_color(cancel, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_radius(cancel, RADIUS_CTRL, 0);
    lv_obj_set_height(cancel, 44);
    lv_obj_add_event_cb(cancel, cancel_cb, LV_EVENT_CLICKED, nullptr);

    polish_parts(box);
    lv_obj_center(box);
}

bool ui_confirm_is_open()
{
    return box != nullptr;
}

void ui_confirm_close()
{
    close_box();
}
