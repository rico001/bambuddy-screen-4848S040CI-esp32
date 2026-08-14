#include "status_screen.h"

#include <Arduino.h>
#include <string.h>
#include <time.h>

#include <WiFi.h>

#include "bambuddy_api.h"
#include "bambuddy_camera.h"
#include "bambuddy_config.h"
#include "bambuddy_cover.h"
#include "bambuddy_queue.h"
#include "printer_icon.h"
#include "settings_screen.h"
#include "ui_layout.h"
#include "ui_nav.h"
#include "ui_dialog.h"
#include "ui_image_view.h"
#include "ui_theme.h"
#include "ui_util.h"

// ============================================================
// Layout (Hoehe ohne Statusleiste — siehe ui_layout.h)
// ============================================================
static constexpr int PAD = 12;
static constexpr int CONTENT_W = SCREEN_W - 2 * PAD; // 456

// Senkrechtes Budget (CONTENT_H = 414, siehe ui_layout.h):
//   Kopfzeile        0 ..  46
//   Auftragskarte   46 .. 258
//   Temperaturen   266 .. 326
//   Steuerung      334 .. 386
//   Fusszeile      394 .. 408 (unten ausgerichtet)
//
// Die Temperaturkarten sind flacher als frueher: Seit die Navigationsleiste
// unten 40 Pixel belegt, endet die Kachel bei 414 statt 454, und die
// Fusszeile lag sonst ueber den Steuerknoepfen.
static constexpr int JOB_Y = 46;
static constexpr int JOB_H = 212;
static constexpr int TEMP_Y = 266;
static constexpr int TEMP_H = 60;
static constexpr int TEMP_W = (CONTENT_W - 12) / 2;
static constexpr int CTRL_Y = 334;
static constexpr int CTRL_H = 52;
static constexpr int CTRL_GAP = 8;
static constexpr int CTRL_W = (CONTENT_W - 4 * CTRL_GAP) / 5;

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
static lv_obj_t *queue_lbl;

static lv_obj_t *nozzle_value_lbl;
static lv_obj_t *bed_value_lbl;

static lv_obj_t *pause_btn;
static lv_obj_t *resume_btn;
static lv_obj_t *stop_btn;
static lv_obj_t *light_btn;
static lv_obj_t *light_btn_lbl;
static lv_obj_t *speed_btn;
static lv_obj_t *speed_btn_lbl;

static lv_obj_t *message_lbl;
// Fehlertext aus update_link(). Die Fusszeile entscheidet danach, was
// tatsaechlich zu sehen ist — Fehler haben Vorrang vor allem anderen.
static const char *footer_error = nullptr;

// Vollbilder: Kamera-Livebild und Modellansicht
static ui_image_view_t cam_view;
static ui_image_view_t big_view;

static bambuddy_status_t status;
static bool have_status = false;
static lv_timer_t *ui_timer = nullptr;

// Der Drucker bleibt nach einem Abbruch dauerhaft auf FAILED stehen — auch
// nach einem gewollten Stopp. Nach dieser Zeit zeigen wir stattdessen wieder
// den Ruhezustand, sonst klebt "Fehlgeschlagen" bis zum naechsten Druck.
static constexpr uint32_t FAILED_DISPLAY_MS = 30000;
static char seen_state[16] = "";
static uint32_t state_since_ms = 0;
static bool light_switching = false;
static bool light_target_on = false;
static uint32_t light_switching_ms = 0;

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

