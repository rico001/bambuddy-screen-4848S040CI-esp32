#include "wifi_screen.h"

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <string.h>

#include "secrets.h"
#include "ui_layout.h"
#include "ui_kit.h"
#include "ui_theme.h"
#include "ui_font.h"

// ============================================================
// Layout & Farben
// ============================================================
static constexpr int PAD = 12;
static constexpr int HEADER_H = 44;
static constexpr int CARD_Y = 48;
static constexpr int CARD_H = 62;
static constexpr int CONTENT_Y = CARD_Y + CARD_H + 10; // 120
static constexpr int CONTENT_W = SCREEN_W - 2 * PAD;   // 456
// Volle Displayhoehe: Die Ansicht liegt als Vollbild ueber den Kacheln.
static constexpr int VIEW_H = SCREEN_H - CONTENT_Y - PAD;
static constexpr int KEYBOARD_H = 190;
static constexpr int ROW_H = 52; // Touch-Ziel: nie unter 44px


// ============================================================
// State Machine
// ============================================================
enum wifi_state_t {
    WS_IDLE,        // offline, nichts laeuft
    WS_SCANNING,    // Scan laeuft
    WS_LIST,        // Scan-Ergebnisse, User waehlt
    WS_PASSWORD,    // Passwort-Eingabe fuer gewaehltes Netz
    WS_CONNECTING,  // Verbindungsversuch laeuft
    WS_CONNECTED,   // verbunden
    WS_RETRY_WAIT,  // Auto-Reconnect: Wartezeit bis zum naechsten Versuch
};

static wifi_state_t state = WS_IDLE;
static bool service_started = false;
static bool ui_ready = false;

// ============================================================
// UI-Objekte
// ============================================================
static lv_obj_t *root;
static lv_obj_t *scan_btn;
static lv_obj_t *scan_btn_lbl;

// Statuskarte (immer sichtbar — einzige Quelle fuer "was passiert gerade")
static lv_obj_t *card;
static lv_obj_t *card_icon;
static lv_obj_t *card_title;
static lv_obj_t *card_sub;

// View: Netzwerkliste
static lv_obj_t *view_list;
static lv_obj_t *net_list;
static lv_obj_t *list_hint;
static lv_obj_t *list_spinner;

// View: Passwort
static lv_obj_t *view_password;
static lv_obj_t *pw_ssid_lbl;
static lv_obj_t *pw_ta;
static lv_obj_t *pw_eye_lbl;
static lv_obj_t *pw_error_lbl;
static lv_obj_t *keyboard;

// View: verbunden
static lv_obj_t *view_connected;
static lv_obj_t *conn_ssid_lbl;
static lv_obj_t *conn_ip_lbl;
static lv_obj_t *conn_rssi_lbl;

// View: Fokus (Verbinden / Reconnect-Countdown)
static lv_obj_t *view_focus;
static lv_obj_t *focus_spinner;
static lv_obj_t *focus_title;
static lv_obj_t *focus_sub;
static lv_obj_t *focus_primary_btn;
static lv_obj_t *focus_primary_lbl;

// ============================================================
// Interner State
// ============================================================
#define MAX_SCAN_RESULTS 24
#define SSID_LEN 33
#define PW_LEN 65

struct scan_entry_t {
    char ssid[SSID_LEN];
    int8_t rssi;
    bool secure;
};

static scan_entry_t scan_results[MAX_SCAN_RESULTS];
static int scan_count = 0;

static char selected_ssid[SSID_LEN];
static char pending_password[PW_LEN];
static char connected_ssid[SSID_LEN];

static bool connect_is_auto = false;      // laeuft der Versuch ohne User-Interaktion?
static bool auto_reconnect_enabled = true; // nach manuellem "Trennen" aus
static uint8_t retry_attempt = 0;          // fuer Backoff
static uint16_t retry_seconds_left = 0;
static uint16_t connect_ticks = 0;         // Ticks des connect_timer (250ms)

// Disconnect-Reason kommt aus dem WiFi-Task — nur lesen/schreiben, kein LVGL!
static volatile uint8_t last_disconnect_reason = 0;

static Preferences prefs;

// Timer: nur der Watchdog laeuft dauerhaft und wird NIE geloescht.
static lv_timer_t *connect_timer = nullptr;
static lv_timer_t *scan_timer = nullptr;
static lv_timer_t *retry_timer = nullptr;
static lv_timer_t *deferred_timer = nullptr;
static lv_timer_t *watchdog_timer = nullptr;

// Backoff in Sekunden — steigt, damit ein dauerhaft weg-Router nicht dauerfeuert.
static const uint16_t retry_backoff[] = {2, 3, 5, 8, 15, 30};
static constexpr uint8_t RETRY_BACKOFF_STEPS = 6;
static constexpr uint16_t CONNECT_TIMEOUT_TICKS = 48; // 48 * 250ms = 12s

// ============================================================
// Forward Declarations
// ============================================================
static void enter_state(wifi_state_t new_state);
static void start_scan();
static void start_connect(bool is_auto);
static void schedule_retry();

// ============================================================
// Credential-Speicher (NVS)
// ============================================================

static void save_credentials(const char *ssid, const char *pw)
{
    prefs.begin("wifi", false);
    prefs.putString("ssid", ssid);
    prefs.putString("pw", pw);
    prefs.end();
}

