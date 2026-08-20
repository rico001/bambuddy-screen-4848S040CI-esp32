#include "screensaver.h"

#include <Arduino.h>
#include <lvgl.h>
#include <string.h>
#include <time.h>

#include "bambuddy_api.h"
#include "settings_screen.h"
#include "ui_layout.h"
#include "ui_theme.h"
#include "ui_util.h"
#include "ui_font.h"

static lv_obj_t *overlay = nullptr;
static lv_timer_t *tick = nullptr;
static screensaver_mode_t current_mode = SCREENSAVER_OFF;

// ============================================================
// Uhr
// ============================================================

static lv_obj_t *time_lbl = nullptr;
static lv_obj_t *date_lbl = nullptr;
static lv_obj_t *printer_lbl = nullptr;
static lv_obj_t *clock_panel = nullptr; // nur im kombinierten Modus
static lv_obj_t *state_pill = nullptr;  // Kapsel um den Druckerzustand
static lv_obj_t *state_dot = nullptr;   // farbiger Punkt darin
static lv_obj_t *progress_bar = nullptr;
static int shown_progress = -1;
static int shown_minute = -1;

// Der Schoner hat eine eigene Farbwelt, und das ist Absicht.
//
// Er liegt immer auf Schwarz — auch im hellen Schema: Ein weiss leuchtender
// 480x480-Bildschirm um drei Uhr nachts waere das Gegenteil eines Schoners.
// Die Token aus ui_theme.h taugen hier deshalb nicht: COL_TEXT ist im hellen
// Schema dunkel und auf schwarzem Grund unlesbar. Stattdessen die Werte des
// dunklen Schemas, fest verdrahtet.
static constexpr uint32_t SAVER_TEXT = 0xE8ECF2; // wie COL_TEXT (dunkel)

// Der Regen faellt in Blau, nicht in Gruen.
//
// Das Gruen war ein Zitat, das mit dem Rest des Geraets nichts zu tun hatte.
// Die Toene hier leiten sich vom Akzentblau der Oberflaeche ab (COL_ACCENT):
// der Kopf fast weiss, der Schweif deutlich dunkler — dieser Abstand ist es,
// der die Fallrichtung ueberhaupt erkennbar macht. Feste Werte, weil der
// Schoner immer auf Schwarz liegt und dem hellen Schema nicht folgen darf.
static constexpr uint32_t RAIN_HEAD = 0xD6E6FF;   // Kopf der Spalte
static constexpr uint32_t RAIN_ACCENT = 0x3B82F6; // wie COL_ACCENT (dunkel)

// Der Schweif verlaeuft vom Kopf weg ins Blau: hell direkt hinter dem Kopf,
// dann in vier Stufen dunkler. Ein einfarbiger Schweif sieht aus wie ein
// fallendes Wort; erst das Abklingen macht daraus eine Spur.
//
// Die Stufen sind fest und nicht gerechnet: Eine Interpolation im RGB-Raum
// kippt zwischen Weiss und Blau ins Graue, von Hand gesetzte Werte bleiben
// bunt.
static constexpr int RAIN_SEGMENTS = 4;
static constexpr uint32_t RAIN_TRAIL[RAIN_SEGMENTS] = {
    0x9CC0F0, // direkt hinter dem Kopf
    0x5E8ED8,
    0x3564BC,
    0x2450A8, // Ende der Spur
};

// Tafel im Regen: sehr dunkles Blau statt Schwarz, damit sie als Teil des
// Bildes wirkt und nicht als Loch darin.
static constexpr uint32_t PANEL_BG = 0x081220;
static constexpr uint32_t PANEL_TEXT = 0xD6E6FF;
static constexpr uint32_t PANEL_MUTED = 0x7FA6D9;
static constexpr uint32_t PANEL_PILL = 0x0C1A2B;
static constexpr uint32_t PANEL_TRACK = 0x16324F;

// Farbe fuer "nichts zu melden": heller als das uebliche Grau, weil der
// Schoner bei gedaempfter Beleuchtung aus einigen Metern gelesen wird.
static constexpr uint32_t PRINTER_NEUTRAL_RGB = 0x9AA5AD;

// Wochentage ausgeschrieben statt der englischen Kuerzel von strftime: Das
// Geraet spricht sonst ueberall Deutsch.
static const char *const WEEKDAYS[] = {"Sonntag",    "Montag",  "Dienstag", "Mittwoch",
                                       "Donnerstag", "Freitag", "Samstag"};