// Restzeit und, wenn die Uhr steht, die voraussichtliche Fertig-Uhrzeit.
//
// "noch 5 h 12 min" muss man im Kopf auf die Uhr addieren; bei einem langen
// Druck ist genau das die Frage, die man hat. Der Tag steht mit dabei,
// sobald das Ende nicht mehr auf den heutigen faellt — "fertig 07:20" waere
// sonst zwoelf Stunden zu frueh verstanden.
static void format_remaining(int32_t minutes, char *out, size_t out_len)
{
    if (minutes <= 0) {
        if (out_len) out[0] = '\0';
        return;
    }

    char span[32];
    if (minutes < 60) {
        snprintf(span, sizeof(span), "noch %d min", (int)minutes);
    } else {
        snprintf(span, sizeof(span), "noch %d h %02d min", (int)(minutes / 60),
                 (int)(minutes % 60));
    }

    // Ohne NTP stuende dort eine Uhrzeit aus dem Startwert des Chips.
    if (!settings_time_synced()) {
        strncpy(out, span, out_len - 1);
        out[out_len - 1] = '\0';
        return;
    }

    const time_t now = time(nullptr);
    const time_t done = now + (time_t)minutes * 60;

    struct tm tm_done;
    struct tm tm_now;
    localtime_r(&done, &tm_done);
    localtime_r(&now, &tm_now);

    // Nach Kalendertagen unterscheiden, nicht nach 24 Stunden — siehe
    // Fusszeile.
    struct tm midnight = tm_now;
    midnight.tm_hour = 0;
    midnight.tm_min = 0;
    midnight.tm_sec = 0;
    midnight.tm_isdst = -1;
    const time_t today_start = mktime(&midnight);

    char day[12] = "";
    if (done >= today_start + 2 * 86400) {
        // Modulo, damit der Compiler die Laenge belegen kann: Er kennt die
        // Wertebereiche von struct tm nicht und rechnet sonst mit elf
        // Stellen je Zahl.
        snprintf(day, sizeof(day), "%d.%d. ", tm_done.tm_mday % 100,
                 (tm_done.tm_mon + 1) % 100);
    } else if (done >= today_start + 86400) {
        snprintf(day, sizeof(day), "morgen ");
    }

    snprintf(out, out_len, "%s - fertig %s%02d:%02d", span, day, tm_done.tm_hour,
             tm_done.tm_min);
}

// Der Knopf traegt die aktuelle Stufe, angetippt oeffnet er die Auswahl.
static const char *speed_name(int32_t level)
{
    switch (level) {
    case 1:  return "Leise";
    case 2:  return "Normal";
    case 3:  return "Sport";
    case 4:  return "Turbo";
    default: return "Tempo";
    }
}

static void set_temperature(lv_obj_t *label, float value, float target)
{
    if (target > 0.5f) {
        ui_set_text_fmt(label, "%d / %d C", (int)(value + 0.5f), (int)(target + 0.5f));
    } else {
        ui_set_text_fmt(label, "%d C", (int)(value + 0.5f));
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
    footer_error = nullptr;

    const bambuddy_link_t link = bambuddy_api_link();
    const char *error = bambuddy_api_error();

    // WLAN direkt beim Treiber erfragen statt beim Netzwerk-Task: dessen
    // Meldung kann veraltet sein, und dann stuende hier "kein WLAN",
    // obwohl das Display laengst online ist.
    if (WiFi.status() != WL_CONNECTED) {
        set_badge(COL_ERR, LV_SYMBOL_CLOSE "  Kein WLAN");
        footer_error = "Display ist nicht im WLAN.";
        return;
    }

    // Haengt oder starb der Hintergrund-Task, sieht das sonst aus wie ein
    // Serverproblem — und man sucht an der falschen Stelle.
    const uint32_t beat = bambuddy_api_heartbeat();
    if (beat == 0) {
        set_badge(COL_MUTED, LV_SYMBOL_REFRESH "  Startet");
        return;
    }
    if (millis() - beat > 30000) {
        set_badge(COL_ERR, LV_SYMBOL_WARNING "  Dienst haengt");
        footer_error = "Der Netzwerk-Dienst meldet sich nicht mehr.";
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
        footer_error = error;
    }
}

static void update_status_fields()
{
    ui_set_text(name_lbl, status.name[0] ? status.name : "Drucker");

    // Ist der Drucker nicht erreichbar, sind alle Werte von vorhin — der
    // letzte Auftragsname, die letzte Temperatur, der letzte Zustand. Sie
    // stehenzulassen taeuscht Aktualitaet vor, "Unbekannt" oder "k.A."
    // fuellt die Flaeche mit Nichtssagendem. Also leer lassen: Was der
    // Drucker gerade macht, sagt die Badge oben rechts.
    if (!status.printer_connected) {
        ui_set_text(state_lbl, "");
        ui_set_text(job_lbl, "");
        ui_set_text(layer_lbl, "");
        ui_set_text(progress_lbl, "");
        ui_set_text(remaining_lbl, "");
        ui_set_text(queue_lbl, "");

        // Bei den Temperaturen ausdruecklich "k.A." statt leer: Eine leere
        // Karte sieht aus wie ein Anzeigefehler, waehrend die uebrigen
        // Felder als Fliesstext gar nicht erst auffallen, wenn sie fehlen.
        ui_set_text(nozzle_value_lbl, "k.A.");
        ui_set_text(bed_value_lbl, "k.A.");
        if (lv_bar_get_value(progress_bar) != 0) {
            lv_bar_set_value(progress_bar, 0, LV_ANIM_OFF);
        }
        return;
    }

    if (light_switching && status.chamber_light == light_target_on) {
        light_switching = false;
    }

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
        // Reicht fuer "noch 12 h 34 min - fertig morgen 07:20".
        char buf[64];
        format_remaining(status.remaining_min, buf, sizeof(buf));
        ui_set_text(remaining_lbl, buf);
    } else {
        ui_set_text(remaining_lbl, "");
    }

    const int queue_total = bambuddy_queue_total();
    if (queue_total > 0) {
        ui_set_text_fmt(queue_lbl, "%d in Warteschlange", queue_total);
    } else {
        ui_set_text(queue_lbl, "");
    }

    // Ist und Soll in einer Zeile: "22 / 60 C" waehrend geheizt wird,
    // sonst nur der Istwert. Ein Zielwert von 0 bedeutet, dass die Heizung
    // aus ist — das braucht keine eigene Zeile, das Fehlen sagt es schon.
    set_temperature(nozzle_value_lbl, status.nozzle, status.nozzle_target);
    set_temperature(bed_value_lbl, status.bed, status.bed_target);
}

