#include "bambuddy_hms.h"

#include <Arduino.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdio.h>
#include <string.h>

#include "settings_screen.h"

// Klartext zu den Codes, die an einem P1S mit AMS tatsaechlich vorkommen.
//
// Bambuddys Frontend fuehrt eine Tabelle mit ueber 850 Eintraegen; sie aufs
// Geraet zu holen waere unverhaeltnismaessig. Hier stehen die haeufigen —
// alles andere zeigt das Log als blossen Code, den man im Wiki nachschlaegt:
// https://wiki.bambulab.com/en/hms/home
//
// Verglichen wird als Teilzeichenkette, und zwar ohne Unterstriche: Der
// Drucker meldet vier Gruppen ("0300_4006_0002_0001"), die Tabelle des
// Frontends fuehrt die ersten beiden als Schluessel — mal mit Unterstrich,
// mal ohne ("0500050000010007"). Ein Vergleich auf Gleichheit ginge ins
// Leere, und einer mit Unterstrich haenge davon ab, welche Schreibweise
// gerade kommt.
//
// Zwei Schluessel sind bewusst kurz gehalten: "8011" (Filament leer) und
// "8030" (Filament aufgebraucht) treten je Extruder und AMS-Einheit unter
// eigenem Praefix auf — 1200, 1201, 1202, 07FE, 07FF. Sieben Zeilen fuer
// dieselbe Aussage waeren schlechter zu pflegen als ein kurzer Schluessel.
struct hms_text_t {
    const char *key;
    const char *text;
};

static const hms_text_t HMS_TEXTS[] = {
    // Filament
    {"8011", "Filament leer - bitte neu einlegen"},
    {"8030", "Filament aufgebraucht - Druck pausiert"},
    {"0300_8004", "Filament leer - bitte neu einlegen"},
    {"0300_4008", "AMS-Filamentwechsel fehlgeschlagen"},
    {"0700_8010", "AMS-Motor überlastet - Filament verheddert oder Spule klemmt"},
    {"0700_8007", "Extrudieren fehlgeschlagen - Extruder möglicherweise verstopft"},

    // Duese und Bett
    {"0300_4006", "Düse verstopft"},
    {"0300_8016", "Düse mit Filament verstopft - Druck abbrechen und reinigen"},
    {"0300_4002", "Bettnivellierung fehlgeschlagen - Druck gestoppt"},

    // Druck gestoppt oder pausiert
    {"0300_400C", "Druck abgebrochen"},
    {"0500_400E", "Druck abgebrochen"},
    {"0300_4000", "Z-Achsen-Referenzfahrt fehlgeschlagen - Druck gestoppt"},
    {"0300_4057", "Z-Achse hat Schritte verloren - Druck gestoppt"},
    {"0500_4003", "Druck gestoppt - Datei nicht lesbar"},
    {"0300_8000", "Druck pausiert - Grund unbekannt"},
    {"0300_8001", "Druck vom Benutzer pausiert"},
    {"0300_8013", "Druck pausiert - Pausenbefehl in der Datei"},
};
static constexpr int HMS_TEXT_COUNT = sizeof(HMS_TEXTS) / sizeof(HMS_TEXTS[0]);

static const char *hms_text(const char *code);

// Nicht "log" nennen: math.h belegt den Namen mit dem Logarithmus.
static bambuddy_hms_entry_t entries[BB_HMS_LOG_MAX];
static int entry_count = 0;

// Der zuletzt gemeldete Satz — daran haengt die Flankenerkennung.
static char previous[BB_HMS_ACTIVE_MAX][24];
static int previous_count = 0;

static SemaphoreHandle_t log_mutex = nullptr;
static volatile bool fresh = false;

// ============================================================
// Dauerhafte Ablage im NVS
//
// Der Kopf traegt eine Fassungsnummer. Aendert sich der Aufbau von
// bambuddy_hms_entry_t, passt der alte Inhalt nicht mehr — dann faengt das
// Protokoll leer an, statt Unsinn anzuzeigen. Ein blosses Byte-Array ohne
// diese Angabe waere nach der naechsten Erweiterung stillschweigend falsch.
// ============================================================

