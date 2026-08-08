#include "ui_dialog.h"

#include "ui_theme.h"
#include "ui_watch.h"

static lv_obj_t *box = nullptr;
static ui_confirm_cb_t on_ok_cb = nullptr;
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

    lv_msgbox_add_title(box, title);

    if (text && text[0]) lv_msgbox_add_text(box, text);

    lv_obj_t *no = lv_msgbox_add_footer_button(box, cancel_label);
    lv_obj_add_event_cb(no, cancel_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *yes = lv_msgbox_add_footer_button(box, ok_label);
    lv_obj_set_style_bg_color(yes, lv_color_hex(ok_color), 0);
    lv_obj_add_event_cb(yes, ok_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_center(box);
    ui_watch("dialog:offen");
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
    lv_msgbox_add_title(box, title);

    // Volle Zeilen statt Knoepfe nebeneinander: Auf einem Touchscreen ist
    // ein breites Ziel schneller und sicherer getroffen als vier schmale.
    lv_obj_t *content = lv_msgbox_get_content(box);

    for (int i = 0; i < count; i++) {
        lv_obj_t *btn = lv_button_create(content);
        lv_obj_set_size(btn, LV_PCT(100), 48);
        lv_obj_set_style_radius(btn, 10, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(i == current ? COL_ACCENT : COL_NEUTRAL), 0);
        lv_obj_add_event_cb(btn, choice_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, options[i]);
        lv_obj_center(lbl);
    }

    lv_obj_t *cancel = lv_msgbox_add_footer_button(box, "Abbrechen");
    lv_obj_add_event_cb(cancel, cancel_cb, LV_EVENT_CLICKED, nullptr);

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