// ============================================================
// Vollbilder
// ============================================================

// Beim Schliessen jeweils den Nachschub abbestellen: ein Kamera-Snapshot
// sind 15 KB alle drei Sekunden, die muessen nicht im Hintergrund
// weiterlaufen, wenn niemand hinsieht.
static void camera_closed()
{
    bambuddy_camera_set_active(false);
}

static void cover_big_closed()
{
    bambuddy_cover_set_big_wanted(false);
}

static void cover_big_open(lv_event_t *)
{
    if (!bambuddy_cover_has_frame()) return; // nichts zu zeigen

    // Schon geladen? Dann direkt zeigen, sonst Hinweis bis das Bild da ist.
    // Geholt wird pro Motiv genau einmal, nicht wiederholt.
    ui_image_view_open(&big_view, COVER_BIG_SIZE, COVER_BIG_SIZE,
                       status.job[0] ? status.job : "Modell",
                       "Modellbild wird geladen ...", cover_big_closed);

    void *frame = nullptr;
    if (bambuddy_cover_has_big_frame() && bambuddy_cover_take_big_frame(&frame) && frame) {
        ui_image_view_set_frame(&big_view, frame, COVER_BIG_SIZE, COVER_BIG_SIZE);
    }

    bambuddy_cover_set_big_wanted(true);
}

static void update_cover_big()
{
    if (!ui_image_view_is_open(&big_view)) return;

    void *frame = nullptr;
    if (bambuddy_cover_take_big_frame(&frame) && frame) {
        ui_image_view_set_frame(&big_view, frame, COVER_BIG_SIZE, COVER_BIG_SIZE);
    }
}

static void camera_open()
{
    ui_image_view_open(&cam_view, CAM_W, CAM_H, "Kamera",
                       "Kamerabild wird geladen ...", camera_closed);
    bambuddy_camera_set_active(true);
}

static void update_camera_overlay()
{
    if (!ui_image_view_is_open(&cam_view)) return;

    void *frame = nullptr;
    if (bambuddy_camera_take_frame(&frame) && frame) {
        ui_image_view_set_frame(&cam_view, frame, CAM_W, CAM_H);
        return;
    }

    const char *error = bambuddy_camera_error();
    if (error[0] && !bambuddy_camera_has_frame()) {
        ui_image_view_set_hint(&cam_view, error, COL_ERR);
    }
}