static constexpr uint16_t BLOB_VERSION = 1;
static constexpr const char *NVS_NS = "hmslog";
static constexpr const char *NVS_KEY = "entries";

struct blob_header_t {
    uint16_t version;
    uint16_t count;
};

static Preferences prefs;
static bool loaded = false;
static bool backfilled = false;
static bool dirty = false;
static uint32_t dirty_since_ms = 0;

// Wann wird geschrieben?
//
// Auf dem ESP32-S3 haengen Flash und PSRAM am selben SPI-Controller.
// Waehrend eines Flash-Schreibvorgangs ist der Cache abgeschaltet und der
// Zugriff auf den PSRAM blockiert — dort liegt aber der Bildpuffer, aus dem
// das Panel fortlaufend per DMA liest. Ihm fehlen dann fuer einige
// Millisekunden die Pixel, und das sieht man als Streifen.
//
// Verhindern laesst sich das nicht, nur verstecken: indem geschrieben wird,
// wenn niemand hinsieht.
//
// Deshalb zwei Bedingungen, und beide muessen gelten:
//
//   - Sechs Minuten ohne Beruehrung. Wer davorsteht und bedient, soll nichts
//     davon mitbekommen; ein Wanddisplay steht die meiste Zeit ohnehin still.
//     Laenger als die Bildschirmabschaltung (fuenf Minuten), damit der
//     Zugriff im Regelfall in einen dunklen oder ruhenden Schirm faellt.
//   - Ein paar Sekunden Abstand zum ausloesenden Ereignis. Faellt ein Druck
//     nachts fertig, ist die Untaetigkeitsgrenze laengst ueberschritten —
//     der Neuaufbau der Kachel laeuft aber trotzdem gerade.
//
// Bewusst ohne hartes Zeitlimit: Ein Schreibvorgang, der sich nach einer
// Weile doch aufdraengt, waere genau das, was hier vermieden werden soll.
// Der Preis ist, dass ein Eintrag bei einem Stromausfall verlorengeht,
// solange niemand das Geraet in Ruhe laesst.
static constexpr uint32_t FLUSH_DELAY_MS = 5000;
static constexpr uint32_t FLUSH_IDLE_MS = 360000;

// Ein Puffer fuer Lesen und Schreiben, angelegt beim ersten Bedarf und im
// PSRAM.
//
// Zwei eigene waeren zusammen ueber drei Kilobyte. Und selbst einer gehoert
// nicht in den internen Speicher: 1,7 KB dauerhaft fuer etwas, das ein paar
// Mal am Tag gebraucht wird, waehrend der interne Vorrat auf diesem Board
// die knappe Groesse ist. Im PSRAM faellt es nicht ins Gewicht.
static constexpr size_t NVS_BUFFER_SIZE = sizeof(blob_header_t) + sizeof(entries);
static uint8_t *nvs_buffer = nullptr;

static bool ensure_buffer()
{
    if (nvs_buffer) return true;

    nvs_buffer = (uint8_t *)ps_malloc(NVS_BUFFER_SIZE);
    if (!nvs_buffer) {
        Serial.println("[HMS] Kein Speicher fuer die Ablage — Protokoll bleibt fluechtig");
    }
    return nvs_buffer != nullptr;
}

// Nur vormerken. Geschrieben wird spaeter in bambuddy_hms_flush().
static void mark_dirty()
{
    // Ohne dauerhafte Ablage gibt es nichts zu schreiben — und damit keinen
    // Flash-Zugriff, der dem Panel die Pixel wegnimmt.
    if (!settings_log_persist()) return;

    dirty = true;
    dirty_since_ms = millis();
}

