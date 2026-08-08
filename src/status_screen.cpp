#include "status_screen.h"

#include <Arduino.h>
#include <string.h>

#include <WiFi.h>

#include "bambuddy_api.h"
#include "bambuddy_camera.h"
#include "bambuddy_config.h"
#include "bambuddy_cover.h"
#include "ui_layout.h"
#include "ui_util.h"

// ============================================================
// Layout (Hoehe ohne Statusleiste — siehe ui_layout.h)
// ============================================================
static constexpr int PAD = 12;
static constexpr int CONTENT_W = SCREEN_W - 2 * PAD; // 456

static constexpr int JOB_Y = 46;
static constexpr int JOB_H = 200;
static constexpr int TEMP_Y = 256;
static constexpr int TEMP_H = 90;
static constexpr int TEMP_W = (CONTENT_W - 12) / 2;
static constexpr int CTRL_Y = 356;
static constexpr int CTRL_H = 52;
static constexpr int CTRL_W = (CONTENT_W - 20) / 3;

static constexpr uint32_t COL_MUTED = 0x9E9E9E;
static constexpr uint32_t COL_OK = 0x4CAF50;
static constexpr uint32_t COL_WARN = 0xFFB300;
static constexpr uint32_t COL_ERR = 0xE53935;
static constexpr uint32_t COL_ACCENT = 0x2196F3;
static constexpr uint32_t COL_NOZZLE = 0xFF7043;
static constexpr uint32_t COL_BED = 0x42A5F5;

// ============================================================
// UI-Objekte
// ============================================================
static lv_obj_t *name_lbl;
static lv_obj_t *badge;
static lv_obj_t *badge_lbl;

static lv_obj_t *cover_canvas;
static lv_obj_t *cover_placeholder;
static lv_obj_t *state_lbl;
static lv_obj_t *job_lbl;
static lv_obj_t *layer_lbl;
static lv_obj_t *progress_bar;
static lv_obj_t *progress_lbl;
static lv_obj_t *remaining_lbl;

static lv_obj_t *nozzle_value_lbl;
static lv_obj_t *nozzle_target_lbl;
static lv_obj_t *bed_value_lbl;
static lv_obj_t *bed_target_lbl;

static lv_obj_t *pause_btn;
static lv_obj_t *resume_btn;
static lv_obj_t *stop_btn;
static lv_obj_t *confirm_box = nullptr;

static lv_obj_t *message_lbl;
static lv_obj_t *updated_lbl;

// Vollbilder: Kamera-Livebild und Modellansicht
static lv_obj_t *cam_overlay = nullptr;
static lv_obj_t *cam_canvas = nullptr;
static lv_obj_t *cam_hint = nullptr;

static lv_obj_t *big_overlay = nullptr;
static lv_obj_t *big_canvas = nullptr;
static lv_obj_t *big_hint = nullptr;

static bambuddy_status_t status;
static bool have_status = false;
static lv_timer_t *ui_timer = nullptr;

// Der Drucker bleibt nach einem Abbruch dauerhaft auf FAILED stehen — auch
// nach einem gewollten Stopp. Nach dieser Zeit zeigen wir stattdessen wieder
// den Ruhezustand, sonst klebt "Fehlgeschlagen" bis zum naechsten Druck.
static constexpr uint32_t FAILED_DISPLAY_MS = 30000;
static char seen_state[16] = "";
static uint32_t state_since_ms = 0;

// ============================================================
// Formatierung
// ============================================================

