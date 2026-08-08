#include "jog_screen.h"

#include <Arduino.h>

#include "bambuddy_api.h"
#include "ui_layout.h"
#include "ui_util.h"

static constexpr int PAD = 12;
static constexpr int BUTTON_SIZE = 64;
static constexpr uint32_t TAP_LOCK_MS = 700;

static constexpr uint32_t COL_BUTTON = 0x30375E;
static constexpr uint32_t COL_SELECTED = 0x6D4C41;
static constexpr uint32_t COL_WARN = 0xFFC107;
static constexpr uint32_t COL_MUTED = 0x8492A0;
static constexpr uint32_t COL_ERR = 0xE53935;

enum action_t {
    ACTION_X_LEFT,
    ACTION_X_RIGHT,
    ACTION_Y_UP,
    ACTION_Y_DOWN,
    ACTION_Z_UP,
    ACTION_Z_DOWN,
    ACTION_E_EXTRUDE,
    ACTION_E_RETRACT,
    ACTION_HOME,
};

static lv_obj_t *motion_buttons[9];
static int motion_button_count = 0;
static lv_obj_t *step_buttons[3];
static lv_obj_t *message_lbl = nullptr;
static lv_obj_t *warning_box = nullptr;
static lv_timer_t *ui_timer = nullptr;

static constexpr float STEP_VALUES[] = {1.0f, 10.0f, 50.0f};
static int selected_step = 1;
static uint32_t tap_lock_ms = 0;
static uint32_t local_message_ms = 0;
static char local_message[48] = "";

static void set_enabled(lv_obj_t *button, bool enabled)
{
    if (enabled) {
        lv_obj_remove_state(button, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(button, LV_STATE_DISABLED);
    }
}

static void warning_close()
{
    if (warning_box) {
        lv_msgbox_close(warning_box);
        warning_box = nullptr;
    }
}

static void warning_close_cb(lv_event_t *)
{
    warning_close();
}

static void warning_open_cb(lv_event_t *)
{
    if (warning_box) return;

    warning_box = lv_msgbox_create(lv_layer_top());
    lv_msgbox_add_title(warning_box, LV_SYMBOL_WARNING "  Sicherheitshinweis");
    lv_msgbox_add_text(warning_box,
                       "Manuelle Bewegungen koennen Kollisionen verursachen. "
                       "Das Display erkennt keine Hindernisse. Drucker beobachten "
                       "und zuerst kleine Schritte verwenden.");

    lv_obj_t *close = lv_msgbox_add_footer_button(warning_box, "Verstanden");
    lv_obj_set_style_bg_color(close, lv_color_hex(COL_WARN), 0);
    lv_obj_set_style_text_color(close, lv_color_black(), 0);
    lv_obj_add_event_cb(close, warning_close_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_set_width(warning_box, 420);
    lv_obj_center(warning_box);
}

static bool printer_ready(bool *active_job)
{
    bambuddy_status_t status;
    const bool have_status = bambuddy_api_copy_status(&status);
    *active_job = have_status && bambuddy_api_has_active_job();
    return bambuddy_api_link() == BB_LINK_OK && have_status &&
           status.printer_connected && !*active_job;
}

static void set_local_message(const char *text)
{
    snprintf(local_message, sizeof(local_message), "%s", text ? text : "");
    local_message_ms = millis();
}

static void motion_cb(lv_event_t *event)
{
    if (tap_lock_ms && millis() - tap_lock_ms < TAP_LOCK_MS) return;

    bool active_job;
    if (!printer_ready(&active_job)) return;

    const action_t action = (action_t)(intptr_t)lv_event_get_user_data(event);
    const float step = STEP_VALUES[selected_step];
    bool queued = false;

    switch (action) {
    case ACTION_X_LEFT:
        queued = bambuddy_api_send_xy_jog(-step, 0.0f);
        break;
    case ACTION_X_RIGHT:
        queued = bambuddy_api_send_xy_jog(step, 0.0f);
        break;
    case ACTION_Y_UP:
        queued = bambuddy_api_send_xy_jog(0.0f, step);
        break;
    case ACTION_Y_DOWN:
        queued = bambuddy_api_send_xy_jog(0.0f, -step);
        break;
    case ACTION_Z_UP:
        queued = bambuddy_api_send_z_jog(-step);
        break;
    case ACTION_Z_DOWN:
        queued = bambuddy_api_send_z_jog(step);
        break;
    case ACTION_E_EXTRUDE:
        queued = bambuddy_api_send_extruder_jog(step);
        break;
    case ACTION_E_RETRACT:
        queued = bambuddy_api_send_extruder_jog(-step);
        break;
    case ACTION_HOME:
        queued = bambuddy_api_send_home();
        break;
    }

    if (queued) {
        tap_lock_ms = millis();
    } else {
        set_local_message("Befehlsqueue ist voll");
    }
}

static void update_step_buttons()
{
    for (int i = 0; i < 3; i++) {
        lv_obj_set_style_bg_color(step_buttons[i],
                                  lv_color_hex(i == selected_step ? COL_SELECTED : 0x171C21), 0);
        lv_obj_set_style_text_color(step_buttons[i],
                                    lv_color_hex(i == selected_step ? 0xFF7A1A : COL_MUTED), 0);
    }
}

static void step_cb(lv_event_t *event)
{
    const int index = (int)(intptr_t)lv_event_get_user_data(event);
    if (index < 0 || index >= 3) return;
    selected_step = index;
    update_step_buttons();
}

static void ui_tick_cb(lv_timer_t *)
{
    bool active_job;
    const bool ready = printer_ready(&active_job);
    const bool tap_locked = tap_lock_ms && millis() - tap_lock_ms < TAP_LOCK_MS;

    for (int i = 0; i < motion_button_count; i++) {
        set_enabled(motion_buttons[i], ready && !tap_locked);
    }

    if (local_message_ms && millis() - local_message_ms < 5000) {
        ui_set_text(message_lbl, local_message);
        ui_set_text_color(message_lbl, COL_ERR);
    } else if (active_job) {
        ui_set_text(message_lbl, "Jogging waehrend eines Drucks gesperrt");
        ui_set_text_color(message_lbl, COL_WARN);
    } else if (!ready) {
        ui_set_text(message_lbl, "Drucker nicht verbunden");
        ui_set_text_color(message_lbl, COL_ERR);
    } else if (bambuddy_api_command_message_age() < 5000) {
        ui_set_text(message_lbl, bambuddy_api_command_message());
        ui_set_text_color(message_lbl, COL_MUTED);
    } else {
        ui_set_text(message_lbl, "");
    }
}

static lv_obj_t *motion_button(lv_obj_t *parent, int x, int y, const char *symbol,
                               action_t action)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, BUTTON_SIZE, BUTTON_SIZE);
    lv_obj_align(button, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_set_style_radius(button, 9, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(COL_BUTTON), 0);
    lv_obj_add_event_cb(button, motion_cb, LV_EVENT_CLICKED, (void *)(intptr_t)action);

    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, symbol);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_24, 0);
    lv_obj_center(label);

    if (motion_button_count < 9) motion_buttons[motion_button_count++] = button;
    return button;
}