// Was macht der Drucker gerade? Genau die Zeile, fuer die man sonst hingeht
// und antippt. Die Daten liegen ohnehin im Speicher — kein zusaetzlicher
// Abruf, nur eine andere Darstellung.
//
// bambuddy_api_copy_status() statt _take(): Das Frisch-Merkmal gehoert dem
// Status-Screen. Wuerde der Bildschirmschoner es verbrauchen, uebersaehe
// jener beim Aufwachen seine erste Aktualisierung.
static void printer_summary(uint32_t *color_out, char *out, size_t out_len)
{
    *color_out = PRINTER_NEUTRAL_RGB;

    bambuddy_status_t st;
    if (!bambuddy_api_copy_status(&st)) {
        out[0] = '\0';
        return;
    }

    if (!st.printer_connected) {
        snprintf(out, out_len, "Drucker offline");
        return;
    }

    char rest[24];
    ui_format_duration(st.remaining_min * 60, rest, sizeof(rest));

    if (strcasecmp(st.state, "RUNNING") == 0) {
        *color_out = COL_OK;
        if (rest[0]) {
            snprintf(out, out_len, "Druckt  %d %%  noch %s", (int)(st.progress + 0.5f),
                     rest);
        } else {
            snprintf(out, out_len, "Druckt  %d %%", (int)(st.progress + 0.5f));
        }
    } else if (strcasecmp(st.state, "PREPARE") == 0) {
        *color_out = COL_ACCENT;
        snprintf(out, out_len, "Bereitet vor");
    } else if (strcasecmp(st.state, "PAUSE") == 0) {
        *color_out = COL_WARN;
        snprintf(out, out_len, "Pausiert  %d %%", (int)(st.progress + 0.5f));
    } else if (strcasecmp(st.state, "FINISH") == 0) {
        *color_out = COL_OK;
        snprintf(out, out_len, "Druck fertig");
    } else if (strcasecmp(st.state, "FAILED") == 0) {
        *color_out = COL_ERR;
        snprintf(out, out_len, "Druck fehlgeschlagen");
    } else if (st.state[0]) {
        // state ist ein freier String vom Drucker. Unbekanntes durchreichen
        // statt zu "Bereit" zu machen — sonst steht dort etwas Falsches.
        snprintf(out, out_len, "%s",
                 strcasecmp(st.state, "IDLE") == 0 ? "Bereit" : st.state);
    } else {
        out[0] = '\0';
    }
}

// Wieviel Prozent zeigt der Balken — und zeigt er ueberhaupt etwas?
// Waehrend eines Drucks ist der Fortschritt die Angabe, fuer die man sonst
// hingeht; steht nichts an, waere ein Balken auf null nur ein Strich.
static int printer_progress()
{
    bambuddy_status_t st;
    if (!bambuddy_api_copy_status(&st) || !st.printer_connected) return -1;

    const bool running = strcasecmp(st.state, "RUNNING") == 0 ||
                         strcasecmp(st.state, "PAUSE") == 0;
    if (!running) return -1;

    const int pct = (int)(st.progress + 0.5f);
    return pct < 0 ? 0 : (pct > 100 ? 100 : pct);
}