// Der Rohwert kommt vom Drucker durch. Bekannte Werte uebersetzen wir,
// alles andere zeigen wir unveraendert an — lieber ein englisches Wort
// als ein verschlucktes Wort.
static const char *state_text(const char *state, uint32_t *color_out)
{
    struct entry_t {
        const char *raw;
        const char *text;
        uint32_t color;
    };
    static const entry_t table[] = {
        {"RUNNING", "Druckt", COL_ACCENT},
        {"printing", "Druckt", COL_ACCENT},
        {"PAUSE", "Pausiert", COL_WARN},
        {"paused", "Pausiert", COL_WARN},
        {"IDLE", "Bereit", COL_MUTED},
        {"idle", "Bereit", COL_MUTED},
        {"FINISH", "Fertig", COL_OK},
        {"finished", "Fertig", COL_OK},
        {"FAILED", "Fehlgeschlagen", COL_ERR},
        {"failed", "Fehlgeschlagen", COL_ERR},
        {"PREPARE", "Bereitet vor", COL_ACCENT},
        {"SLICING", "Slicing", COL_ACCENT},
    };

    for (const entry_t &e : table) {
        if (strcasecmp(state, e.raw) == 0) {
            *color_out = e.color;
            return e.text;
        }
    }

    *color_out = COL_MUTED;
    return state[0] ? state : "Unbekannt";
}

static void format_remaining(int32_t minutes, char *out, size_t out_len)
{
    if (minutes <= 0) {
        strncpy(out, "", out_len);
        return;
    }
    if (minutes < 60) {
        snprintf(out, out_len, "noch %d min", (int)minutes);
    } else {
        snprintf(out, out_len, "noch %d h %02d min", (int)(minutes / 60), (int)(minutes % 60));
    }
}

// ============================================================
// Aktualisierung
// ============================================================

static void set_badge(uint32_t color, const char *text)
{
    const char *current = lv_label_get_text(badge_lbl);
    if (current && strcmp(current, text) == 0) {
        ui_set_bg_color(badge, color);
        return;
    }

    ui_set_bg_color(badge, color);
    lv_label_set_text(badge_lbl, text);
    lv_obj_set_width(badge, LV_SIZE_CONTENT);
}

static void update_link()
{
    const bambuddy_link_t link = bambuddy_api_link();
    const char *error = bambuddy_api_error();

    // WLAN direkt beim Treiber erfragen statt beim Netzwerk-Task: dessen
    // Meldung kann veraltet sein, und dann stuende hier "kein WLAN",
    // obwohl das Display laengst online ist.
    if (WiFi.status() != WL_CONNECTED) {
        set_badge(COL_ERR, LV_SYMBOL_CLOSE "  Kein WLAN");
        ui_set_text(message_lbl, "Display ist nicht im WLAN.");
        ui_set_text_color(message_lbl, COL_ERR);
        return;
    }

    // Haengt oder starb der Hintergrund-Task, sieht das sonst aus wie ein
    // Serverproblem — und man sucht an der falschen Stelle.
    const uint32_t beat = bambuddy_api_heartbeat();
    if (beat == 0) {
        set_badge(COL_MUTED, LV_SYMBOL_REFRESH "  Startet");
        ui_set_text(message_lbl, "");
        return;
    }
    if (millis() - beat > 30000) {
        set_badge(COL_ERR, LV_SYMBOL_WARNING "  Dienst haengt");
        ui_set_text(message_lbl, "Der Netzwerk-Dienst meldet sich nicht mehr.");
        ui_set_text_color(message_lbl, COL_ERR);
        return;
    }

    // Drei Ebenen, drei Aussagen — "offline" allein wuerde verschweigen,
    // ob das Display, der Server oder der Drucker das Problem ist.
    switch (link) {
    case BB_LINK_OK:
        if (have_status && status.printer_connected) {
            set_badge(COL_OK, LV_SYMBOL_OK "  Verbunden");
        } else {
            set_badge(COL_WARN, LV_SYMBOL_WARNING "  Drucker offline");
        }
        break;
    case BB_LINK_STARTING:
        set_badge(COL_MUTED, LV_SYMBOL_REFRESH "  Startet");
        break;
    case BB_LINK_NO_WIFI:
        set_badge(COL_WARN, LV_SYMBOL_REFRESH "  Verbindet");
        break;
    case BB_LINK_NO_CONFIG:
        set_badge(COL_WARN, LV_SYMBOL_SETTINGS "  Nicht konfiguriert");
        break;
    case BB_LINK_UNAUTHORIZED:
        set_badge(COL_ERR, LV_SYMBOL_WARNING "  API-Key");
        break;
    case BB_LINK_NO_SERVER:
    default:
        set_badge(COL_ERR, LV_SYMBOL_CLOSE "  Offline");
        break;
    }

    // Kein Hinweistext fuer "Drucker offline" — das steht schon in der
    // Badge oben rechts, zweimal dasselbe ist nur Rauschen.
    if (error[0]) {
        ui_set_text(message_lbl, error);
        ui_set_text_color(message_lbl, COL_ERR);
    } else {
        ui_set_text(message_lbl, "");
    }
}