static void camera_btn_cb(lv_event_t *)
{
    camera_open();
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
    if (light_switching && millis() - light_switching_ms > 10000) {
        light_switching = false;
    }

    const bool reachable = (bambuddy_api_link() == BB_LINK_OK) &&
                           have_status && status.printer_connected;
    const bool running = reachable && strcasecmp(status.state, "RUNNING") == 0;
    const bool paused = reachable && strcasecmp(status.state, "PAUSE") == 0;
    const bool has_job = reachable && bambuddy_api_has_active_job();

    // Die Geschwindigkeit laesst sich nur waehrend eines Drucks aendern.
    set_enabled(speed_btn, running || paused);
    ui_set_text(speed_btn_lbl, speed_name(status.speed_level));

    set_enabled(pause_btn, running);
    set_enabled(resume_btn, paused);
    set_enabled(stop_btn, has_job);
    set_enabled(light_btn, reachable && !light_switching);

    const bool light_on = have_status && status.chamber_light;
    ui_set_text(light_btn_lbl, light_on ? LV_SYMBOL_CHARGE "  Licht aus"
                                       : LV_SYMBOL_CHARGE "  Licht an");
    ui_set_bg_color(light_btn, light_on ? COL_NEUTRAL : COL_WARN);
}

static void stop_confirmed(void *)
{
    bambuddy_api_send_command(BB_CMD_STOP);
}

// Stoppen ist die einzige Aktion, die Arbeit vernichtet — und daneben liegt
// der Pause-Knopf. Deshalb hier eine Rueckfrage, sonst nirgends.
// "Stoppen" statt "Abbrechen": Letzteres liest sich in einem Dialog wie
// "nichts tun" — das Gegenteil dessen, was der Knopf macht.
static void stop_cb(lv_event_t *)
{
    ui_confirm("Druck stoppen?",
               "Der laufende Druck wird gestoppt und kann nicht "
               "fortgesetzt werden.",
               "Weiterdrucken", "Stoppen", COL_ERR,
               stop_confirmed, nullptr);
}

static void speed_chosen(int index, void *)
{
    const int level = index + 1; // Auswahl 0..3 -> Stufe 1..4

    // Sofort mitziehen, damit der Knopf nicht bis zur naechsten Antwort die
    // alte Stufe zeigt. Der naechste Status korrigiert es, falls der
    // Drucker den Wechsel ablehnt.
    status.speed_level = level;
    ui_set_text(speed_btn_lbl, speed_name(level));

    bambuddy_api_send_speed(level);
}

static void plugs_cb(lv_event_t *)
{
    ui_nav_smart_plugs();
}

static void jog_cb(lv_event_t *)
{
    ui_nav_jog();
}

static void speed_cb(lv_event_t *)
{
    static const char *const options[] = {"Leise", "Normal", "Sport", "Turbo"};
    const int current = (status.speed_level >= 1 && status.speed_level <= 4)
                            ? status.speed_level - 1 : -1;

    ui_choice("Druckgeschwindigkeit", options, 4, current, speed_chosen, nullptr);
}

static void pause_cb(lv_event_t *)
{
    bambuddy_api_send_command(BB_CMD_PAUSE);
}

static void resume_cb(lv_event_t *)
{
    bambuddy_api_send_command(BB_CMD_RESUME);
}