static bool load_credentials(char *ssid, size_t ssid_len, char *pw, size_t pw_len)
{
    prefs.begin("wifi", false);

    // Startwert aus secrets.h, aber nur solange ueberhaupt kein Eintrag
    // existiert. Ein leerer, aber vorhandener Eintrag bleibt leer — sonst
    // waere "Netzwerk vergessen" beim naechsten Start wieder rueckgaengig
    // gemacht, und niemand kaeme von diesem Netz mehr los.
    if (!prefs.isKey("ssid") && WIFI_DEFAULT_SSID[0] != '\0') {
        prefs.putString("ssid", WIFI_DEFAULT_SSID);
        prefs.putString("pw", WIFI_DEFAULT_PASS);
        Serial.println("[WLAN] Zugangsdaten aus secrets.h uebernommen");
    }

    String s = prefs.getString("ssid", "");
    String p = prefs.getString("pw", "");
    prefs.end();

    if (s.length() == 0) return false;

    strncpy(ssid, s.c_str(), ssid_len - 1);
    ssid[ssid_len - 1] = '\0';
    strncpy(pw, p.c_str(), pw_len - 1);
    pw[pw_len - 1] = '\0';
    return true;
}

static void clear_credentials()
{
    prefs.begin("wifi", false);
    prefs.clear();
    // Leeren Eintrag stehen lassen: Er ist die Spur, dass hier bewusst
    // geloescht wurde. Ohne ihn wuerde load_credentials() den Startwert aus
    // secrets.h fuer einen unbeschriebenen Speicher halten und ihn erneut
    // eintragen.
    prefs.putString("ssid", "");
    prefs.end();
}

static bool saved_ssid_matches(const char *ssid)
{
    char s[SSID_LEN], p[PW_LEN];
    return load_credentials(s, sizeof(s), p, sizeof(p)) && strcmp(s, ssid) == 0;
}

// ============================================================
// Helfer
// ============================================================

static int rssi_to_percent(int8_t rssi)
{
    int q = 2 * (rssi + 100);
    if (q < 0) q = 0;
    if (q > 100) q = 100;
    return q;
}

// Disconnect-Reason in Klartext — konkrete Fehler statt "Verbindung fehlgeschlagen".
static const char *disconnect_reason_text(uint8_t reason)
{
    switch (reason) {
    case 201: return "Netzwerk nicht in Reichweite";
    case 2:
    case 15:
    case 202:
    case 204: return "Passwort vermutlich falsch";
    case 203: return "Router hat die Anmeldung abgelehnt";
    case 0:   return "Zeitüberschreitung";
    default:  return "Verbindung abgebrochen";
    }
}

static void set_card(uint32_t color, const char *icon, const char *title, const char *sub)
{
    if (!ui_ready) return;
    lv_label_set_text(card_icon, icon);
    lv_obj_set_style_text_color(card_icon, lv_color_hex(color), 0);
    lv_label_set_text(card_title, title);
    lv_label_set_text(card_sub, sub ? sub : "");
}

static void stop_timer(lv_timer_t **t)
{
    if (*t) {
        lv_timer_delete(*t);
        *t = nullptr;
    }
}

// Alle Aktions-Timer stoppen. Der Watchdog bleibt bewusst am Leben.
static void stop_action_timers()
{
    stop_timer(&connect_timer);
    stop_timer(&scan_timer);
    stop_timer(&retry_timer);
    stop_timer(&deferred_timer);
}

// ============================================================
// State-Uebergaenge — eine Stelle entscheidet, was sichtbar ist
// ============================================================