static void update_status_fields()
{
    ui_set_text(name_lbl, status.name[0] ? status.name : "Drucker");

    // Zeitpunkt des letzten Zustandswechsels merken
    if (strcmp(seen_state, status.state) != 0) {
        strncpy(seen_state, status.state, sizeof(seen_state) - 1);
        seen_state[sizeof(seen_state) - 1] = '\0';
        state_since_ms = millis();
    }

    // Ein abgelaufenes "Fehlgeschlagen" wie Ruhezustand behandeln — samt
    // aller Reste des alten Auftrags, die sonst stehen blieben.
    const bool failed = strcasecmp(status.state, "FAILED") == 0 ||
                        strcasecmp(status.state, "failed") == 0;
    const bool stale_failure = failed && (millis() - state_since_ms) > FAILED_DISPLAY_MS;
    const bool job_active = bambuddy_api_has_active_job();

    uint32_t state_color;
    const char *text = stale_failure ? state_text("IDLE", &state_color)
                                     : state_text(status.state, &state_color);
    ui_set_text(state_lbl, text);
    ui_set_text_color(state_lbl, state_color);

    const bool show_job = status.job[0] && !stale_failure;
    ui_set_text(job_lbl, show_job ? status.job : "Kein Auftrag");

    // Schichtzaehler nur bei laufendem Auftrag: nach Abbruch oder Ende
    // beschreibt er nichts mehr, was gerade passiert.
    if (job_active && status.total_layers > 0) {
        ui_set_text_fmt(layer_lbl, "Schicht %d von %d",
                        (int)status.layer, (int)status.total_layers);
    } else {
        ui_set_text(layer_lbl, "");
    }

    // Balken nur bei echter Aenderung anfassen: lv_bar_set_value startet
    // sonst bei jedem Tick eine neue Animation und zeichnet dauerhaft neu.
    const int progress = (int)(status.progress + 0.5f);
    if (lv_bar_get_value(progress_bar) != progress) {
        lv_bar_set_value(progress_bar, progress, LV_ANIM_ON);
    }

    const bool printing = bambuddy_api_is_printing();
    if (printing || progress > 0) {
        ui_set_text_fmt(progress_lbl, "%d %%", progress);
        ui_set_text_color(progress_lbl, COL_ACCENT);
    } else {
        // Nichts anzuzeigen heisst nichts anzeigen: Zustand und leerer
        // Balken sagen bereits, dass gerade nicht gedruckt wird.
        ui_set_text(progress_lbl, "");
    }

    // Restzeit nur bei laufendem Auftrag. Nach einem Abbruch liefert die API
    // den letzten Wert weiter — der zaehlt dann nicht mehr runter und waere
    // schlicht falsch.
    if (job_active) {
        char buf[32];
        format_remaining(status.remaining_min, buf, sizeof(buf));
        ui_set_text(remaining_lbl, buf);
    } else {
        ui_set_text(remaining_lbl, "");
    }

    ui_set_text_fmt(nozzle_value_lbl, "%d C", (int)(status.nozzle + 0.5f));
    if (status.nozzle_target > 0.5f) {
        ui_set_text_fmt(nozzle_target_lbl, "Ziel %d C", (int)(status.nozzle_target + 0.5f));
    } else {
        ui_set_text(nozzle_target_lbl, "aus");
    }

    ui_set_text_fmt(bed_value_lbl, "%d C", (int)(status.bed + 0.5f));
    if (status.bed_target > 0.5f) {
        ui_set_text_fmt(bed_target_lbl, "Ziel %d C", (int)(status.bed_target + 0.5f));
    } else {
        ui_set_text(bed_target_lbl, "aus");
    }
}