static void axis_label(lv_obj_t *parent, int x, int y, const char *text)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(COL_MUTED), 0);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, x, y);
}

void jog_screen_create(lv_obj_t *parent)
{
    motion_button_count = 0;
    tap_lock_ms = 0;
    local_message_ms = 0;
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, "Jog-Steuerung");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, PAD + 52, 14);

    lv_obj_t *warning_btn = lv_button_create(parent);
    lv_obj_set_size(warning_btn, 40, 40);
    lv_obj_align(warning_btn, LV_ALIGN_TOP_RIGHT, -PAD, 6);
    lv_obj_set_style_radius(warning_btn, 20, 0);
    lv_obj_set_style_bg_color(warning_btn, lv_color_hex(COL_WARN), 0);
    lv_obj_add_event_cb(warning_btn, warning_open_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *warning_icon = lv_label_create(warning_btn);
    lv_label_set_text(warning_icon, LV_SYMBOL_WARNING);
    lv_obj_set_style_text_color(warning_icon, lv_color_black(), 0);
    lv_obj_center(warning_icon);

    // XY-Kreuz
    motion_button(parent, 112, 80, LV_SYMBOL_UP, ACTION_Y_UP);
    motion_button(parent, 40, 152, LV_SYMBOL_LEFT, ACTION_X_LEFT);
    motion_button(parent, 112, 152, LV_SYMBOL_HOME, ACTION_HOME);
    motion_button(parent, 184, 152, LV_SYMBOL_RIGHT, ACTION_X_RIGHT);
    motion_button(parent, 112, 224, LV_SYMBOL_DOWN, ACTION_Y_DOWN);

    // Z und Extruder
    motion_button(parent, 280, 80, LV_SYMBOL_UP, ACTION_Z_UP);
    motion_button(parent, 280, 224, LV_SYMBOL_DOWN, ACTION_Z_DOWN);
    axis_label(parent, 304, 172, "Z");

    motion_button(parent, 376, 80, LV_SYMBOL_UP, ACTION_E_EXTRUDE);
    motion_button(parent, 376, 224, LV_SYMBOL_DOWN, ACTION_E_RETRACT);
    axis_label(parent, 400, 172, "E");

    lv_obj_t *step_title = lv_label_create(parent);
    lv_label_set_text(step_title, "SCHRITT (MM)");
    lv_obj_set_style_text_font(step_title, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(step_title, lv_color_hex(COL_MUTED), 0);
    lv_obj_align(step_title, LV_ALIGN_TOP_LEFT, 33, 310);

    static const char *step_texts[] = {"1", "10", "50"};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *button = lv_button_create(parent);
        lv_obj_set_size(button, 130, 56);
        lv_obj_align(button, LV_ALIGN_TOP_LEFT, 33 + i * 142, 332);
        lv_obj_set_style_radius(button, 8, 0);
        lv_obj_add_event_cb(button, step_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *label = lv_label_create(button);
        lv_label_set_text(label, step_texts[i]);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
        lv_obj_center(label);
        step_buttons[i] = button;
    }
    update_step_buttons();

    message_lbl = lv_label_create(parent);
    lv_label_set_text(message_lbl, "");
    lv_obj_set_width(message_lbl, SCREEN_W - 2 * PAD);
    lv_label_set_long_mode(message_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(message_lbl, &lv_font_montserrat_12, 0);
    lv_obj_align(message_lbl, LV_ALIGN_BOTTOM_LEFT, PAD + 4, -3);

    ui_timer = lv_timer_create(ui_tick_cb, 250, nullptr);
    lv_timer_set_repeat_count(ui_timer, -1);
    ui_tick_cb(nullptr);
}

void jog_screen_destroy()
{
    warning_close();
    if (ui_timer) {
        lv_timer_delete(ui_timer);
        ui_timer = nullptr;
    }
    message_lbl = nullptr;
    motion_button_count = 0;
    tap_lock_ms = 0;
}
