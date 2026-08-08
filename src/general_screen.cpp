#include "general_screen.h"

#include "jog_screen.h"
#include "settings_screen.h"
#include "smart_plugs_screen.h"
#include "ui_nav.h"
#include "ui_layout.h"
#include "ui_theme.h"
#include "wifi_screen.h"

static constexpr int PAD = 12;
static constexpr uint32_t COL_WIFI = 0x00897B;
static constexpr uint32_t COL_SETTINGS = COL_NEUTRAL; // Kachel "Einstellungen"
static constexpr uint32_t COL_JOG = 0x1565C0;

enum view_t {
    VIEW_HOME,
    VIEW_WIFI,
    VIEW_SETTINGS,
    VIEW_SMART_PLUGS,
    VIEW_JOG,
};

static lv_obj_t *root = nullptr;
static view_t active_view = VIEW_HOME;
static bool transition_pending = false;
static bool screen_visible = false;

// Wurde die Ansicht per Direktsprung vom Statusscreen geoeffnet? Dann fuehrt
// "Zurueck" auch dorthin zurueck und nicht in die Kachelübersicht — sonst
// steht man nach dem Schalten einer Steckdose an einer Stelle, die man gar
// nicht aufgesucht hat.
static bool came_from_status = false;

static void build_home();

// Gibt die Objekte der offenen Unteransicht frei. Muss vor jedem
// lv_obj_clean(root) laufen, egal ob zurueck oder quer gesprungen wird.
static void destroy_active_view()
{
    if (active_view == VIEW_WIFI) {
        wifi_screen_destroy();
    } else if (active_view == VIEW_SETTINGS) {
        settings_screen_destroy();
    } else if (active_view == VIEW_SMART_PLUGS) {
        smart_plugs_screen_destroy();
    } else if (active_view == VIEW_JOG) {
        jog_screen_destroy();
    }
}

static void close_active_view()
{
    if (!root || active_view == VIEW_HOME) return;

    destroy_active_view();

    lv_obj_clean(root);
    active_view = VIEW_HOME;
    build_home();

    if (came_from_status) {
        came_from_status = false;
        ui_nav_status();
    }
}

static void close_async(void *)
{
    transition_pending = false;
    close_active_view();
}

static void back_cb(lv_event_t *)
{
    if (transition_pending) return;
    transition_pending = true;
    lv_async_call(close_async, nullptr);
}

static void add_back_button()
{
    lv_obj_t *btn = lv_button_create(root);
    lv_obj_set_size(btn, 40, 40);
    lv_obj_align(btn, LV_ALIGN_TOP_LEFT, PAD, 6);
    lv_obj_set_style_radius(btn, 20, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(COL_SETTINGS), 0);
    lv_obj_add_event_cb(btn, back_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, LV_SYMBOL_LEFT);
    lv_obj_center(label);
}

static void open_wifi_async(void *)
{
    transition_pending = false;
    if (!screen_visible) return;
    lv_obj_clean(root);
    active_view = VIEW_WIFI;
    wifi_screen_create(root);
    add_back_button();
}

static void open_wifi_cb(lv_event_t *)
{
    if (transition_pending) return;
    transition_pending = true;
    lv_async_call(open_wifi_async, nullptr);
}

static void open_settings_async(void *)
{
    transition_pending = false;
    if (!screen_visible) return;
    lv_obj_clean(root);
    active_view = VIEW_SETTINGS;
    settings_screen_create(root);
    add_back_button();
}

static void open_settings_cb(lv_event_t *)
{
    if (transition_pending) return;
    transition_pending = true;
    lv_async_call(open_settings_async, nullptr);
}

static void open_smart_plugs_async(void *)
{
    transition_pending = false;
    if (!screen_visible || !root) return;

    // War eine andere Unteransicht offen, muss sie ihre Objekte freigeben —
    // sonst zeigen ihre Timer auf geloeschte Labels.
    destroy_active_view();

    lv_obj_clean(root);
    active_view = VIEW_SMART_PLUGS;
    smart_plugs_screen_create(root);
    add_back_button();
}

static void open_smart_plugs_cb(lv_event_t *)
{
    if (transition_pending) return;
    transition_pending = true;
    lv_async_call(open_smart_plugs_async, nullptr);
}