static void write_locked()
{
    if (!ensure_buffer()) return;

    blob_header_t header = {BLOB_VERSION, (uint16_t)entry_count};

    // Kopf und Eintraege in einem Stueck: Zwei Schluessel koennten bei einem
    // Stromausfall zwischen den Schreibvorgaengen auseinanderlaufen.
    memcpy(nvs_buffer, &header, sizeof(header));
    memcpy(nvs_buffer + sizeof(header), entries,
           sizeof(bambuddy_hms_entry_t) * entry_count);

    prefs.begin(NVS_NS, false);
    prefs.putBytes(NVS_KEY, nvs_buffer,
                   sizeof(header) + sizeof(bambuddy_hms_entry_t) * entry_count);
    prefs.end();
}

static void load_once()
{
    if (loaded) return;
    loaded = true;

    if (!ensure_buffer()) return;

    prefs.begin(NVS_NS, true);
    const size_t size = prefs.getBytesLength(NVS_KEY);

    if (size < sizeof(blob_header_t) || size > NVS_BUFFER_SIZE) {
        prefs.end();
        return;
    }

    prefs.getBytes(NVS_KEY, nvs_buffer, size);
    prefs.end();

    blob_header_t header;
    memcpy(&header, nvs_buffer, sizeof(header));
    if (header.version != BLOB_VERSION) return;

    const size_t payload = size - sizeof(header);
    int count = (int)(payload / sizeof(bambuddy_hms_entry_t));
    if (count > header.count) count = header.count;
    if (count > BB_HMS_LOG_MAX) count = BB_HMS_LOG_MAX;
    if (count < 0) count = 0;

    memcpy(entries, nvs_buffer + sizeof(header),
           sizeof(bambuddy_hms_entry_t) * count);
    entry_count = count;

    // Die Laufzeitangabe stammt aus einem frueheren Lauf und waere jetzt
    // irrefuehrend. Der Zeitstempel bleibt gueltig, sofern die Uhr damals
    // stand.
    for (int i = 0; i < entry_count; i++) entries[i].restored = true;

    Serial.printf("[HMS] %d Eintraege aus dem Speicher geladen\n", entry_count);
}

static void ensure_mutex()
{
    if (!log_mutex) log_mutex = xSemaphoreCreateMutex();
}

// Zeitstempel nachtragen, sobald die Uhr steht.
//
// Beim Booten laeuft NTP noch nicht — der Starteintrag und alles, was in den
// ersten Sekunden passiert, haette sonst dauerhaft keine Uhrzeit. Wie lange
// es her ist, weiss das Geraet aber: uptime_s. Die Differenz zur jetzigen
// Laufzeit von der aktuellen Uhrzeit abgezogen ergibt den echten Zeitpunkt.
//
// Laeuft genau einmal. Eintraege aus einem frueheren Lauf bleiben aussen
// vor: Deren Laufzeit bezieht sich auf einen anderen Start und ergaebe eine
// frei erfundene Uhrzeit.
static void maybe_backfill()
{
    if (backfilled || !settings_time_synced()) return;

    xSemaphoreTake(log_mutex, portMAX_DELAY);
    if (!backfilled) {
        backfilled = true;

        const uint32_t now_uptime = millis() / 1000;
        const time_t now = time(nullptr);
        int fixed = 0;

        for (int i = 0; i < entry_count; i++) {
            if (entries[i].when != 0 || entries[i].restored) continue;
            if (entries[i].uptime_s > now_uptime) continue;

            entries[i].when = now - (time_t)(now_uptime - entries[i].uptime_s);
            fixed++;
        }

        if (fixed > 0) {
            mark_dirty();
            fresh = true;
            Serial.printf("[HMS] %d Zeitstempel nachgetragen\n", fixed);
        }
    }
    xSemaphoreGive(log_mutex);
}

// Mutex und Inhalt zusammen bereitstellen. Gerufen aus dem Netzwerk-Task
// ebenso wie aus der Oberflaeche — wer zuerst kommt, laedt.
static void ensure_ready()
{
    ensure_mutex();
    if (loaded) return;

    xSemaphoreTake(log_mutex, portMAX_DELAY);
    load_once();
    xSemaphoreGive(log_mutex);
}