static void clock_refresh_printer()
{
    if (!printer_lbl) return;

    char text[64];
    uint32_t color;
    printer_summary(&color, text, sizeof(text));

    ui_set_text(printer_lbl, text);
    ui_set_text_color(printer_lbl, color);

    // Der Punkt traegt die Zustandsfarbe, der Text bleibt hell: Aus der
    // Entfernung sieht man die Farbe, aus der Naehe liest man die Zeile.
    if (state_dot) ui_set_bg_color(state_dot, color);
    if (state_pill) {
        // Leere Zeile heisst: nichts zu melden. Dann verschwindet die Kapsel,
        // statt als leerer Kasten dazustehen.
        if (text[0]) {
            lv_obj_remove_flag(state_pill, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(state_pill, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (progress_bar) {
        const int pct = printer_progress();
        if (pct < 0) {
            lv_obj_add_flag(progress_bar, LV_OBJ_FLAG_HIDDEN);
            shown_progress = -1;
        } else {
            lv_obj_remove_flag(progress_bar, LV_OBJ_FLAG_HIDDEN);
            // Nur bei echter Aenderung setzen: lv_bar_set_value zeichnet
            // sonst jede Sekunde neu, auch wenn dieselbe Zahl herauskommt.
            if (pct != shown_progress) {
                shown_progress = pct;
                lv_bar_set_value(progress_bar, pct, LV_ANIM_OFF);
            }
            lv_obj_set_style_bg_color(progress_bar, lv_color_hex(color),
                                      LV_PART_INDICATOR);
        }
    }

    // Der Rahmen der Tafel bleibt im Blau des Regens.
    //
    // Frueher trug er die Zustandsfarbe — das stammt aus der Zeit, als der
    // Zustand nur als farbiger Text darunter stand. Seit die Kapsel ihren
    // eigenen Punkt hat, sagen beide dasselbe, und der Rahmen ist die
    // schlechtere Stelle dafuer: Er ist die groesste Flaeche im Bild und
    // wuerde es bei jedem Zustandswechsel umfaerben.
}

static void clock_refresh(bool force)
{
    if (!time_lbl || !date_lbl) return;

    const time_t now = time(nullptr);
    struct tm tm_now;
    localtime_r(&now, &tm_now);

    // Nur bei Minutenwechsel neu zeichnen. Jede Sekunde neu zu setzen wuerde
    // LVGL sechzigmal so oft schmutzig machen — und jede Neuzeichnung
    // konkurriert mit dem Panel um die Speicherbandbreite.
    if (!force && tm_now.tm_min == shown_minute) return;
    shown_minute = tm_now.tm_min;

    ui_set_text_fmt(time_lbl, "%02d:%02d", tm_now.tm_hour, tm_now.tm_min);
    ui_set_text_fmt(date_lbl, "%s, %d.%d.%d", WEEKDAYS[tm_now.tm_wday % 7],
                    tm_now.tm_mday, tm_now.tm_mon + 1, tm_now.tm_year + 1900);
}

// Wie stark deckt die Tafel? LV_OPA_COVER waere undurchsichtig, kleinere
// Werte lassen den Regen durchscheinen.
//
// Das ist der teuerste Wert in diesem Modus: Durchscheinend heisst, dass die
// Flaeche bei jedem Schritt einer Spalte darunter neu mit dem Hintergrund
// verrechnet und die Beschriftungen darauf neu gezeichnet werden muessen —
// auch die grosse Uhrzeit. Auf diesem Board teilen sich Bildpuffer und Panel
// eine Speicheranbindung, und zuviel davon wird als Zucken sichtbar.
//
// 85 % ist der Kompromiss: Der Regen ist als Andeutung zu erkennen, die
// Ziffern bleiben ruhig. Wer mehr davon sehen will, geht auf LV_OPA_70
// herunter — darunter wird die Uhrzeit unruhig zu lesen.
static constexpr lv_opa_t CLOCK_PANEL_OPA = (lv_opa_t)217; // ~85 %

// Die Tafel fuer den kombinierten Modus: eine Flaeche mitten im Regen, mit
// Rahmen im Akzentblau.
static lv_obj_t *clock_panel_build()
{
    lv_obj_t *panel = lv_obj_create(overlay);
    lv_obj_set_size(panel, SCREEN_W - 56, 236);
    lv_obj_center(panel);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(panel, 18, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);

    lv_obj_set_style_bg_color(panel, lv_color_hex(PANEL_BG), 0);
    lv_obj_set_style_bg_opa(panel, CLOCK_PANEL_OPA, 0);
    lv_obj_set_style_border_width(panel, 2, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(RAIN_ACCENT), 0);
    lv_obj_set_style_border_opa(panel, LV_OPA_COVER, 0);
    return panel;
}

// Uhr, Datum, Zustandskapsel und — waehrend eines Drucks — ein schmaler
// Fortschrittsbalken.
//
// Der Aufbau ist in beiden Modi derselbe, nur die Farben und die Abstaende
// unterscheiden sich: allein auf Schwarz das gewohnte Weiss-Grau, auf der
// Tafel im Regen dessen Blautoene.
static void clock_build(bool on_panel)
{
    lv_obj_t *parent = overlay;
    uint32_t time_rgb = SAVER_TEXT;
    // Heller als sonst: Aus zwei Metern Abstand und bei 30 %
    // Hintergrundbeleuchtung waere das uebliche Grau fuer Nebeninformation
    // kaum noch zu lesen.
    uint32_t date_rgb = PRINTER_NEUTRAL_RGB;
    uint32_t pill_rgb = 0x1A1F27;
    uint32_t track_rgb = 0x232A35;
    int width = SCREEN_W - 40;

    if (on_panel) {
        clock_panel = clock_panel_build();
        parent = clock_panel;
        time_rgb = PANEL_TEXT; // dieselbe Farbe wie die Koepfe der Spalten
        date_rgb = PANEL_MUTED;
        pill_rgb = PANEL_PILL;
        track_rgb = PANEL_TRACK;
        width = SCREEN_W - 76;
    }

    // Die Uhrzeit ist die eine Sache, fuer die man aus dem Sessel hinsieht.
    // Alles andere ordnet sich darunter an, mit deutlichem Abstand — eng
    // gesetzte Zeilen lesen sich aus der Entfernung als Block.
    time_lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(time_lbl, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(time_lbl, lv_color_hex(time_rgb), 0);
    lv_obj_align(time_lbl, LV_ALIGN_CENTER, 0, on_panel ? -52 : -66);

    date_lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(date_lbl, &bb_font_16, 0);
    lv_obj_set_style_text_color(date_lbl, lv_color_hex(date_rgb), 0);
    // Weite Laufweite: Das Datum ist die ruhige Zeile unter der Uhrzeit und
    // darf gesperrt stehen, ohne mit ihr um Aufmerksamkeit zu streiten.
    lv_obj_set_style_text_letter_space(date_lbl, 2, 0);
    lv_obj_align(date_lbl, LV_ALIGN_CENTER, 0, on_panel ? 2 : -6);

    // Zustand als Kapsel mit farbigem Punkt — dieselbe Form wie im
    // Status-Screen, nur groesser und in den Farben des Schoners.
    state_pill = lv_obj_create(parent);
    lv_obj_remove_flag(state_pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(state_pill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(state_pill, LV_SIZE_CONTENT, 40);
    lv_obj_set_style_radius(state_pill, 20, 0);
    lv_obj_set_style_border_width(state_pill, 0, 0);
    lv_obj_set_style_pad_hor(state_pill, 18, 0);
    lv_obj_set_style_pad_ver(state_pill, 0, 0);
    lv_obj_set_style_bg_color(state_pill, lv_color_hex(pill_rgb), 0);
    lv_obj_set_style_bg_opa(state_pill, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(state_pill, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(state_pill, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_align(state_pill, LV_ALIGN_CENTER, 0, on_panel ? 52 : 48);

    state_dot = lv_obj_create(state_pill);
    lv_obj_remove_flag(state_dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(state_dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(state_dot, 10, 10);
    lv_obj_set_style_radius(state_dot, 5, 0);
    lv_obj_set_style_border_width(state_dot, 0, 0);
    lv_obj_set_style_pad_all(state_dot, 0, 0);
    lv_obj_set_style_margin_right(state_dot, 10, 0);

    // Ohne feste Breite: Die Kapsel legt sich um den Text, statt immer
    // ueber die halbe Zeile zu gehen. "Bereit" wird dann zu einer kleinen
    // Kapsel, "Druckt 45 % noch 1 h 12 min" zu einer breiten — das ist der
    // Unterschied zwischen einer Anzeige und einem Kasten mit Text darin.
    //
    // Die Obergrenze faengt den Sonderfall ab, dass der Drucker einen langen
    // unbekannten Zustand durchreicht; laenger als die Tafel darf die Kapsel
    // nicht werden.
    lv_obj_set_style_max_width(state_pill, width, 0);

    printer_lbl = lv_label_create(state_pill);
    lv_obj_set_style_text_font(printer_lbl, &bb_font_16, 0);
    lv_obj_set_style_text_color(printer_lbl, lv_color_hex(time_rgb), 0);

    // Fortschritt nur waehrend eines Drucks — sonst waere es ein Strich ohne
    // Aussage. Flach und mit runden Enden wie im Status-Screen.
    progress_bar = lv_bar_create(parent);
    lv_obj_set_size(progress_bar, width - 60, 6);
    lv_obj_align(progress_bar, LV_ALIGN_CENTER, 0, on_panel ? 92 : 100);
    lv_bar_set_range(progress_bar, 0, 100);
    lv_bar_set_value(progress_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(progress_bar, 3, 0);
    lv_obj_set_style_radius(progress_bar, 3, LV_PART_INDICATOR);
    lv_obj_set_style_border_width(progress_bar, 0, 0);
    lv_obj_set_style_bg_color(progress_bar, lv_color_hex(track_rgb), 0);
    lv_obj_add_flag(progress_bar, LV_OBJ_FLAG_HIDDEN);

    shown_minute = -1;
    shown_progress = -1;
    clock_refresh(true);
    clock_refresh_printer();
}

// ============================================================
// Matrix
// ============================================================
//
// Teuer ist hier nicht das Rechnen, sondern das Neuzeichnen: Der Bildpuffer
// liegt im PSRAM, aus dem das Panel gleichzeitig liest. Deshalb zeichnet
// dieser Schoner nicht Bild fuer Bild die ganze Flaeche, sondern haelt je
// Spalte zwei Beschriftungen und verschiebt nur diese — LVGL macht dann pro
// Schritt bloss die betroffenen Streifen schmutzig.
//
// Dazu laeuft jede Spalte in ihrem eigenen Takt: Bei jedem Tick ruehrt sich
// nur ein Teil von ihnen. Ein gleichzeitiger Schritt aller Spalten waere ein
// voller Bildaufbau — genau das, was auf diesem Board sichtbar zuckt.

// Wieviel Regen vertraegt das Geraet?
//
// Jedes bewegte Objekt kostet LVGL pro Bild einen Zeichenauftrag, und diese
// Auftraege legt es im internen RAM an — demselben Speicher, aus dem sich der
// WLAN-Stack bedient. Bei Spalten x (Abschnitte + 1) Beschriftungen ist das
// die groesste Dauerlast, die die Oberflaeche erzeugt:
//
//   20 Spalten, 4 Abschnitte -> 100 Objekte   (aktuell)
//   14 Spalten, 4 Abschnitte ->  70 Objekte
//   20 Spalten, 2 Abschnitte ->  60 Objekte
//   20 Spalten, 1 Abschnitt  ->  40 Objekte   (Stand vor dem Farbverlauf)
//
// Wer hier dreht, sollte danach die Zeile "[MQTT] ... intern frei ..." im
// Log beobachten: Faellt sie waehrend des Schoners unter etwa 20 KB, reissen
// Verbindungen ab.
static constexpr int MATRIX_COLS = 20;
static constexpr int MATRIX_COL_W = SCREEN_W / MATRIX_COLS; // 24
static constexpr int MATRIX_LINE_H = 28;                    // Rasterhoehe je Zeichen
static constexpr int MATRIX_ROWS = SCREEN_H / MATRIX_LINE_H + 1;
static constexpr int MATRIX_TRAIL_MIN = 5;
static constexpr int MATRIX_TRAIL_MAX = 12;
static constexpr uint32_t MATRIX_TICK_MS = 110;

// Katakana waere das Vorbild gewesen; so weit reicht der Zeichenvorrat der
// Schnitte nicht (siehe ui_font.h — Latin-1 endet lange davor). Ziffern und
// Grossbuchstaben kommen dem Bild am naechsten — mit dem Blau ist es ohnehin
// kein Zitat mehr, sondern eine eigene Note.
static const char MATRIX_CHARS[] = "0123456789ABCDEFGHJKLMNPQRSTUVWXYZ<>*+=";
static constexpr int MATRIX_CHAR_COUNT = sizeof(MATRIX_CHARS) - 1;

struct matrix_col_t {
    lv_obj_t *seg[RAIN_SEGMENTS]; // Schweif, von hell nach dunkel
    lv_obj_t *head;
    int16_t row;     // Zeile des hellen Kopfes, darf negativ sein
    uint8_t length;  // Laenge des Schweifs in Zeichen
    uint8_t period;  // alle wie viel Ticks ein Schritt?
    uint8_t counter;
    char text[RAIN_SEGMENTS][2 * MATRIX_TRAIL_MAX]; // je Zeichen ein Umbruch
    char head_text[2];
};

// Die Beschriftungen zeigen auf diese Puffer, statt den Text zu kopieren.
//
// lv_label_set_text() ruft intern lv_realloc — bei zwanzig Spalten waeren
// das rund sechzig Neuanforderungen je Sekunde, und der Schoner laeuft
// womoeglich die ganze Nacht. lv_label_set_text_static() zeigt stattdessen
// auf einen Puffer, der uns gehoert: keine Allokation, keine Zerstueckelung
// des internen Speichers.
//
// Bedingung dafuer: Der Puffer muss leben, solange die Beschriftung lebt,
// und nach jeder Aenderung muss set_text_static() erneut gerufen werden —
// sonst weiss LVGL nicht, dass es neu messen und zeichnen soll.

static matrix_col_t columns[MATRIX_COLS];

static char matrix_random_char()
{
    return MATRIX_CHARS[random(MATRIX_CHAR_COUNT)];
}

// Spalte neu auswuerfeln. Sie startet oberhalb des Bildes, damit sie
// hereinlaeuft, statt aus dem Nichts zu erscheinen.
static void matrix_reset(matrix_col_t &c)
{
    c.length = MATRIX_TRAIL_MIN + random(MATRIX_TRAIL_MAX - MATRIX_TRAIL_MIN + 1);
    c.period = 1 + random(4);
    c.counter = random(c.period + 1);
    c.row = -(int16_t)random(MATRIX_ROWS);
}

// Den Schweif auf die Abschnitte verteilen und setzen.
//
// Der erste Abschnitt beginnt eine Zeile ueber dem Kopf, jeder weitere
// schliesst an. Bleibt fuer einen Abschnitt nichts uebrig — kurze Schweife
// gibt es —, bekommt er einen leeren Text und verschwindet.
static void matrix_write_trail(matrix_col_t &c)
{
    int remaining = c.length;
    int above = 1; // Abstand des naechsten Zeichens ueber dem Kopf

    for (int s = 0; s < RAIN_SEGMENTS; s++) {
        const int count = remaining / (RAIN_SEGMENTS - s);

        size_t n = 0;
        for (int i = 0; i < count && n + 2 < sizeof(c.text[s]); i++) {
            if (i > 0) c.text[s][n++] = '\n';
            c.text[s][n++] = matrix_random_char();
        }
        c.text[s][n] = '\0';
        lv_label_set_text_static(c.seg[s], c.text[s]);

        // Die Beschriftung wird an ihrer obersten Zeile ausgerichtet, der
        // Abschnitt reicht aber von oben nach unten auf den Kopf zu.
        lv_obj_set_y(c.seg[s], (c.row - above - count + 1) * MATRIX_LINE_H);

        above += count;
        remaining -= count;
    }
}

static void matrix_step(matrix_col_t &c)
{
    c.row++;
    if (c.row - (int16_t)c.length > MATRIX_ROWS) matrix_reset(c);

    // Zeichen bei jedem Schritt neu wuerfeln — ein starrer Schweif sieht aus
    // wie ein herunterfallendes Wort, nicht wie Matrix.
    matrix_write_trail(c);

    lv_obj_set_y(c.head, c.row * MATRIX_LINE_H);

    c.head_text[0] = matrix_random_char();
    c.head_text[1] = '\0';
    lv_label_set_text_static(c.head, c.head_text);
}

static void matrix_build()
{
    for (int i = 0; i < MATRIX_COLS; i++) {
        matrix_col_t &c = columns[i];
        c = {};
        matrix_reset(c);

        for (int s = 0; s < RAIN_SEGMENTS; s++) {
            c.seg[s] = lv_label_create(overlay);
            lv_obj_set_style_text_font(c.seg[s], &bb_font_24, 0);
            lv_obj_set_style_text_color(c.seg[s], lv_color_hex(RAIN_TRAIL[s]), 0);

            // Zeilenabstand aus der Schrift ableiten, nicht aus ihrer
            // Punktgroesse: Die Abschnitte sind mehrzeilige Labels, der Kopf
            // ist ein eigenes, und alle werden in Schritten von
            // MATRIX_LINE_H gesetzt. Passt der Abstand im Label nicht dazu,
            // laeuft der Schweif dem Kopf langsam davon. Die Zeilenhoehe
            // haengt am Zeichenvorrat — mit den Latin-1-Schnitten aus
            // ui_font.h ist sie eine andere als vorher.
            lv_obj_set_style_text_line_space(
                c.seg[s], MATRIX_LINE_H - lv_font_get_line_height(&bb_font_24), 0);
            lv_obj_set_x(c.seg[s], i * MATRIX_COL_W + 4);
        }

        // Der Kopf ist heller als der Schweif — das ist das, was die
        // Bewegungsrichtung ueberhaupt erkennbar macht.
        c.head = lv_label_create(overlay);
        lv_obj_set_style_text_font(c.head, &bb_font_24, 0);
        lv_obj_set_style_text_color(c.head, lv_color_hex(RAIN_HEAD), 0);
        lv_obj_set_x(c.head, i * MATRIX_COL_W + 4);

        matrix_step(c);
    }
}

static void matrix_tick()
{
    for (int i = 0; i < MATRIX_COLS; i++) {
        matrix_col_t &c = columns[i];
        if (++c.counter < c.period) continue;

        c.counter = 0;
        matrix_step(c);
    }
}

// ============================================================
// Gemeinsam
// ============================================================

static void tick_cb(lv_timer_t *)
{
    if (current_mode == SCREENSAVER_CLOCK) {
        clock_refresh(false);
        clock_refresh_printer();
        return;
    }

    if (current_mode != SCREENSAVER_MATRIX && current_mode != SCREENSAVER_MATRIX_CLOCK) {
        return;
    }

    matrix_tick();

    if (!time_lbl) return;

    // Der Takt gehoert dem Regen; die Uhr braucht ihn nicht. Einmal je Sekunde
    // nachsehen reicht — und geschrieben wird ohnehin nur, wenn sich der Text
    // geaendert hat (ui_set_text).
    static uint8_t clock_divider = 0;
    if (++clock_divider < (1000 / MATRIX_TICK_MS)) return;
    clock_divider = 0;

    clock_refresh(false);
    clock_refresh_printer();
}

static void build(screensaver_mode_t mode)
{
    overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(overlay, SCREEN_W, SCREEN_H);
    lv_obj_align(overlay, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(overlay, 0, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_pad_all(overlay, 0, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);

    // Bewusst nicht anklickbar: Die Weckflaeche der Abschaltung liegt
    // darueber und faengt die Beruehrung ab. Zwei Empfaenger fuer denselben
    // Tipp waeren zwei Gelegenheiten, ihn unterschiedlich zu behandeln.
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_CLICKABLE);

    if (mode == SCREENSAVER_MATRIX || mode == SCREENSAVER_MATRIX_CLOCK) {
        matrix_build();

        // Nach dem Regen angelegt und damit darueber. Ohne gestellte Uhr
        // bleibt es beim reinen Regen: Eine erfundene Uhrzeit gross in den
        // Raum zu leuchten waere schlimmer als gar keine — und anders als bei
        // der reinen Uhr gibt es hier ja noch etwas zu sehen.
        if (mode == SCREENSAVER_MATRIX_CLOCK && settings_time_synced()) {
            clock_build(true);
        }

        tick = lv_timer_create(tick_cb, MATRIX_TICK_MS, nullptr);
    } else {
        clock_build(false);
        tick = lv_timer_create(tick_cb, 1000, nullptr);
    }
    lv_timer_set_repeat_count(tick, -1);
}

void screensaver_show(screensaver_mode_t mode)
{
    if (mode == current_mode) return;

    if (tick) {
        lv_timer_delete(tick);
        tick = nullptr;
    }
    if (overlay) {
        // Asynchron: Der Aufruf kommt aus dem Weck-Callback einer Flaeche,
        // die ueber diesem Overlay liegt.
        lv_obj_delete_async(overlay);
        overlay = nullptr;
    }

    time_lbl = nullptr;
    date_lbl = nullptr;
    printer_lbl = nullptr;
    clock_panel = nullptr;
    state_pill = nullptr;
    state_dot = nullptr;
    progress_bar = nullptr;
    shown_progress = -1;
    memset(columns, 0, sizeof(columns));
    current_mode = mode;

    if (mode == SCREENSAVER_OFF) return;

    build(mode);
}

bool screensaver_visible()
{
    return overlay != nullptr;
}

bool screensaver_mode_available(screensaver_mode_t mode)
{
    // "Matrix + Uhr" steht hier bewusst nicht: Ohne gestellte Zeit faellt nur
    // die Tafel weg, der Regen bleibt. Die Wahl deshalb abzulehnen und
    // stattdessen abzuschalten waere weniger, als der Modus zu bieten hat.
    if (mode == SCREENSAVER_CLOCK) return settings_time_synced();
    return mode != SCREENSAVER_OFF;
}