static void light_cb(lv_event_t *)
{
    if (light_switching || !have_status) return;

    light_target_on = !status.chamber_light;
    if (bambuddy_api_send_command(light_target_on ? BB_CMD_LIGHT_ON : BB_CMD_LIGHT_OFF)) {
        light_switching = true;
        light_switching_ms = millis();
        update_controls();
    }
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

// Zustand und Auftrag, bei denen der Bildschirm zuletzt geweckt wurde, sowie
// die zuletzt gesehene Plattenfrage.
static char last_wake_state[sizeof(status.state)] = "";
static char last_wake_job[sizeof(status.job)] = "";
static bool last_awaiting_plate = false;

// Wechselt der Drucker den Zustand, ist das genau der Moment, in dem jemand
// hinsehen soll: fertig, angehalten, gescheitert, faengt an. Steht das
// Display dann dunkel im Regal, ist die Nachricht verloren.
//
// Bewusst nur bei einem Wechsel und nicht bei jedem Status: Sonst bliebe der
// Bildschirm waehrend eines ganzen Drucks an.
static void wake_on_state_change()
{
    // Verbindungsluecken sind kein Ereignis. Faellt der Drucker weg, kommt
    // ein leerer Zustand — und beim Wiederkommen derselbe wie vorher.
    if (!status.printer_connected || status.state[0] == '\0') return;

    const bool plate_asked = status.awaiting_plate_clear && !last_awaiting_plate;
    last_awaiting_plate = status.awaiting_plate_clear;

    // Ohne Ruecksicht auf Schreibweise wie ueberall sonst: Der Rohwert kommt
    // vom Drucker durch, und HTTP und MQTT muessen ihn nicht gleich schreiben.
    const bool state_changed = strcasecmp(last_wake_state, status.state) != 0;

    // Zweiter Ausloeser neben dem Zustand: ein anderer Auftragsname. Zwei
    // Auftraege hintereinander durchlaufen zwar dieselbe Folge von Zustaenden,
    // aber haengt die Statusquelle im falschen Moment, kann der Wechsel
    // dazwischen unbemerkt durchrutschen. Der Name aendert sich dann trotzdem.
    // Ein leer werdender Name zaehlt nicht — das ist das Ende, nicht der
    // Anfang von etwas.
    const bool job_changed = status.job[0] && strcmp(last_wake_job, status.job) != 0;

    const bool first = last_wake_state[0] == '\0';

    strncpy(last_wake_state, status.state, sizeof(last_wake_state) - 1);
    last_wake_state[sizeof(last_wake_state) - 1] = '\0';
    strncpy(last_wake_job, status.job, sizeof(last_wake_job) - 1);
    last_wake_job[sizeof(last_wake_job) - 1] = '\0';

    // Der erste Status nach dem Einschalten ist kein Wechsel. Ihn mitzuzaehlen
    // hiesse, die Abschaltzeit beim Start ein zweites Mal zu starten.
    if (first) return;

    if (state_changed || job_changed || plate_asked) settings_screen_wake();
}

static void ui_tick_cb(lv_timer_t *)
{
    if (bambuddy_api_take(&status)) {
        have_status = true;
        wake_on_state_change();
    }

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

    // Eine Fusszeile mit klarer Rangfolge: Ein Fehler verdraengt alles,
    // danach die Rueckmeldung zum letzten Knopfdruck, sonst das Alter der
    // Daten. Zwei getrennte Zeilen dafuer waeren doppelter Platzverbrauch
    // fuer eine Information, die man ohnehin nacheinander liest.
    if (footer_error && footer_error[0]) {
        ui_set_text(message_lbl, footer_error);
        ui_set_text_color(message_lbl, COL_ERR);
        return;
    }

    if (bambuddy_api_command_message_age() < 6000) {
        ui_set_text(message_lbl, bambuddy_api_command_message());
        ui_set_text_color(message_lbl, COL_ACCENT);
        return;
    }

    // Quelle mit anzeigen: per MQTT kommt nur bei Aenderungen etwas rein,
    // da ist ein hohes Alter normal und kein Hinweis auf einen Fehler.
    const char *source = bambuddy_source_mqtt() ? "MQTT" : "HTTP";
    ui_set_text_color(message_lbl, COL_MUTED);

    if (!have_status) {
        ui_set_text_fmt(message_lbl, "%s - warte auf Daten ...", source);
        return;
    }

    const uint32_t age_s = (millis() - status.updated_ms) / 1000;
    if (age_s < 5) {
        ui_set_text_fmt(message_lbl, "%s - gerade aktualisiert", source);
        return;
    }
    if (age_s < 120) {
        ui_set_text_fmt(message_lbl, "%s - vor %d s", source, (int)age_s);
        return;
    }
    if (age_s < 3600) {
        ui_set_text_fmt(message_lbl, "%s - vor %d min", source, (int)(age_s / 60));
        return;
    }

    // Ab einer Stunde ist die Zeitspanne keine Hilfe mehr: "vor 97 min" muss
    // man erst zurueckrechnen. Die Uhrzeit steht direkt da.
    //
    // Nur mit gestellter Uhr — ohne NTP steht sie auf dem Startwert des
    // Chips, und eine falsche Uhrzeit waere schlechter als eine grobe
    // Spanne. Der Tag steht mit dabei, sonst waere "14:32" nicht einzuordnen.
    if (!settings_time_synced()) {
        ui_set_text_fmt(message_lbl, "%s - vor %d h", source, (int)(age_s / 3600));
        return;
    }

    const time_t now = time(nullptr);
    const time_t when = now - (time_t)age_s;

    struct tm tm_when;
    struct tm tm_now;
    localtime_r(&when, &tm_when);
    localtime_r(&now, &tm_now);

    // Nach Kalendertag unterscheiden, nicht nach Stunden. "Weniger als 24
    // Stunden her" heisst nicht "heute": Um ein Uhr nachts liegen zwanzig
    // Stunden zurueck am Vortag, und "heute, 05:00 Uhr" waere schlicht
    // falsch.
    struct tm midnight = tm_now;
    midnight.tm_hour = 0;
    midnight.tm_min = 0;
    midnight.tm_sec = 0;
    midnight.tm_isdst = -1; // Sommerzeit von mktime bestimmen lassen
    const time_t today_start = mktime(&midnight);

    const char *day = "";
    if (when >= today_start) {
        day = "heute";
    } else if (when >= today_start - 86400) {
        day = "gestern";
    }

    if (day[0]) {
        ui_set_text_fmt(message_lbl, "%s - %s %02d:%02d Uhr", source, day,
                        tm_when.tm_hour, tm_when.tm_min);
    } else {
        ui_set_text_fmt(message_lbl, "%s - %02d.%02d. %02d:%02d Uhr", source,
                        tm_when.tm_mday, tm_when.tm_mon + 1, tm_when.tm_hour,
                        tm_when.tm_min);
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
    lv_obj_t *printer_img = lv_image_create(parent);
    lv_image_set_src(printer_img, &printer_icon);
    lv_obj_set_style_image_recolor(printer_img, lv_color_white(), 0);
    lv_obj_set_style_image_recolor_opa(printer_img, LV_OPA_COVER, 0);
    lv_obj_align(printer_img, LV_ALIGN_TOP_LEFT, PAD + 2, 7);

    name_lbl = lv_label_create(parent);
    lv_label_set_text(name_lbl, "Drucker");
    lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_16, 0);
    // Schmaler als frueher: Rechts stehen jetzt zwei Sprungknoepfe, und der
    // Name darf der Badge nicht in die Quere kommen. Zu lange Namen kuerzt
    // LVGL mit Punkten.
    lv_obj_set_width(name_lbl, 150);
    lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_DOT);
    lv_obj_align(name_lbl, LV_ALIGN_TOP_LEFT, PAD + 38, 12);

    // Direktspruenge in die Systemkachel, ganz rechts in der Kopfzeile. Beide
    // Ansichten braucht man, waehrend man vor dem Drucker steht: die
    // Steckdosen meist direkt nach einem Druckende, die Jog-Steuerung beim
    // Aufraeumen der Platte. Dafuer soll man nicht erst zwei Kacheln weiter
    // wischen.
    lv_obj_t *plugs_btn = lv_button_create(parent);
    lv_obj_set_size(plugs_btn, 52, 30);
    lv_obj_align(plugs_btn, LV_ALIGN_TOP_RIGHT, -PAD, 8);
    lv_obj_set_style_radius(plugs_btn, 10, 0);
    lv_obj_set_style_bg_color(plugs_btn, lv_color_hex(COL_PLUG), 0);
    lv_obj_add_event_cb(plugs_btn, plugs_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *plugs_icon = lv_label_create(plugs_btn);
    lv_label_set_text(plugs_icon, LV_SYMBOL_POWER);
    lv_obj_center(plugs_icon);

    lv_obj_t *jog_btn = lv_button_create(parent);
    lv_obj_set_size(jog_btn, 52, 30);
    lv_obj_align(jog_btn, LV_ALIGN_TOP_RIGHT, -(PAD + 52 + 8), 8);
    lv_obj_set_style_radius(jog_btn, 10, 0);
    lv_obj_set_style_bg_color(jog_btn, lv_color_hex(COL_JOG), 0);
    lv_obj_add_event_cb(jog_btn, jog_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *jog_icon = lv_label_create(jog_btn);
    lv_label_set_text(jog_icon, LV_SYMBOL_SHUFFLE);
    lv_obj_center(jog_icon);

    // Fester Abstand vom rechten Rand statt Ausrichtung am Knopf: Die Badge
    // aendert mit dem Text ihre Breite und waechst dann nach links, ohne
    // dass die Position neu berechnet werden muss.
    badge = lv_obj_create(parent);
    // Genauso hoch wie die beiden Sprungknoepfe rechts daneben (52 x 30).
    // Zwei Pixel Unterschied sieht man nicht bewusst, aber die Zeile wirkt
    // dadurch unsauber ausgerichtet.
    lv_obj_set_height(badge, 30);
    lv_obj_set_width(badge, LV_SIZE_CONTENT);
    lv_obj_align(badge, LV_ALIGN_TOP_RIGHT, -(PAD + 2 * (52 + 8)), 8);
    lv_obj_remove_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(badge, 15, 0); // halbe Hoehe: bleibt eine Kapsel
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
    lv_obj_align(remaining_lbl, LV_ALIGN_TOP_LEFT, col_x, 140);

    queue_lbl = muted_label(card, "");
    lv_obj_set_width(queue_lbl, col_w);
    lv_label_set_long_mode(queue_lbl, LV_LABEL_LONG_DOT);
    lv_obj_align(queue_lbl, LV_ALIGN_TOP_LEFT, col_x, 166);

    progress_bar = lv_bar_create(card);
    lv_obj_set_size(progress_bar, CONTENT_W - 28, 16);
    lv_obj_align(progress_bar, LV_ALIGN_TOP_LEFT, 14, 186);
    lv_bar_set_range(progress_bar, 0, 100);
    lv_bar_set_value(progress_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(progress_bar, lv_color_hex(COL_ACCENT), LV_PART_INDICATOR);
}

static void build_temp_card(lv_obj_t *parent, int x, const char *title, uint32_t color,
                            lv_obj_t **value_out)
{
    lv_obj_t *card = card_create(parent, x, TEMP_Y, TEMP_W, TEMP_H);

    lv_obj_t *title_lbl = lv_label_create(card);
    lv_label_set_text(title_lbl, title);
    lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(title_lbl, lv_color_hex(color), 0);
    lv_obj_align(title_lbl, LV_ALIGN_TOP_MID, 0, 7);

    lv_obj_t *value = lv_label_create(card);
    lv_label_set_text(value, "");
    lv_obj_set_style_text_font(value, &lv_font_montserrat_24, 0);
    lv_obj_align(value, LV_ALIGN_TOP_MID, 0, 26);

    *value_out = value;
}

static lv_obj_t *control_button(lv_obj_t *parent, int x, const char *symbol,
                                const char *text, uint32_t color, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, CTRL_W, CTRL_H);
    lv_obj_align(btn, LV_ALIGN_TOP_LEFT, x, CTRL_Y);
    lv_obj_set_style_bg_color(btn, lv_color_hex(color), 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_set_style_pad_hor(btn, 4, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text_fmt(lbl, "%s  %s", symbol, text);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
    lv_obj_center(lbl);

    return btn;
}

static void build_controls(lv_obj_t *parent)
{
    pause_btn = control_button(parent, PAD, LV_SYMBOL_PAUSE, "Pause",
                               COL_NEUTRAL, pause_cb);
    resume_btn = control_button(parent, PAD + CTRL_W + CTRL_GAP, LV_SYMBOL_PLAY, "Start",
                                COL_OK, resume_cb);
    stop_btn = control_button(parent, PAD + 2 * (CTRL_W + CTRL_GAP), LV_SYMBOL_STOP, "Stopp",
                              COL_ERR, stop_cb);
    light_btn = control_button(parent, PAD + 3 * (CTRL_W + CTRL_GAP), LV_SYMBOL_CHARGE,
                               "Licht an", COL_WARN, light_cb);
    light_btn_lbl = lv_obj_get_child(light_btn, 0);

    speed_btn = control_button(parent, PAD + 4 * (CTRL_W + CTRL_GAP), LV_SYMBOL_REFRESH,
                               "Tempo", COL_ACCENT, speed_cb);
    speed_btn_lbl = lv_obj_get_child(speed_btn, 0);
}

void status_screen_create(lv_obj_t *parent)
{
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    build_header(parent);
    build_job_card(parent);
    build_temp_card(parent, PAD, "DUESE", COL_NOZZLE, &nozzle_value_lbl);
    build_temp_card(parent, PAD + TEMP_W + 12, "DRUCKBETT", COL_BED, &bed_value_lbl);

    build_controls(parent);

    message_lbl = lv_label_create(parent);
    ui_set_text(message_lbl, "");
    lv_obj_set_width(message_lbl, CONTENT_W);
    lv_label_set_long_mode(message_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(message_lbl, &lv_font_montserrat_12, 0);
    lv_obj_align(message_lbl, LV_ALIGN_BOTTOM_LEFT, PAD + 4, -6);

    ui_timer = lv_timer_create(ui_tick_cb, 500, nullptr);
    lv_timer_set_repeat_count(ui_timer, -1);
    ui_tick_cb(nullptr);
}