// Zustaende, in denen ein Auftrag auf dem Drucker liegt. PREPARE deckt das
// Vorbereiten ab — Bett heizen, kalibrieren —, PAUSE die Unterbrechung.
static bool is_job_state(const char *state)
{
    static const char *const active[] = {"RUNNING", "printing", "PREPARE",
                                         "preparing", "PAUSE", "paused"};
    for (const char *s : active) {
        if (strcasecmp(state, s) == 0) return true;
    }
    return false;
}

static bool was_active(const char *code)
{
    for (int i = 0; i < previous_count; i++) {
        if (strcmp(previous[i], code) == 0) return true;
    }
    return false;
}

// "Druck fertig" mit dem Namen in der zweiten Zeile, bzw. nur die Aussage,
// wenn kein Name vorliegt.
//
// Der Umbruch ist ausdruecklich gesetzt, nicht dem Zufall der Textbreite
// ueberlassen: Was passiert ist, steht dann immer oben und der Dateiname
// darunter — untereinander liest sich eine Liste solcher Zeilen schneller
// als hintereinander. Die Ansicht raeumt dafuer zwei Zeilen ein.
//
// Stand dreimal fast gleich da; dreimal die Gelegenheit, es beim naechsten
// Mal anders zu setzen.
static void job_text(const char *prefix, const char *job, char *out, size_t out_len)
{
    if (job && job[0]) {
        snprintf(out, out_len, "%s:\n%s", prefix, job);
    } else {
        snprintf(out, out_len, "%s", prefix);
    }
}

static void add_entry(const char *code, const char *text, int32_t severity)
{
    // Neuestes vorn: Beim Anzeigen will man den letzten Fehler zuerst sehen,
    // nicht ans Ende scrollen muessen.
    if (entry_count < BB_HMS_LOG_MAX) entry_count++;
    for (int i = entry_count - 1; i > 0; i--) entries[i] = entries[i - 1];

    bambuddy_hms_entry_t &e = entries[0];
    memset(&e, 0, sizeof(e));
    strncpy(e.code, code ? code : "", sizeof(e.code) - 1);
    strncpy(e.text, text ? text : "", sizeof(e.text) - 1);
    e.severity = severity;
    e.uptime_s = millis() / 1000;

    // Zeitstempel nur mit gestellter Uhr. Sonst bleibt when auf 0, und die
    // Ansicht zeigt stattdessen die Laufzeit — besser als eine Uhrzeit aus
    // dem Startwert des Chips.
    e.when = settings_time_synced() ? time(nullptr) : 0;
    e.restored = false;

    mark_dirty();

    Serial.printf("[HMS] %s %s (Schwere %d)\n", e.code, e.text, (int)severity);
}

void bambuddy_hms_report(const char codes[][24], const int32_t *severities, int count)
{
    if (count < 0) count = 0;
    if (count > BB_HMS_ACTIVE_MAX) count = BB_HMS_ACTIVE_MAX;

    ensure_ready();

    // Hier statt in der Ansicht: Dieser Weg laeuft bei jedem Statuseingang,
    // also spaetestens Sekunden nach dem NTP-Abgleich. An der Ansicht haengte
    // der Zeitpunkt sonst davon ab, wann jemand zufaellig hinschaut.
    maybe_backfill();

    xSemaphoreTake(log_mutex, portMAX_DELAY);

    // Der zuletzt gesehene Satz wird auch dann fortgeschrieben, wenn nichts
    // eingetragen wird — sonst gaebe ein spaeter eingeschalteter Schalter
    // alle laengst anstehenden Fehler auf einen Schlag ins Protokoll.
    const bool wanted = settings_log_errors();

    for (int i = 0; i < count; i++) {
        if (!codes[i][0] || was_active(codes[i])) continue;
        if (!wanted) continue;

        add_entry(codes[i], hms_text(codes[i]), severities ? severities[i] : 0);
        fresh = true;
    }

    previous_count = count;
    for (int i = 0; i < count; i++) {
        strncpy(previous[i], codes[i], sizeof(previous[0]) - 1);
        previous[i][sizeof(previous[0]) - 1] = '\0';
    }

    xSemaphoreGive(log_mutex);
}