// ============================================================
// Kamera-Vollbild
// ============================================================

static void camera_close();

// Tippen irgendwo auf das Vollbild schliesst es wieder — dieselbe Geste,
// mit der es geoeffnet wurde, ohne einen zusaetzlichen Schliessen-Knopf.
static void camera_overlay_cb(lv_event_t *)
{
    camera_close();
}

// Gemeinsames Geruest fuer beide Vollbilder: schwarze Flaeche ueber allem,
// mittig das Bild, Tippen schliesst.
static lv_obj_t *fullscreen_create(lv_event_cb_t close_cb, lv_obj_t **canvas_out,
                                   lv_obj_t **hint_out, int canvas_w, int canvas_h)
{
    lv_obj_t *overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(overlay, SCREEN_W, SCREEN_H);
    lv_obj_align(overlay, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(overlay, 0, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_pad_all(overlay, 0, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(overlay, close_cb, LV_EVENT_CLICKED, nullptr);

    // Bewusst ohne lv_image_set_scale: Skalieren wuerde LVGL bei jeder
    // Neuzeichnung erneut rechnen lassen. Die Bilder kommen deshalb schon
    // in Anzeigegroesse aus dem Decoder.
    lv_obj_t *canvas = lv_canvas_create(overlay);
    lv_obj_set_size(canvas, canvas_w, canvas_h);
    lv_obj_center(canvas);
    lv_obj_add_flag(canvas, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *hint = lv_label_create(overlay);
    lv_obj_set_style_text_color(hint, lv_color_hex(COL_MUTED), 0);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(hint);

    *canvas_out = canvas;
    *hint_out = hint;
    return overlay;
}

// ============================================================
// Modell-Vollbild
// ============================================================

static void cover_big_close()
{
    bambuddy_cover_set_big_wanted(false);
    if (big_overlay) lv_obj_add_flag(big_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void cover_big_overlay_cb(lv_event_t *)
{
    cover_big_close();
}

static void cover_big_open(lv_event_t *)
{
    if (!bambuddy_cover_has_frame()) return; // nichts zu zeigen

    if (!big_overlay) {
        big_overlay = fullscreen_create(cover_big_overlay_cb, &big_canvas, &big_hint,
                                        COVER_BIG_SIZE, COVER_BIG_SIZE);
    }

    // Schon geladen? Dann direkt zeigen, sonst Hinweis bis das Bild da ist.
    // Geholt wird pro Auftrag genau einmal, nicht wiederholt.
    if (bambuddy_cover_has_big_frame()) {
        lv_obj_remove_flag(big_canvas, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(big_hint, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(big_hint, "Modellbild wird geladen ...");
        lv_obj_remove_flag(big_hint, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(big_canvas, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_remove_flag(big_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(big_overlay);

    bambuddy_cover_set_big_wanted(true);
}

static void update_cover_big()
{
    if (!big_overlay || lv_obj_has_flag(big_overlay, LV_OBJ_FLAG_HIDDEN)) return;

    void *frame = nullptr;
    if (bambuddy_cover_take_big_frame(&frame) && frame) {
        lv_canvas_set_buffer(big_canvas, frame, COVER_BIG_SIZE, COVER_BIG_SIZE,
                             LV_COLOR_FORMAT_RGB565);
        lv_obj_remove_flag(big_canvas, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(big_hint, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(big_canvas);
    }
}

static void camera_open()
{
    if (!cam_overlay) {
        cam_overlay = fullscreen_create(camera_overlay_cb, &cam_canvas, &cam_hint,
                                        CAM_W, CAM_H);
    }

    lv_label_set_text(cam_hint, "Kamerabild wird geladen ...");
    lv_obj_remove_flag(cam_hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(cam_canvas, LV_OBJ_FLAG_HIDDEN);

    lv_obj_remove_flag(cam_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(cam_overlay);

    bambuddy_camera_set_active(true);
}

static void camera_close()
{
    // Abschalten, sobald es zu ist: ein Snapshot sind 15 KB, die muessen
    // nicht im Hintergrund weiterlaufen.
    bambuddy_camera_set_active(false);
    if (cam_overlay) lv_obj_add_flag(cam_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void camera_btn_cb(lv_event_t *)
{
    camera_open();
}

static void update_camera_overlay()
{
    if (!cam_overlay || !bambuddy_camera_active()) return;

    void *frame = nullptr;
    if (bambuddy_camera_take_frame(&frame) && frame) {
        lv_canvas_set_buffer(cam_canvas, frame, CAM_W, CAM_H, LV_COLOR_FORMAT_RGB565);
        lv_obj_remove_flag(cam_canvas, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(cam_hint, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(cam_canvas);
        return;
    }

    const char *error = bambuddy_camera_error();
    if (error[0] && !bambuddy_camera_has_frame()) {
        ui_set_text(cam_hint, error);
        ui_set_text_color(cam_hint, COL_ERR);
    }
}

// ============================================================
// Steuerung
// ============================================================

static void set_enabled(lv_obj_t *btn, bool enabled)
{
    if (enabled) {
        lv_obj_remove_state(btn, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(btn, LV_STATE_DISABLED);
    }
}

// Knoepfe bleiben immer sichtbar und wechseln nur den Zustand — ein Layout,
// in dem Knoepfe auftauchen und verschwinden, laedt zu Fehlgriffen ein.
static void update_controls()
{
    const bool reachable = (bambuddy_api_link() == BB_LINK_OK) &&
                           have_status && status.printer_connected;
    const bool running = reachable && strcasecmp(status.state, "RUNNING") == 0;
    const bool paused = reachable && strcasecmp(status.state, "PAUSE") == 0;
    const bool has_job = reachable && bambuddy_api_has_active_job();

    set_enabled(pause_btn, running);
    set_enabled(resume_btn, paused);
    set_enabled(stop_btn, has_job);
}

static void confirm_close()
{
    if (confirm_box) {
        lv_msgbox_close(confirm_box);
        confirm_box = nullptr;
    }
}

static void confirm_no_cb(lv_event_t *)
{
    confirm_close();
}

static void confirm_yes_cb(lv_event_t *)
{
    confirm_close();
    bambuddy_api_send_command(BB_CMD_STOP);
}

// Abbrechen ist die einzige Aktion, die Arbeit vernichtet — und daneben
// liegt der Pause-Knopf. Deshalb hier eine Rueckfrage, sonst nirgends.
static void stop_cb(lv_event_t *)
{
    if (confirm_box) return;

    confirm_box = lv_msgbox_create(lv_layer_top());
    lv_msgbox_add_title(confirm_box, "Druck stoppen?");
    lv_msgbox_add_text(confirm_box,
                       "Der laufende Druck wird gestoppt und kann nicht "
                       "fortgesetzt werden.");

    lv_obj_t *no = lv_msgbox_add_footer_button(confirm_box, "Weiterdrucken");
    lv_obj_add_event_cb(no, confirm_no_cb, LV_EVENT_CLICKED, nullptr);

    // Bewusst nicht "Abbrechen": das liest sich in einem Dialog wie
    // "nichts tun" — genau das Gegenteil dessen, was der Knopf macht.
    lv_obj_t *yes = lv_msgbox_add_footer_button(confirm_box, "Stoppen");
    lv_obj_set_style_bg_color(yes, lv_color_hex(COL_ERR), 0);
    lv_obj_add_event_cb(yes, confirm_yes_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_set_width(confirm_box, 400);
    lv_obj_center(confirm_box);
}

static void pause_cb(lv_event_t *)
{
    bambuddy_api_send_command(BB_CMD_PAUSE);
}

static void resume_cb(lv_event_t *)
{
    bambuddy_api_send_command(BB_CMD_RESUME);
}

static void update_cover()
{
    // Kein Auftrag auf dem Drucker: Platzhalter zeigen, damit nicht das
    // Modell des letzten Drucks stehen bleibt.
    if (!bambuddy_cover_has_frame()) {
        lv_obj_add_flag(cover_canvas, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(cover_placeholder, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    void *frame = nullptr;
    if (bambuddy_cover_take_frame(&frame) && frame) {
        lv_canvas_set_buffer(cover_canvas, frame, COVER_SIZE, COVER_SIZE, LV_COLOR_FORMAT_RGB565);
        lv_obj_remove_flag(cover_canvas, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(cover_placeholder, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(cover_canvas);
    }
}

static void ui_tick_cb(lv_timer_t *)
{
    if (bambuddy_api_take(&status)) have_status = true;

    // Jeden Tick auswerten, nicht nur bei neuen Daten: per MQTT kommt nach
    // einem Abbruch nichts mehr nach, die Anzeige muss sich aber trotzdem
    // nach 30 Sekunden umstellen. Dank der Aenderungspruefung in ui_util.h
    // kostet ein Durchlauf ohne Aenderung nichts.
    if (have_status) update_status_fields();

    update_link();
    update_cover();
    update_controls();
    update_camera_overlay();
    update_cover_big();

    // Rueckmeldung zum letzten Knopfdruck kurz einblenden — sonst weiss man
    // nicht, ob der Befehl ueberhaupt rausgegangen ist.
    if (bambuddy_api_command_message_age() < 6000) {
        ui_set_text(message_lbl, bambuddy_api_command_message());
        ui_set_text_color(message_lbl, COL_ACCENT);
    }

    // Quelle mit anzeigen: per MQTT kommt nur bei Aenderungen etwas rein,
    // da ist ein hohes Alter normal und kein Hinweis auf einen Fehler.
    const char *source = bambuddy_source_mqtt() ? "MQTT" : "HTTP";

    if (have_status) {
        const uint32_t age_s = (millis() - status.updated_ms) / 1000;
        if (age_s < 5) {
            ui_set_text_fmt(updated_lbl, "%s - gerade aktualisiert", source);
        } else if (age_s < 120) {
            ui_set_text_fmt(updated_lbl, "%s - vor %d s", source, (int)age_s);
        } else {
            ui_set_text_fmt(updated_lbl, "%s - vor %d min", source, (int)(age_s / 60));
        }
    } else {
        ui_set_text_fmt(updated_lbl, "%s - warte auf Daten ...", source);
    }
}

// ============================================================
// Aufbau
// ============================================================

static lv_obj_t *card_create(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, w, h);
    lv_obj_align(card, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    return card;
}

static lv_obj_t *muted_label(lv_obj_t *parent, const char *text)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(COL_MUTED), 0);
    return lbl;
}

static void build_header(lv_obj_t *parent)
{
    name_lbl = lv_label_create(parent);
    lv_label_set_text(name_lbl, "Drucker");
    lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_width(name_lbl, 250);
    lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_DOT);
    lv_obj_align(name_lbl, LV_ALIGN_TOP_LEFT, PAD + 4, 12);

    badge = lv_obj_create(parent);
    lv_obj_set_height(badge, 28);
    lv_obj_set_width(badge, LV_SIZE_CONTENT);
    lv_obj_align(badge, LV_ALIGN_TOP_RIGHT, -PAD, 8);
    lv_obj_remove_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(badge, 15, 0);
    lv_obj_set_style_border_width(badge, 0, 0);
    lv_obj_set_style_pad_hor(badge, 12, 0);
    lv_obj_set_style_pad_ver(badge, 0, 0);

    badge_lbl = lv_label_create(badge);
    lv_label_set_text(badge_lbl, "...");
    lv_obj_set_style_text_font(badge_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(badge_lbl, lv_color_white(), 0);
    lv_obj_center(badge_lbl);
}

static void build_job_card(lv_obj_t *parent)
{
    lv_obj_t *card = card_create(parent, PAD, JOB_Y, CONTENT_W, JOB_H);

    // Bildbereich: Platzhalter und Modellbild liegen deckungsgleich.
    // Der Platzhalter traegt dieselbe Farbe, gegen die das PNG beim
    // Dekodieren freigestellt wird — dadurch gibt es keine sichtbare Kante.
    cover_placeholder = lv_obj_create(card);
    lv_obj_set_size(cover_placeholder, COVER_SIZE, COVER_SIZE);
    lv_obj_align(cover_placeholder, LV_ALIGN_TOP_LEFT, 14, 12);
    lv_obj_remove_flag(cover_placeholder, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(cover_placeholder, 8, 0);
    lv_obj_set_style_border_width(cover_placeholder, 0, 0);
    lv_obj_set_style_bg_opa(cover_placeholder, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(cover_placeholder, lv_color_hex(COVER_BG), 0);

    lv_obj_t *ph_icon = lv_label_create(cover_placeholder);
    lv_label_set_text(ph_icon, LV_SYMBOL_IMAGE);
    lv_obj_set_style_text_color(ph_icon, lv_color_hex(COL_MUTED), 0);
    lv_obj_center(ph_icon);

    cover_canvas = lv_canvas_create(card);
    lv_obj_set_size(cover_canvas, COVER_SIZE, COVER_SIZE);
    lv_obj_align(cover_canvas, LV_ALIGN_TOP_LEFT, 14, 12);
    lv_obj_add_flag(cover_canvas, LV_OBJ_FLAG_HIDDEN);

    // Tippen aufs Modell zeigt es gross — wie beim Kamerabild
    lv_obj_add_flag(cover_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(cover_canvas, cover_big_open, LV_EVENT_CLICKED, nullptr);

    // Kamera-Knopf in der Ecke des Bildes — dort, wo man das Livebild
    // erwartet, und ohne der Steuerungsreihe Platz wegzunehmen.
    lv_obj_t *cam_btn = lv_button_create(card);
    lv_obj_set_size(cam_btn, 40, 34);
    lv_obj_align(cam_btn, LV_ALIGN_TOP_LEFT, 14 + COVER_SIZE - 46, 12 + COVER_SIZE - 40);
    lv_obj_set_style_radius(cam_btn, 8, 0);
    lv_obj_set_style_bg_color(cam_btn, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(cam_btn, LV_OPA_60, 0);
    lv_obj_set_style_shadow_width(cam_btn, 0, 0);
    lv_obj_add_event_cb(cam_btn, camera_btn_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *cam_icon = lv_label_create(cam_btn);
    lv_label_set_text(cam_icon, LV_SYMBOL_VIDEO);
    lv_obj_set_style_text_color(cam_icon, lv_color_white(), 0);
    lv_obj_center(cam_icon);

    // Rechte Spalte
    const int col_x = 14 + COVER_SIZE + 16;
    const int col_w = CONTENT_W - col_x - 14;

    state_lbl = lv_label_create(card);
    lv_label_set_text(state_lbl, "...");
    lv_obj_set_style_text_font(state_lbl, &lv_font_montserrat_24, 0);
    lv_obj_align(state_lbl, LV_ALIGN_TOP_LEFT, col_x, 12);

    job_lbl = muted_label(card, "");
    lv_obj_set_width(job_lbl, col_w);
    lv_label_set_long_mode(job_lbl, LV_LABEL_LONG_DOT);
    lv_obj_align(job_lbl, LV_ALIGN_TOP_LEFT, col_x, 48);

    layer_lbl = muted_label(card, "");
    lv_obj_set_width(layer_lbl, col_w);
    lv_obj_align(layer_lbl, LV_ALIGN_TOP_LEFT, col_x, 72);

    progress_lbl = lv_label_create(card);
    lv_label_set_text(progress_lbl, "");
    lv_obj_set_style_text_font(progress_lbl, &lv_font_montserrat_24, 0);
    lv_obj_align(progress_lbl, LV_ALIGN_TOP_LEFT, col_x, 98);

    remaining_lbl = lv_label_create(card);
    lv_label_set_text(remaining_lbl, "");
    lv_obj_set_width(remaining_lbl, col_w);
    lv_label_set_long_mode(remaining_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(remaining_lbl, lv_color_hex(COL_MUTED), 0);
    lv_obj_align(remaining_lbl, LV_ALIGN_TOP_LEFT, col_x, 134);

    progress_bar = lv_bar_create(card);
    lv_obj_set_size(progress_bar, CONTENT_W - 28, 16);
    lv_obj_align(progress_bar, LV_ALIGN_TOP_LEFT, 14, 160);
    lv_bar_set_range(progress_bar, 0, 100);
    lv_bar_set_value(progress_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(progress_bar, lv_color_hex(COL_ACCENT), LV_PART_INDICATOR);
}

static void build_temp_card(lv_obj_t *parent, int x, const char *title, uint32_t color,
                            lv_obj_t **value_out, lv_obj_t **target_out)
{
    lv_obj_t *card = card_create(parent, x, TEMP_Y, TEMP_W, TEMP_H);

    lv_obj_t *title_lbl = lv_label_create(card);
    lv_label_set_text(title_lbl, title);
    lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(title_lbl, lv_color_hex(color), 0);
    lv_obj_align(title_lbl, LV_ALIGN_TOP_MID, 0, 12);

    lv_obj_t *value = lv_label_create(card);
    lv_label_set_text(value, "");
    lv_obj_set_style_text_font(value, &lv_font_montserrat_24, 0);
    lv_obj_align(value, LV_ALIGN_TOP_MID, 0, 34);

    lv_obj_t *target = muted_label(card, "");
    lv_obj_align(target, LV_ALIGN_BOTTOM_MID, 0, -14);

    *value_out = value;
    *target_out = target;
}

static lv_obj_t *control_button(lv_obj_t *parent, int x, const char *symbol,
                                const char *text, uint32_t color, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, CTRL_W, CTRL_H);
    lv_obj_align(btn, LV_ALIGN_TOP_LEFT, x, CTRL_Y);
    lv_obj_set_style_bg_color(btn, lv_color_hex(color), 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text_fmt(lbl, "%s  %s", symbol, text);
    lv_obj_center(lbl);

    return btn;
}

static void build_controls(lv_obj_t *parent)
{
    pause_btn = control_button(parent, PAD, LV_SYMBOL_PAUSE, "Pause",
                               0x546E7A, pause_cb);
    resume_btn = control_button(parent, PAD + CTRL_W + 10, LV_SYMBOL_PLAY, "Start",
                                COL_OK, resume_cb);
    stop_btn = control_button(parent, PAD + 2 * (CTRL_W + 10), LV_SYMBOL_STOP, "Stopp",
                              COL_ERR, stop_cb);
}

void status_screen_create(lv_obj_t *parent)
{
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    build_header(parent);
    build_job_card(parent);
    build_temp_card(parent, PAD, "DUESE", COL_NOZZLE, &nozzle_value_lbl, &nozzle_target_lbl);
    build_temp_card(parent, PAD + TEMP_W + 12, "DRUCKBETT", COL_BED, &bed_value_lbl, &bed_target_lbl);

    build_controls(parent);

    message_lbl = lv_label_create(parent);
    ui_set_text(message_lbl, "");
    lv_obj_set_width(message_lbl, CONTENT_W);
    lv_label_set_long_mode(message_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(message_lbl, &lv_font_montserrat_12, 0);
    lv_obj_align(message_lbl, LV_ALIGN_TOP_LEFT, PAD + 4, CTRL_Y + CTRL_H + 4);

    updated_lbl = muted_label(parent, "warte auf Daten ...");
    lv_obj_align(updated_lbl, LV_ALIGN_BOTTOM_LEFT, PAD + 4, -6);

    ui_timer = lv_timer_create(ui_tick_cb, 500, nullptr);
    lv_timer_set_repeat_count(ui_timer, -1);
    ui_tick_cb(nullptr);
}