static void open_jog_async(void *)
{
    transition_pending = false;
    if (!screen_visible) return;
    lv_obj_clean(root);
    active_view = VIEW_JOG;
    jog_screen_create(root);
    add_back_button();
}

static void open_jog_cb(lv_event_t *)
{
    if (transition_pending) return;
    transition_pending = true;
    lv_async_call(open_jog_async, nullptr);
}

static lv_obj_t *add_launcher(const char *icon, const char *title, const char *subtitle,
                              uint32_t color, int y, lv_event_cb_t callback)
{
    lv_obj_t *btn = lv_button_create(root);
    lv_obj_set_size(btn, SCREEN_W - 2 * PAD, 82);
    lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_radius(btn, 14, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(color), 0);
    lv_obj_set_style_pad_all(btn, 18, 0);
    lv_obj_add_event_cb(btn, callback, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *icon_lbl = lv_label_create(btn);
    lv_label_set_text(icon_lbl, icon);
    lv_obj_set_style_text_font(icon_lbl, &lv_font_montserrat_24, 0);
    lv_obj_align(icon_lbl, LV_ALIGN_LEFT_MID, 4, 0);

    lv_obj_t *title_lbl = lv_label_create(btn);
    lv_label_set_text(title_lbl, title);
    lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_16, 0);
    lv_obj_align(title_lbl, LV_ALIGN_TOP_LEFT, 54, 6);

    lv_obj_t *sub_lbl = lv_label_create(btn);
    lv_label_set_text(sub_lbl, subtitle);
    lv_obj_set_width(sub_lbl, SCREEN_W - 2 * PAD - 100);
    lv_label_set_long_mode(sub_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(sub_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_opa(sub_lbl, LV_OPA_70, 0);
    lv_obj_align(sub_lbl, LV_ALIGN_TOP_LEFT, 54, 34);

    lv_obj_t *arrow = lv_label_create(btn);
    lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -4, 0);
    return btn;
}

static void build_home()
{
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(root);
    lv_label_set_text(title, LV_SYMBOL_SETTINGS "  System");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, PAD + 4, 14);

    lv_obj_t *hint = lv_label_create(root);
    lv_label_set_text(hint, "Drucker, Verbindung und Display verwalten");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(COL_MUTED), 0);
    lv_obj_align(hint, LV_ALIGN_TOP_LEFT, PAD + 4, 42);

    add_launcher(LV_SYMBOL_WIFI, "WLAN", "Netzwerk suchen und Verbindung verwalten",
                 COL_WIFI, 68, open_wifi_cb);
    add_launcher(LV_SYMBOL_SETTINGS, "Einstellungen", "Bambuddy, MQTT und Darstellung",
                 COL_SETTINGS, 158, open_settings_cb);
    add_launcher(LV_SYMBOL_POWER, "Smart Plugs", "Steckdosen ein- und ausschalten",
                 COL_PLUG, 248, open_smart_plugs_cb);
    add_launcher(LV_SYMBOL_SHUFFLE, "Jog-Steuerung", "Achsen und Extruder manuell bewegen",
                 COL_JOG, 338, open_jog_cb);
}

void general_screen_create(lv_obj_t *parent)
{
    root = parent;
    active_view = VIEW_HOME;
    screen_visible = false;
    build_home();
}

void general_screen_show_smart_plugs()
{
    if (transition_pending) return;
    transition_pending = true;

    // Die Kachel wurde gerade aufgeschlagen; das Sichtbar-Ereignis des
    // Tileviews kann noch ausstehen, deshalb hier selbst setzen.
    screen_visible = true;
    came_from_status = true;
    lv_async_call(open_smart_plugs_async, nullptr);
}

void general_screen_set_visible(bool visible)
{
    // Wischt man selbst weg, ist der Zusammenhang zum Statusscreen weg —
    // ein spaeteres "Zurueck" soll dann nicht dorthin springen.
    if (!visible) came_from_status = false;

    screen_visible = visible;
    if (!visible && active_view != VIEW_HOME && !transition_pending) {
        transition_pending = true;
        lv_async_call(close_async, nullptr);
    }
}