void bambuddy_hms_report_state(const char *state, const char *job)
{
    if (!state || !state[0]) return;

    ensure_ready();
    xSemaphoreTake(log_mutex, portMAX_DELAY);

    // Die Merker liegen mit unter dem Mutex. Gelesen wird dieser Weg zwar
    // immer nur aus einem Task — Bambuddy liefert entweder per MQTT oder per
    // HTTP, nie beides —, aber beim Umschalten der Quelle im laufenden
    // Betrieb koennen sich beide fuer einen Moment ueberschneiden.
    static char previous_state[16] = "";
    static bool have_previous = false;
    static bool was_job = false;

    if (have_previous && strcasecmp(previous_state, state) == 0) {
        xSemaphoreGive(log_mutex);
        return;
    }

    const bool first = !have_previous;
    have_previous = true;
    strncpy(previous_state, state, sizeof(previous_state) - 1);
    previous_state[sizeof(previous_state) - 1] = '\0';

    // Den allerersten gemeldeten Zustand nur merken, nicht eintragen.
    //
    // Sonst schriebe jeder Neustart des Displays einen Abbruch ins Log, der
    // laengst vorbei ist: FAILED bleibt am Drucker stehen, bis der naechste
    // Druck laeuft — mit der Uhrzeit des Einschaltens statt der des
    // Ereignisses. Das Protokoll soll sagen, was passiert ist, waehrend es
    // lief. Ebenso beim Auftragszustand: Ein beim Einschalten laufender
    // Druck ist kein neu gestarteter.
    if (first) {
        was_job = is_job_state(state);
        xSemaphoreGive(log_mutex);
        return;
    }

    const bool failed = strcasecmp(state, "FAILED") == 0;
    const bool finished = strcasecmp(state, "FINISH") == 0;

    // Beginn eines Auftrags: Uebergang von "kein Auftrag" zu einem der
    // Auftragszustaende.
    //
    // Nicht schlicht auf RUNNING pruefen — ein Druck laeuft ueber PREPARE an
    // und kaeme sonst erst nach dem Heizen ins Protokoll. Und ein Fortsetzen
    // nach einer Pause geht ebenfalls nach RUNNING; das ist kein neuer Druck
    // und soll auch nicht so dastehen.
    const bool started = is_job_state(state) && !was_job;
    was_job = is_job_state(state);

    // Nach dem Fortschreiben der Merker pruefen, nicht davor: Ein
    // ausgeschalteter Eintrag darf die Flankenerkennung nicht aushebeln.
    const bool wanted = (started && settings_log_print_start()) ||
                        (finished && settings_log_print_done()) ||
                        (failed && settings_log_errors());

    if (!wanted) {
        xSemaphoreGive(log_mutex);
        return;
    }

    char text[64];
    if (started) {
        job_text("Druck gestartet", job, text, sizeof(text));
    } else if (failed) {
        job_text("Druck gestoppt oder fehlgeschlagen", job, text, sizeof(text));
    } else {
        job_text("Druck fertig", job, text, sizeof(text));
    }

    // Abbruch als Fehler — kein Hardwaredefekt, aber nichts, was man im
    // Protokoll uebersehen moechte. Fertiger Druck gruen, Start als blosser
    // Hinweis.
    add_entry("", text, failed ? 2 : (started ? 4 : BB_HMS_SEVERITY_OK));
    fresh = true;
    xSemaphoreGive(log_mutex);
}

void bambuddy_hms_report_auto_off(const char *plug_name)
{
    char text[64];
    job_text("Strom aus (Auto-Off) nach Druckende", plug_name, text, sizeof(text));

    ensure_ready();
    xSemaphoreTake(log_mutex, portMAX_DELAY);
    // Hinweisstufe: Es ist nichts schiefgegangen, aber man will es wissen.
    add_entry("", text, 4);
    fresh = true;
    xSemaphoreGive(log_mutex);

    // Sofort sichern statt auf den ruhigen Moment zu warten: Gleich faellt
    // der Strom des Druckers, und wenn das Display an derselben Steckdose
    // haengt, ist der Eintrag sonst weg, bevor er geschrieben wurde.
    bambuddy_hms_flush_now();
}