static void show_only(lv_obj_t *view)
{
    if (!ui_ready) return;
    lv_obj_t *views[] = {view_list, view_password, view_connected, view_focus};
    for (lv_obj_t *v : views) {
        if (v == view) {
            lv_obj_remove_flag(v, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(v, LV_OBJ_FLAG_HIDDEN);
        }
    }
    // Tastatur haengt am Parent, nicht an der View
    if (view == view_password) {
        lv_obj_remove_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    }
}

static void enter_state(wifi_state_t new_state)
{
    state = new_state;

    if (state == WS_CONNECTED) {
        strncpy(connected_ssid, selected_ssid, sizeof(connected_ssid) - 1);
        connected_ssid[sizeof(connected_ssid) - 1] = '\0';
    }

    if (!ui_ready) return;

    // Der Scan-Knopf erscheint nur, solange keine Verbindung steht.
    //
    // Frueher stand er immer da und war nur gesperrt — die Ueberlegung war,
    // dass ein verschwindender Knopf das Layout springen laesst. Hier springt
    // aber nichts: Er sitzt allein in der Kopfzeile, und rechts daneben ist
    // ohnehin nichts. Verbunden ist ein Scan zudem nicht bloss gesperrt,
    // sondern gegenstandslos — die Ansicht darunter zeigt dann das Netz, in
    // dem man steckt.
    //
    // Waehrend Passworteingabe und Verbindungsversuch bleibt er ebenfalls
    // weg: Dort laeuft eine Handlung, die ein Scan abbrechen wuerde.
    const bool scan_visible = (state == WS_IDLE || state == WS_LIST ||
                               state == WS_RETRY_WAIT);
    if (scan_visible) {
        lv_obj_remove_flag(scan_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_state(scan_btn, LV_STATE_DISABLED);
    } else {
        lv_obj_add_flag(scan_btn, LV_OBJ_FLAG_HIDDEN);
    }

    switch (state) {
    case WS_IDLE:
        show_only(view_list);
        lv_obj_add_flag(net_list, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(list_spinner, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(list_hint, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(list_hint, "Kein Netzwerk ausgewählt.\nTippe oben auf \"Scannen\".");
        set_card(COL_MUTED, LV_SYMBOL_CLOSE, "Nicht verbunden", "Offline");
        break;

    case WS_SCANNING:
        show_only(view_list);
        lv_obj_add_flag(net_list, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(list_spinner, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(list_hint, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(list_hint, "Suche Netzwerke ...");
        set_card(COL_ACCENT, LV_SYMBOL_REFRESH, "Suche Netzwerke", "Das dauert ein paar Sekunden");
        break;

    case WS_LIST:
        show_only(view_list);
        lv_obj_add_flag(list_hint, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(list_spinner, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(net_list, LV_OBJ_FLAG_HIDDEN);
        set_card(COL_ACCENT, LV_SYMBOL_WIFI, "Netzwerk wählen",
                 scan_count == 1 ? "1 Netzwerk gefunden" : nullptr);
        if (scan_count != 1) {
            lv_label_set_text_fmt(card_sub, "%d Netzwerke gefunden", scan_count);
        }
        break;

    case WS_PASSWORD:
        show_only(view_password);
        lv_label_set_text_fmt(pw_ssid_lbl, "Passwort für %s", selected_ssid);
        lv_obj_add_flag(pw_error_lbl, LV_OBJ_FLAG_HIDDEN);
        set_card(COL_ACCENT, LV_SYMBOL_KEYBOARD, "Passwort eingeben",
                 "Leer lassen bei offenen Netzen");
        break;

    case WS_CONNECTING:
        show_only(view_focus);
        lv_obj_remove_flag(focus_spinner, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text_fmt(focus_title, "Verbinde mit\n%s", selected_ssid);
        lv_label_set_text(focus_sub, "");
        lv_label_set_text(focus_primary_lbl, LV_SYMBOL_CLOSE "  Abbrechen");
        set_card(COL_ACCENT, LV_SYMBOL_WIFI, "Verbinde ...", selected_ssid);
        break;

    case WS_CONNECTED:
        show_only(view_connected);
        lv_label_set_text(conn_ssid_lbl, connected_ssid);
        lv_label_set_text_fmt(conn_ip_lbl, LV_SYMBOL_HOME "  %s", WiFi.localIP().toString().c_str());
        lv_label_set_text_fmt(conn_rssi_lbl, LV_SYMBOL_WIFI "  %d %%  (%d dBm)",
                              rssi_to_percent((int8_t)WiFi.RSSI()), (int)WiFi.RSSI());
        set_card(COL_OK, LV_SYMBOL_OK, "Verbunden", connected_ssid);
        break;

    case WS_RETRY_WAIT:
        show_only(view_focus);
        lv_obj_add_flag(focus_spinner, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text_fmt(focus_title, "Keine Verbindung zu\n%s", selected_ssid);
        lv_label_set_text_fmt(focus_sub, "Neuer Versuch in %d s", retry_seconds_left);
        lv_label_set_text(focus_primary_lbl, LV_SYMBOL_REFRESH "  Jetzt versuchen");
        set_card(COL_WARN, LV_SYMBOL_WARNING, "Verbindung verloren",
                 disconnect_reason_text(last_disconnect_reason));
        break;
    }
}

// ============================================================
// Verbindungs-Logik
// ============================================================

// WiFi.begin() direkt nach disconnect() schlaegt oft fehl — daher die
// disconnect -> (300ms) -> begin Kette ueber einen One-Shot-Timer.
static void deferred_begin_cb(lv_timer_t *)
{
    deferred_timer = nullptr;
    connect_ticks = 0;
    last_disconnect_reason = 0;
    WiFi.begin(selected_ssid, pending_password);
}

static void connect_failed(const char *reason)
{
    Serial.printf("[WiFi] Verbindung fehlgeschlagen: %s (reason=%u)\n",
                  reason, last_disconnect_reason);
    stop_action_timers();
    WiFi.disconnect();

    if (connect_is_auto && auto_reconnect_enabled) {
        schedule_retry();
        return;
    }

    // Manueller Versuch: zurueck ins Passwortfeld, Eingabe bleibt erhalten,
    // damit der User nur den Tippfehler korrigieren muss.
    enter_state(WS_PASSWORD);
    if (ui_ready) {
        lv_textarea_set_text(pw_ta, pending_password);
        lv_label_set_text_fmt(pw_error_lbl, LV_SYMBOL_WARNING "  %s", reason);
        lv_obj_remove_flag(pw_error_lbl, LV_OBJ_FLAG_HIDDEN);
    }
}

static void check_connection_cb(lv_timer_t *)
{
    connect_ticks++;
    const wl_status_t ws = WiFi.status();

    if (ws == WL_CONNECTED) {
        stop_action_timers();
        retry_attempt = 0;
        auto_reconnect_enabled = true;
        save_credentials(selected_ssid, pending_password);
        Serial.printf("[WiFi] verbunden mit '%s', IP %s\n",
                      selected_ssid, WiFi.localIP().toString().c_str());
        enter_state(WS_CONNECTED);
        return;
    }

    // Der Reason-Code aus dem Event ist aussagekraeftiger als wl_status_t.
    // Erst ab Tick 8 auswerten — davor kann er noch vom vorherigen Versuch stammen.
    if (connect_ticks > 8 && last_disconnect_reason != 0) {
        connect_failed(disconnect_reason_text(last_disconnect_reason));
        return;
    }

    if (connect_ticks >= CONNECT_TIMEOUT_TICKS) {
        connect_failed("Zeitüberschreitung");
    }
}

static void start_connect(bool is_auto)
{
    stop_action_timers();
    connect_is_auto = is_auto;
    enter_state(WS_CONNECTING);

    WiFi.disconnect();
    deferred_timer = lv_timer_create(deferred_begin_cb, 300, nullptr);
    lv_timer_set_repeat_count(deferred_timer, 1);

    connect_timer = lv_timer_create(check_connection_cb, 250, nullptr);
    lv_timer_set_repeat_count(connect_timer, -1);
}

// ============================================================
// Auto-Reconnect mit Countdown
// ============================================================

static void retry_tick_cb(lv_timer_t *)
{
    if (retry_seconds_left > 0) retry_seconds_left--;

    if (retry_seconds_left == 0) {
        stop_timer(&retry_timer);
        start_connect(true);
        return;
    }
    if (ui_ready) {
        lv_label_set_text_fmt(focus_sub, "Neuer Versuch in %d s", retry_seconds_left);
    }
}

static void schedule_retry()
{
    const uint8_t idx = retry_attempt < RETRY_BACKOFF_STEPS ? retry_attempt : RETRY_BACKOFF_STEPS - 1;
    retry_seconds_left = retry_backoff[idx];
    if (retry_attempt < RETRY_BACKOFF_STEPS) retry_attempt++;

    enter_state(WS_RETRY_WAIT);

    retry_timer = lv_timer_create(retry_tick_cb, 1000, nullptr);
    lv_timer_set_repeat_count(retry_timer, -1);
}

// Laeuft dauerhaft: erkennt Verbindungsabbrueche im Betrieb und
// haelt die Signalanzeige im Connected-View aktuell.
static void watchdog_cb(lv_timer_t *)
{
    if (state != WS_CONNECTED) return;

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[WiFi] Verbindung verloren — starte Auto-Reconnect");
        retry_attempt = 0;
        connect_is_auto = true;
        if (auto_reconnect_enabled) {
            schedule_retry();
        } else {
            enter_state(WS_IDLE);
        }
        return;
    }

    if (ui_ready) {
        lv_label_set_text_fmt(conn_rssi_lbl, LV_SYMBOL_WIFI "  %d %%  (%d dBm)",
                              rssi_to_percent((int8_t)WiFi.RSSI()), (int)WiFi.RSSI());
    }
}

// ============================================================
// Scan
// ============================================================

static void network_row_cb(lv_event_t *e)
{
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= scan_count) return;

    strncpy(selected_ssid, scan_results[idx].ssid, sizeof(selected_ssid) - 1);
    selected_ssid[sizeof(selected_ssid) - 1] = '\0';
    auto_reconnect_enabled = true;
    retry_attempt = 0;

    // Offenes Netz: kein Passwort noetig.
    if (!scan_results[idx].secure) {
        pending_password[0] = '\0';
        start_connect(false);
        return;
    }

    // Bekanntes Netz: gespeichertes Passwort direkt nutzen, statt es
    // den User nochmal eintippen zu lassen.
    char s[SSID_LEN], p[PW_LEN];
    if (load_credentials(s, sizeof(s), p, sizeof(p)) && strcmp(s, selected_ssid) == 0) {
        strncpy(pending_password, p, sizeof(pending_password) - 1);
        pending_password[sizeof(pending_password) - 1] = '\0';
        start_connect(false);
        return;
    }

    pending_password[0] = '\0';
    lv_textarea_set_text(pw_ta, "");
    enter_state(WS_PASSWORD);
}

static void build_network_row(int idx)
{
    const scan_entry_t &net = scan_results[idx];
    const bool known = saved_ssid_matches(net.ssid);

    // Bewusst lv_obj statt lv_button: das Theme faerbt Buttons mit Akzentfarbe
    // und weissem Text — auf dem Listenhintergrund waere die SSID unsichtbar.
    // Ein lv_obj erbt die normale Textfarbe und folgt automatisch dem Theme.
    lv_obj_t *row = lv_obj_create(net_list);
    lv_obj_set_size(row, LV_PCT(100), ROW_H);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(COL_ACCENT), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_20, LV_STATE_PRESSED);
    ui_card_style(row);
    lv_obj_set_style_pad_hor(row, GAP_M, 0);
    lv_obj_set_style_pad_ver(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(row, network_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);

    lv_obj_t *icon = lv_label_create(row);
    lv_label_set_text(icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(icon, lv_color_hex(net.rssi > -70 ? COL_OK : COL_WARN), 0);
    lv_obj_set_style_pad_right(icon, 10, 0);

    lv_obj_t *name = lv_label_create(row);
    lv_label_set_text(name, net.ssid);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(name, 1);

    if (known) {
        lv_obj_t *badge = lv_label_create(row);
        lv_label_set_text(badge, "gespeichert");
        lv_obj_set_style_text_font(badge, &bb_font_12, 0);
        lv_obj_set_style_text_color(badge, lv_color_hex(COL_OK), 0);
        lv_obj_set_style_pad_right(badge, 8, 0);
    }

    if (net.secure) {
        lv_obj_t *lock = lv_label_create(row);
        lv_label_set_text(lock, LV_SYMBOL_EYE_CLOSE);
        lv_obj_set_style_text_font(lock, &bb_font_12, 0);
        lv_obj_set_style_text_color(lock, lv_color_hex(COL_MUTED), 0);
        lv_obj_set_style_pad_right(lock, 8, 0);
    }

    lv_obj_t *strength = lv_label_create(row);
    lv_label_set_text_fmt(strength, "%d%%", rssi_to_percent(net.rssi));
    lv_obj_set_style_text_font(strength, &bb_font_12, 0);
    lv_obj_set_style_text_color(strength, lv_color_hex(COL_MUTED), 0);
}

// Ergebnisse einsammeln: Duplikate raus (Mesh/Repeater senden dieselbe SSID
// mehrfach), staerkstes Signal gewinnt, Sortierung nach Signalstaerke.
static void collect_scan_results(int n)
{
    scan_count = 0;

    for (int i = 0; i < n; i++) {
        String ssid = WiFi.SSID(i);
        if (ssid.length() == 0) continue;

        const int8_t rssi = (int8_t)WiFi.RSSI(i);
        bool dup = false;
        for (int j = 0; j < scan_count; j++) {
            if (ssid == scan_results[j].ssid) {
                if (rssi > scan_results[j].rssi) scan_results[j].rssi = rssi;
                dup = true;
                break;
            }
        }
        if (dup || scan_count >= MAX_SCAN_RESULTS) continue;

        strncpy(scan_results[scan_count].ssid, ssid.c_str(), SSID_LEN - 1);
        scan_results[scan_count].ssid[SSID_LEN - 1] = '\0';
        scan_results[scan_count].rssi = rssi;
        scan_results[scan_count].secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
        scan_count++;
    }

    for (int i = 1; i < scan_count; i++) {
        scan_entry_t key = scan_results[i];
        int j = i - 1;
        while (j >= 0 && scan_results[j].rssi < key.rssi) {
            scan_results[j + 1] = scan_results[j];
            j--;
        }
        scan_results[j + 1] = key;
    }
}

static void scan_poll_cb(lv_timer_t *timer)
{
    const int16_t n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) return;

    stop_timer(&scan_timer);

    if (n == WIFI_SCAN_FAILED || n == 0) {
        WiFi.scanDelete();
        enter_state(WS_IDLE);
        if (ui_ready) {
            lv_label_set_text(list_hint, "Keine Netzwerke gefunden.\nNäher an den Router gehen und erneut scannen.");
            set_card(COL_WARN, LV_SYMBOL_WARNING, "Nichts gefunden", "Scan lieferte kein Ergebnis");
        }
        return;
    }

    collect_scan_results(n);
    WiFi.scanDelete();

    if (ui_ready) {
        lv_obj_clean(net_list);
        for (int i = 0; i < scan_count; i++) build_network_row(i);
        lv_obj_scroll_to_y(net_list, 0, LV_ANIM_OFF);
    }

    enter_state(WS_LIST);
}

static void start_scan()
{
    stop_action_timers();
    if (ui_ready) lv_obj_clean(net_list);
    scan_count = 0;
    enter_state(WS_SCANNING);

    WiFi.scanNetworks(true);
    scan_timer = lv_timer_create(scan_poll_cb, 250, nullptr);
    lv_timer_set_repeat_count(scan_timer, 60); // max ~15s
}

// ============================================================
// Button-Callbacks
// ============================================================

static void scan_btn_cb(lv_event_t *)
{
    start_scan();
}

static void pw_connect_cb(lv_event_t *)
{
    if (state != WS_PASSWORD) return;

    const char *pw = lv_textarea_get_text(pw_ta);
    strncpy(pending_password, pw, sizeof(pending_password) - 1);
    pending_password[sizeof(pending_password) - 1] = '\0';
    start_connect(false);
}

static void pw_cancel_cb(lv_event_t *)
{
    pending_password[0] = '\0';
    lv_textarea_set_text(pw_ta, "");
    enter_state(scan_count > 0 ? WS_LIST : WS_IDLE);
}

// Passwort sichtbar machen — auf Touch-Tastaturen sonst reines Raten.
static void pw_eye_cb(lv_event_t *)
{
    const bool hidden = lv_textarea_get_password_mode(pw_ta);
    lv_textarea_set_password_mode(pw_ta, !hidden);
    lv_label_set_text(pw_eye_lbl, hidden ? LV_SYMBOL_EYE_CLOSE : LV_SYMBOL_EYE_OPEN);
}

static void pw_ta_cb(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {
        pw_connect_cb(e); // Enter auf der Tastatur = Verbinden
    } else if (code == LV_EVENT_CANCEL) {
        pw_cancel_cb(e);
    }
}

static void disconnect_cb(lv_event_t *)
{
    // Bewusstes Trennen durch den User: Auto-Reconnect aus, sonst
    // verbindet sich das Geraet sofort wieder und ignoriert die Absicht.
    stop_action_timers();
    auto_reconnect_enabled = false;
    WiFi.disconnect();
    connected_ssid[0] = '\0';
    enter_state(WS_IDLE);
}

static void forget_cb(lv_event_t *)
{
    stop_action_timers();
    auto_reconnect_enabled = false;
    clear_credentials();
    WiFi.disconnect();
    connected_ssid[0] = '\0';
    selected_ssid[0] = '\0';
    enter_state(WS_IDLE);
    set_card(COL_MUTED, LV_SYMBOL_TRASH, "Netzwerk vergessen", "Zugangsdaten gelöscht");
}

static void focus_primary_cb(lv_event_t *)
{
    if (state == WS_RETRY_WAIT) {
        stop_timer(&retry_timer);
        start_connect(true); // "Jetzt versuchen" — Wartezeit ueberspringen
    } else if (state == WS_CONNECTING) {
        stop_action_timers(); // "Abbrechen"
        WiFi.disconnect();
        enter_state(scan_count > 0 ? WS_LIST : WS_IDLE);
    }
}

static void focus_secondary_cb(lv_event_t *)
{
    stop_action_timers();
    start_scan();
}

// ============================================================
// WiFi-Events (laufen im WiFi-Task — hier NICHTS mit LVGL machen)
// ============================================================

static void wifi_event_cb(WiFiEvent_t event, WiFiEventInfo_t info)
{
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
        last_disconnect_reason = info.wifi_sta_disconnected.reason;
    } else if (event == ARDUINO_EVENT_WIFI_STA_CONNECTED) {
        last_disconnect_reason = 0;
    }
}

// ============================================================
// UI-Aufbau
// ============================================================

// Die Arc-Breite aus dem Theme ist auf kleine Spinner ausgelegt und wirkt
// bei grossen Durchmessern gedrungen — daher fest und schlank setzen.
static void style_spinner(lv_obj_t *spinner)
{
    lv_obj_set_style_arc_width(spinner, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_width(spinner, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(spinner, lv_color_hex(COL_LINE), LV_PART_MAIN);
    lv_obj_set_style_arc_color(spinner, lv_color_hex(COL_ACCENT), LV_PART_INDICATOR);
}

// Panels und Knoepfe kommen aus dem Baukasten (ui_kit.h) — dieselbe Karte,
// dieselben Radien und dieselben Zustandsfarben wie auf den uebrigen Screens.
static void style_panel(lv_obj_t *obj)
{
    ui_card_style(obj);
    lv_obj_set_style_pad_all(obj, GAP_L, 0);
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text, uint32_t color,
                             lv_event_cb_t cb, lv_obj_t **out_label = nullptr)
{
    lv_obj_t *btn = ui_button(parent, color, LV_SIZE_CONTENT, 46);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_hex(COL_TEXT), 0);
    lv_obj_center(lbl);
    if (out_label) *out_label = lbl;
    return btn;
}

static void build_header(lv_obj_t *parent)
{
    // Gegenstueck zum runden Zurueck-Knopf links in derselben Kopfzeile:
    // gleiche Hoehe, gleiche Grundlinie, und als Kapsel gerundet statt als
    // Rechteck. Zwei Bedienelemente nebeneinander mit unterschiedlicher Hoehe
    // und Rundung sind das, was eine Kopfzeile unruhig macht.
    static constexpr int SCAN_H = 40;

    scan_btn = make_button(parent, LV_SYMBOL_REFRESH "  Scannen", COL_ACCENT,
                           scan_btn_cb, &scan_btn_lbl);
    lv_obj_set_size(scan_btn, 132, SCAN_H);
    lv_obj_set_style_radius(scan_btn, SCAN_H / 2, 0);
    lv_obj_set_style_text_font(scan_btn_lbl, &bb_font_12, 0);
    lv_obj_align(scan_btn, LV_ALIGN_TOP_RIGHT, -PAD, 6);
}

static void build_status_card(lv_obj_t *parent)
{
    card = lv_obj_create(parent);
    lv_obj_set_size(card, CONTENT_W, CARD_H);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, CARD_Y);
    style_panel(card);

    // Symbol in einer eigenen kleinen Flaeche statt frei stehend: Dieselbe
    // Anordnung wie bei den Startknoepfen der Systemkachel.
    lv_obj_t *icon_box = ui_tile(card, 40, 40);
    lv_obj_align(icon_box, LV_ALIGN_LEFT_MID, 0, 0);

    card_icon = lv_label_create(icon_box);
    lv_label_set_text(card_icon, LV_SYMBOL_WIFI);
    lv_obj_center(card_icon);

    card_title = lv_label_create(card);
    lv_label_set_text(card_title, "");
    lv_obj_set_style_text_font(card_title, &bb_font_16, 0);
    lv_obj_set_style_text_color(card_title, lv_color_hex(COL_TEXT), 0);
    lv_obj_align(card_title, LV_ALIGN_LEFT_MID, 52, -10);

    card_sub = lv_label_create(card);
    lv_label_set_text(card_sub, "");
    lv_obj_set_width(card_sub, CONTENT_W - 92);
    lv_label_set_long_mode(card_sub, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(card_sub, &bb_font_12, 0);
    lv_obj_set_style_text_color(card_sub, lv_color_hex(COL_MUTED), 0);
    lv_obj_align(card_sub, LV_ALIGN_LEFT_MID, 52, 11);
}

static void build_list_view(lv_obj_t *parent)
{
    view_list = lv_obj_create(parent);
    lv_obj_set_size(view_list, CONTENT_W, VIEW_H);
    lv_obj_align(view_list, LV_ALIGN_TOP_MID, 0, CONTENT_Y);
    lv_obj_set_style_bg_opa(view_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(view_list, 0, 0);
    lv_obj_set_style_border_width(view_list, 0, 0);
    lv_obj_remove_flag(view_list, LV_OBJ_FLAG_SCROLLABLE);

    net_list = lv_list_create(view_list);
    lv_obj_set_size(net_list, LV_PCT(100), LV_PCT(100));
    // Die Liste traegt keine eigene Flaeche mehr: Ihre Zeilen sind seit dem
    // Redesign selbst Karten, und eine Karte in einer Karte ist ein Rahmen
    // zuviel.
    lv_obj_set_style_bg_opa(net_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(net_list, 0, 0);
    lv_obj_set_style_pad_all(net_list, 0, 0);
    lv_obj_set_style_pad_row(net_list, GAP_S, 0);

    list_spinner = lv_spinner_create(view_list);
    lv_obj_set_size(list_spinner, 72, 72);
    lv_obj_align(list_spinner, LV_ALIGN_CENTER, 0, -46);
    lv_spinner_set_anim_params(list_spinner, 1000, 200);
    style_spinner(list_spinner);
    lv_obj_add_flag(list_spinner, LV_OBJ_FLAG_HIDDEN);

    list_hint = lv_label_create(view_list);
    lv_label_set_text(list_hint, "");
    lv_obj_set_width(list_hint, CONTENT_W - 40);
    lv_label_set_long_mode(list_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(list_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(list_hint, lv_color_hex(COL_MUTED), 0);
    lv_obj_align(list_hint, LV_ALIGN_CENTER, 0, 24);
}

static void build_password_view(lv_obj_t *parent)
{
    view_password = lv_obj_create(parent);
    lv_obj_set_size(view_password, CONTENT_W, VIEW_H - KEYBOARD_H);
    lv_obj_align(view_password, LV_ALIGN_TOP_MID, 0, CONTENT_Y);
    lv_obj_set_style_bg_opa(view_password, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(view_password, 0, 0);
    lv_obj_set_style_border_width(view_password, 0, 0);
    lv_obj_remove_flag(view_password, LV_OBJ_FLAG_SCROLLABLE);

    pw_ssid_lbl = lv_label_create(view_password);
    lv_label_set_text(pw_ssid_lbl, "");
    lv_obj_set_style_text_color(pw_ssid_lbl, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_width(pw_ssid_lbl, CONTENT_W);
    lv_label_set_long_mode(pw_ssid_lbl, LV_LABEL_LONG_DOT);
    lv_obj_align(pw_ssid_lbl, LV_ALIGN_TOP_LEFT, 4, 0);

    pw_ta = lv_textarea_create(view_password);
    lv_textarea_set_one_line(pw_ta, true);
    lv_textarea_set_password_mode(pw_ta, true);
    lv_textarea_set_placeholder_text(pw_ta, "Passwort");
    lv_obj_set_size(pw_ta, CONTENT_W - 66, 46);
    lv_obj_align(pw_ta, LV_ALIGN_TOP_LEFT, 0, 26);
    lv_obj_set_style_radius(pw_ta, RADIUS_CTRL, 0);
    lv_obj_set_style_bg_color(pw_ta, lv_color_hex(COL_RAISED), 0);
    lv_obj_set_style_border_width(pw_ta, 1, 0);
    lv_obj_set_style_border_color(pw_ta, lv_color_hex(COL_LINE), 0);
    lv_obj_set_style_border_color(pw_ta, lv_color_hex(COL_ACCENT), LV_STATE_FOCUSED);
    lv_obj_set_style_text_color(pw_ta, lv_color_hex(COL_TEXT), 0);

    lv_obj_t *eye_btn = ui_button(view_password, COL_NEUTRAL, 58, 46);
    lv_obj_align(eye_btn, LV_ALIGN_TOP_RIGHT, 0, 26);
    lv_obj_add_event_cb(eye_btn, pw_eye_cb, LV_EVENT_CLICKED, nullptr);
    pw_eye_lbl = lv_label_create(eye_btn);
    lv_label_set_text(pw_eye_lbl, LV_SYMBOL_EYE_CLOSE);
    lv_obj_center(pw_eye_lbl);

    pw_error_lbl = lv_label_create(view_password);
    lv_label_set_text(pw_error_lbl, "");
    lv_obj_set_width(pw_error_lbl, CONTENT_W);
    lv_label_set_long_mode(pw_error_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(pw_error_lbl, &bb_font_12, 0);
    lv_obj_set_style_text_color(pw_error_lbl, lv_color_hex(COL_ERR), 0);
    lv_obj_align(pw_error_lbl, LV_ALIGN_TOP_LEFT, 4, 78);
    lv_obj_add_flag(pw_error_lbl, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *cancel_btn = make_button(view_password, LV_SYMBOL_LEFT "  Zurück", COL_NEUTRAL, pw_cancel_cb);
    lv_obj_set_size(cancel_btn, 150, 46);
    lv_obj_align(cancel_btn, LV_ALIGN_TOP_LEFT, 0, 100);

    lv_obj_t *connect_btn = make_button(view_password, LV_SYMBOL_OK "  Verbinden", COL_ACCENT, pw_connect_cb);
    lv_obj_set_size(connect_btn, CONTENT_W - 160, 46);
    lv_obj_align(connect_btn, LV_ALIGN_TOP_RIGHT, 0, 100);

    keyboard = lv_keyboard_create(parent);
    lv_obj_set_size(keyboard, SCREEN_W, KEYBOARD_H);
    lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(keyboard, pw_ta);
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_event_cb(pw_ta, pw_ta_cb, LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(pw_ta, pw_ta_cb, LV_EVENT_CANCEL, nullptr);
}

static void build_connected_view(lv_obj_t *parent)
{
    view_connected = lv_obj_create(parent);
    lv_obj_set_size(view_connected, CONTENT_W, VIEW_H);
    lv_obj_align(view_connected, LV_ALIGN_TOP_MID, 0, CONTENT_Y);
    lv_obj_set_style_bg_opa(view_connected, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(view_connected, 0, 0);
    lv_obj_set_style_border_width(view_connected, 0, 0);
    lv_obj_remove_flag(view_connected, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *info = lv_obj_create(view_connected);
    lv_obj_set_size(info, CONTENT_W, 130);
    lv_obj_align(info, LV_ALIGN_TOP_MID, 0, 0);
    style_panel(info);

    conn_ssid_lbl = lv_label_create(info);
    lv_label_set_text(conn_ssid_lbl, "");
    lv_obj_set_style_text_font(conn_ssid_lbl, &bb_font_16, 0);
    lv_obj_set_style_text_color(conn_ssid_lbl, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_width(conn_ssid_lbl, CONTENT_W - 30);
    lv_label_set_long_mode(conn_ssid_lbl, LV_LABEL_LONG_DOT);
    lv_obj_align(conn_ssid_lbl, LV_ALIGN_TOP_LEFT, 4, 4);

    conn_ip_lbl = lv_label_create(info);
    lv_label_set_text(conn_ip_lbl, "");
    lv_obj_set_style_text_color(conn_ip_lbl, lv_color_hex(COL_MUTED), 0);
    lv_obj_align(conn_ip_lbl, LV_ALIGN_TOP_LEFT, 4, 42);

    conn_rssi_lbl = lv_label_create(info);
    lv_label_set_text(conn_rssi_lbl, "");
    lv_obj_set_style_text_color(conn_rssi_lbl, lv_color_hex(COL_MUTED), 0);
    lv_obj_align(conn_rssi_lbl, LV_ALIGN_TOP_LEFT, 4, 70);

    lv_obj_t *disc_btn = make_button(view_connected, LV_SYMBOL_CLOSE "  Trennen", COL_NEUTRAL, disconnect_cb);
    lv_obj_set_size(disc_btn, (CONTENT_W - 10) / 2, 48);
    lv_obj_align(disc_btn, LV_ALIGN_TOP_LEFT, 0, 146);

    lv_obj_t *forget_btn = make_button(view_connected, LV_SYMBOL_TRASH "  Vergessen", COL_ERR, forget_cb);
    lv_obj_set_size(forget_btn, (CONTENT_W - 10) / 2, 48);
    lv_obj_align(forget_btn, LV_ALIGN_TOP_RIGHT, 0, 146);
}

static void build_focus_view(lv_obj_t *parent)
{
    view_focus = lv_obj_create(parent);
    lv_obj_set_size(view_focus, CONTENT_W, VIEW_H);
    lv_obj_align(view_focus, LV_ALIGN_TOP_MID, 0, CONTENT_Y);
    lv_obj_set_style_bg_opa(view_focus, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(view_focus, 0, 0);
    lv_obj_set_style_border_width(view_focus, 0, 0);
    lv_obj_remove_flag(view_focus, LV_OBJ_FLAG_SCROLLABLE);

    focus_spinner = lv_spinner_create(view_focus);
    lv_obj_set_size(focus_spinner, 72, 72);
    lv_obj_align(focus_spinner, LV_ALIGN_TOP_MID, 0, 8);
    lv_spinner_set_anim_params(focus_spinner, 1000, 200);
    style_spinner(focus_spinner);

    focus_title = lv_label_create(view_focus);
    lv_label_set_text(focus_title, "");
    lv_obj_set_style_text_color(focus_title, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_width(focus_title, CONTENT_W - 20);
    lv_label_set_long_mode(focus_title, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(focus_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(focus_title, LV_ALIGN_TOP_MID, 0, 88);

    focus_sub = lv_label_create(view_focus);
    lv_label_set_text(focus_sub, "");
    lv_obj_set_style_text_color(focus_sub, lv_color_hex(COL_MUTED), 0);
    lv_obj_align(focus_sub, LV_ALIGN_TOP_MID, 0, 146);

    focus_primary_btn = make_button(view_focus, "", COL_ACCENT, focus_primary_cb, &focus_primary_lbl);
    lv_obj_set_size(focus_primary_btn, CONTENT_W, 48);
    lv_obj_align(focus_primary_btn, LV_ALIGN_TOP_MID, 0, 182);

    lv_obj_t *other_btn = make_button(view_focus, LV_SYMBOL_LIST "  Anderes Netzwerk", COL_NEUTRAL, focus_secondary_cb);
    lv_obj_set_size(other_btn, CONTENT_W, 48);
    lv_obj_align(other_btn, LV_ALIGN_TOP_MID, 0, 240);
}

// ============================================================
// Public API
// ============================================================

void wifi_service_start()
{
    if (service_started) return;
    service_started = true;

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false); // Reconnect steuern wir selbst (sichtbar fuer den User)
    WiFi.onEvent(wifi_event_cb, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
    WiFi.onEvent(wifi_event_cb, ARDUINO_EVENT_WIFI_STA_CONNECTED);
    WiFi.disconnect();

    // Watchdog laeuft dauerhaft und wird nie geloescht.
    watchdog_timer = lv_timer_create(watchdog_cb, 2000, nullptr);
    lv_timer_set_repeat_count(watchdog_timer, -1);

    // Ohne gespeicherte Zugangsdaten bleibt der Dienst ruhig. Ein Scan ist
    // erst sinnvoll, wenn die WLAN-Ansicht geoeffnet wird.
    char ssid[SSID_LEN], pw[PW_LEN];
    if (load_credentials(ssid, sizeof(ssid), pw, sizeof(pw))) {
        strncpy(selected_ssid, ssid, sizeof(selected_ssid) - 1);
        selected_ssid[sizeof(selected_ssid) - 1] = '\0';
        strncpy(pending_password, pw, sizeof(pending_password) - 1);
        pending_password[sizeof(pending_password) - 1] = '\0';
        Serial.printf("[WiFi] Auto-Connect zu '%s'\n", selected_ssid);
        start_connect(true);
    } else {
        enter_state(WS_IDLE);
    }
}

void wifi_screen_create(lv_obj_t *parent)
{
    wifi_service_start();
    if (ui_ready) return;

    root = parent;
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    build_header(root);
    build_status_card(root);
    build_list_view(root);
    build_password_view(root);
    build_connected_view(root);
    build_focus_view(root);
    ui_ready = true;

    if (state == WS_LIST && scan_count > 0) {
        for (int i = 0; i < scan_count; i++) build_network_row(i);
    }

    if (state == WS_IDLE) {
        start_scan();
    } else {
        enter_state(state);
    }
}

void wifi_screen_destroy()
{
    if (!ui_ready) return;

    // Ein Scan dient nur der Ansicht. Verbindungs- und Reconnect-Timer
    // bleiben dagegen als Teil des Hintergrunddienstes aktiv.
    if (state == WS_SCANNING) {
        stop_timer(&scan_timer);
        WiFi.scanDelete();
        state = WS_IDLE;
    }
    if (state == WS_CONNECTING) connect_is_auto = true;

    ui_ready = false;
    root = nullptr;
    scan_btn = nullptr;
    scan_btn_lbl = nullptr;
    card = nullptr;
    card_icon = nullptr;
    card_title = nullptr;
    card_sub = nullptr;
    view_list = nullptr;
    net_list = nullptr;
    list_hint = nullptr;
    list_spinner = nullptr;
    view_password = nullptr;
    pw_ssid_lbl = nullptr;
    pw_ta = nullptr;
    pw_eye_lbl = nullptr;
    pw_error_lbl = nullptr;
    keyboard = nullptr;
    view_connected = nullptr;
    conn_ssid_lbl = nullptr;
    conn_ip_lbl = nullptr;
    conn_rssi_lbl = nullptr;
    view_focus = nullptr;
    focus_spinner = nullptr;
    focus_title = nullptr;
    focus_sub = nullptr;
    focus_primary_btn = nullptr;
    focus_primary_lbl = nullptr;
}


