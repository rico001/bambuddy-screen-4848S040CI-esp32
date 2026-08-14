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
    {"0700_8010", "AMS-Motor ueberlastet - Filament verheddert oder Spule klemmt"},
    {"0700_8007", "Extrudieren fehlgeschlagen - Extruder moeglicherweise verstopft"},

    // Duese und Bett
    {"0300_4006", "Duese verstopft"},
    {"0300_8016", "Duese mit Filament verstopft - Druck abbrechen und reinigen"},
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

// Ein Puffer fuer Lesen und Schreiben. Zwei eigene waeren zusammen ueber
// drei Kilobyte internes RAM, dauerhaft belegt fuer etwas, das ein paar Mal
// am Tag gebraucht wird.
static uint8_t nvs_buffer[sizeof(blob_header_t) + sizeof(entries)];

static void save_locked()
{
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

    prefs.begin(NVS_NS, true);
    const size_t size = prefs.getBytesLength(NVS_KEY);

    if (size < sizeof(blob_header_t) ||
        size > sizeof(blob_header_t) + sizeof(entries)) {
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
            save_locked();
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

static bool was_active(const char *code)
{
    for (int i = 0; i < previous_count; i++) {
        if (strcmp(previous[i], code) == 0) return true;
    }
    return false;
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

    save_locked();

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

    for (int i = 0; i < count; i++) {
        if (!codes[i][0] || was_active(codes[i])) continue;

        add_entry(codes[i], bambuddy_hms_text(codes[i]),
                  severities ? severities[i] : 0);
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

    // Flankengesteuert wie bei den Fehlercodes: FAILED bleibt stehen, bis
    // der naechste Druck laeuft. Ohne diesen Vergleich stuende der Abbruch
    // im Poll-Takt zehnmal im Log.
    static char previous_state[16] = "";
    static bool have_previous = false;

    if (have_previous && strcasecmp(previous_state, state) == 0) return;

    const bool first = !have_previous;
    have_previous = true;
    strncpy(previous_state, state, sizeof(previous_state) - 1);
    previous_state[sizeof(previous_state) - 1] = '\0';

    // Den allerersten gemeldeten Zustand nur merken, nicht eintragen.
    //
    // Sonst schriebe jeder Neustart des Displays einen Abbruch ins Log, der
    // laengst vorbei ist: FAILED bleibt am Drucker stehen, bis der naechste
    // Druck laeuft — mit der Uhrzeit des Einschaltens statt der des
    // Ereignisses. Das Log soll sagen, was passiert ist, waehrend es lief.
    if (first) return;

    const bool failed = strcasecmp(state, "FAILED") == 0;
    const bool finished = strcasecmp(state, "FINISH") == 0;
    if (!failed && !finished) return;

    char text[64];
    if (failed) {
        if (job && job[0]) {
            snprintf(text, sizeof(text), "Druck gestoppt oder fehlgeschlagen: %s", job);
        } else {
            snprintf(text, sizeof(text), "Druck gestoppt oder fehlgeschlagen");
        }
    } else if (job && job[0]) {
        snprintf(text, sizeof(text), "Druck fertig: %s", job);
    } else {
        snprintf(text, sizeof(text), "Druck fertig");
    }

    ensure_ready();
    xSemaphoreTake(log_mutex, portMAX_DELAY);
    // Schwere 2 beim Abbruch: kein Hardwaredefekt, aber nichts, was man im
    // Log uebersehen moechte.
    add_entry("", text, failed ? 2 : BB_HMS_SEVERITY_OK);
    fresh = true;
    xSemaphoreGive(log_mutex);
}

void bambuddy_hms_report_boot(const char *reason, bool unexpected)
{
    char text[64];
    if (reason && reason[0]) {
        snprintf(text, sizeof(text), "Display gestartet: %s", reason);
    } else {
        snprintf(text, sizeof(text), "Display gestartet");
    }

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

const char *bambuddy_hms_text(const char *code)
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
    switch (severity) {
    case 1: return "schwer";
    case 2: return "ernst";
    case 3: return "normal";
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

void bambuddy_hms_clear()
{
    ensure_ready();

    xSemaphoreTake(log_mutex, portMAX_DELAY);
    entry_count = 0;
    memset(entries, 0, sizeof(entries));

    prefs.begin(NVS_NS, false);
    prefs.remove(NVS_KEY);
    prefs.end();

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