void bambuddy_hms_report_boot(const char *reason, bool unexpected)
{
    if (!settings_log_boot()) return;

    char text[64];
    job_text("Display gestartet", reason, text, sizeof(text));

    ensure_ready();
    xSemaphoreTake(log_mutex, portMAX_DELAY);
    // Ein gewollter Start ist ein Hinweis, ein Absturz nicht.
    add_entry("", text, unexpected ? 2 : 4);
    fresh = true;
    xSemaphoreGive(log_mutex);
}

int bambuddy_hms_count()
{
    ensure_ready();
    maybe_backfill();
    return entry_count;
}

bool bambuddy_hms_get(int index, bambuddy_hms_entry_t *out)
{
    if (!out) return false;
    ensure_ready();
    maybe_backfill();

    xSemaphoreTake(log_mutex, portMAX_DELAY);
    const bool ok = index >= 0 && index < entry_count;
    if (ok) *out = entries[index];
    xSemaphoreGive(log_mutex);
    return ok;
}

// Unterstriche entfernen, damit beide Schreibweisen dasselbe ergeben.
static void strip_underscores(const char *src, char *out, size_t out_len)
{
    size_t n = 0;
    for (const char *p = src; *p && n + 1 < out_len; p++) {
        if (*p != '_') out[n++] = *p;
    }
    out[n] = '\0';
}

static const char *hms_text(const char *code)
{
    if (!code || !code[0]) return "";

    char flat[32];
    strip_underscores(code, flat, sizeof(flat));

    for (int i = 0; i < HMS_TEXT_COUNT; i++) {
        char key[24];
        strip_underscores(HMS_TEXTS[i].key, key, sizeof(key));
        if (strstr(flat, key)) return HMS_TEXTS[i].text;
    }
    return "";
}

const char *bambuddy_hms_severity_text(int32_t severity)
{
    // Eine durchgaengige Leiter statt gemischter Begriffe. "normal" fuer
    // Stufe 3 stand frueher direkt neben "ernst" und las sich, als sei
    // alles in Ordnung.
    switch (severity) {
    case 1: return "kritisch";
    case 2: return "Fehler";
    case 3: return "Warnung";
    case 4: return "Hinweis";
    case BB_HMS_SEVERITY_OK: return "fertig";
    default: break;
    }

    // Unbekannte Stufe durchreichen statt zu "normal" zu machen — sonst
    // stuende dort etwas Falsches.
    static char text[16];
    snprintf(text, sizeof(text), "Stufe %d", (int)severity);
    return text;
}

static void flush_locked_now()
{
    xSemaphoreTake(log_mutex, portMAX_DELAY);
    if (dirty) {
        dirty = false;
        write_locked();
    }
    xSemaphoreGive(log_mutex);
}

void bambuddy_hms_flush()
{
    if (!dirty || !log_mutex) return;

    if (millis() - dirty_since_ms < FLUSH_DELAY_MS) return;
    if (settings_display_idle_ms() < FLUSH_IDLE_MS) return;

    flush_locked_now();
}

void bambuddy_hms_flush_now()
{
    if (!dirty || !log_mutex) return;
    flush_locked_now();
}

void bambuddy_hms_clear()
{
    ensure_ready();

    xSemaphoreTake(log_mutex, portMAX_DELAY);
    entry_count = 0;
    memset(entries, 0, sizeof(entries));

    prefs.begin(NVS_NS, false);
    prefs.remove(NVS_KEY);
    prefs.end();

    // Nichts mehr zu schreiben — sonst legte der naechste Flush das gerade
    // Geloeschte wieder an.
    dirty = false;
    fresh = true;
    xSemaphoreGive(log_mutex);

    Serial.println("[HMS] Protokoll geleert");
}

bool bambuddy_hms_take_fresh()
{
    if (!fresh) return false;
    fresh = false;
    return true;
}
