#include "bambuddy_filament.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <ctype.h>
#include <strings.h> // strncasecmp
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdlib.h>
#include <string.h>

#include "bambuddy_config.h"
#include "bambuddy_http.h"
#include "bambuddy_status_parse.h"

// ============================================================
// Ableitungen aus dem Profilnamen
//
// Der Drucker bekommt beim Konfigurieren Material und Temperaturen mit,
// die Profillisten liefern aber nur Namen. Bambuddys Oberflaeche rechnet
// sich beides im Browser aus, bevor sie den Aufruf absetzt — dieselbe
// Rechnung steht hier, damit derselbe Name zum selben Ergebnis fuehrt.
// Weicht das ab, hat ein Slot je nach Bedienweg andere Temperaturen.
// ============================================================

// Reihenfolge wie in Bambuddy: Es gewinnt der erste Treffer, deshalb steht
// PETG vor PET und PCTG vor PC.
static const char *const MATERIALS[] = {"PLA", "PETG", "PCTG", "ABS",  "ASA",
                                        "TPU", "PC",   "PA",   "NYLON", "PVA",
                                        "HIPS", "PP",  "PET"};
static constexpr int MATERIAL_COUNT = sizeof(MATERIALS) / sizeof(MATERIALS[0]);

// Generische Kurz-IDs je Material. Ein lokales Preset hat keine eigene
// Filament-ID, die der Drucker kennt — Bambuddy schickt deshalb die
// generische des Materials. Die Preset-ID selbst sieht der Drucker nie,
// auch wenn die API-Beschreibung das nahelegt.
struct generic_id_t {
    const char *material;
    const char *idx;
};

static const generic_id_t GENERIC_IDS[] = {
    {"PLA", "GFL99"},   {"PLA-CF", "GFL98"},  {"PLA SILK", "GFL96"},
    {"PLA HIGH SPEED", "GFL95"},              {"PETG", "GFG99"},
    {"PETG HF", "GFG96"}, {"PETG-CF", "GFG98"}, {"PCTG", "GFG97"},
    {"ABS", "GFB99"},   {"ASA", "GFB98"},     {"PC", "GFC99"},
    {"PA", "GFN99"},    {"PA-CF", "GFN98"},   {"NYLON", "GFN99"},
    {"TPU", "GFU99"},   {"PVA", "GFS99"},     {"HIPS", "GFS98"},
    {"PE", "GFP99"},    {"PP", "GFP97"},
};
static constexpr int GENERIC_ID_COUNT = sizeof(GENERIC_IDS) / sizeof(GENERIC_IDS[0]);

static bool is_word_char(char c)
{
    return isalnum((unsigned char)c) != 0;
}

// Wortgrenzen-Suche: "PA" darf nicht in "PAHT" anschlagen, "PET" nicht in
// "PETG". Ohne diese Pruefung bekaeme ein PETG-Profil die Temperaturen von
// PET, und das faellt erst am verzogenen Druck auf.
static const char *find_word(const char *haystack, const char *needle)
{
    const size_t len = strlen(needle);
    for (const char *p = haystack; *p; p++) {
        if (strncasecmp(p, needle, len) != 0) continue;
        if (p != haystack && is_word_char(p[-1])) continue;
        if (is_word_char(p[len])) continue;
        return p;
    }
    return nullptr;
}

// Namenszusaetze ab "@" gehoeren zum Druckermodell ("Bambu PLA @BBL P1S"),
// nicht zum Material. Bambuddy schneidet sie ab, bevor es weitersucht — und
// schickt genau diesen gekuerzten Namen als tray_sub_brands mit.
static void strip_variant(const char *src, char *out, size_t out_len)
{
    bambuddy_copy_field(out, out_len, src);
    char *at = strchr(out, '@');
    if (at) *at = '\0';

    size_t len = strlen(out);
    while (len > 0 && out[len - 1] == ' ') out[--len] = '\0';
}

static void derive_material(const char *name, char *out, size_t out_len)
{
    char base[64];
    strip_variant(name, base, sizeof(base));

    // "Bambu Support For PA/PET": Das Material steht hinter "Support For",
    // davor steht das Traegermaterial des Herstellers.
    const char *search = base;
    const char *support = nullptr;
    for (const char *p = base; *p; p++) {
        if (strncasecmp(p, "SUPPORT FOR ", 12) == 0) {
            support = p + 12;
            break;
        }
    }
    if (support) search = support;

    for (int i = 0; i < MATERIAL_COUNT; i++) {
        if (find_word(search, MATERIALS[i])) {
            bambuddy_copy_field(out, out_len, MATERIALS[i]);
            return;
        }
    }

    // Kein bekanntes Material: zweites Wort nehmen ("Fiberon PA6-CF" -> PA6-CF).
    const char *space = strchr(base, ' ');
    if (space && space[1]) {
        bambuddy_copy_field(out, out_len, space + 1);
        char *end = strchr(out, ' ');
        if (end) *end = '\0';
        return;
    }
    bambuddy_copy_field(out, out_len, base);
}

// Temperaturen nach Material — dieselben Werte und dieselbe Reihenfolge wie
// im Frontend. PCTG steht vor PC, sonst faengt "PC" den Treffer ab.
static void material_temps(const char *material, int16_t *lo, int16_t *hi)
{
    char m[16];
    bambuddy_copy_field(m, sizeof(m), material);
    for (char *p = m; *p; p++) *p = toupper((unsigned char)*p);

    if (strstr(m, "PLA")) { *lo = 190; *hi = 230; return; }
    if (strstr(m, "PETG")) { *lo = 220; *hi = 260; return; }
    if (strstr(m, "ABS") || strstr(m, "ASA")) { *lo = 240; *hi = 280; return; }
    if (strstr(m, "TPU")) { *lo = 200; *hi = 240; return; }
    if (strcmp(m, "PCTG") == 0) { *lo = 220; *hi = 260; return; }
    if (strstr(m, "PC")) { *lo = 260; *hi = 300; return; }
    if (strstr(m, "PA") || strstr(m, "NYLON")) { *lo = 250; *hi = 290; return; }

    *lo = 190;
    *hi = 230;
}

static const char *generic_tray_idx(const char *material)
{
    char m[24];
    bambuddy_copy_field(m, sizeof(m), material);
    for (char *p = m; *p; p++) *p = toupper((unsigned char)*p);

    for (int i = 0; i < GENERIC_ID_COUNT; i++) {
        if (strcmp(m, GENERIC_IDS[i].material) == 0) return GENERIC_IDS[i].idx;
    }

    // Wie im Frontend abgestuft weitersuchen: erst ohne CF-Endung, dann ohne
    // "+", zuletzt nur das erste Teilwort ("PLA-CF" -> "PLA").
    char trimmed[24];
    bambuddy_copy_field(trimmed, sizeof(trimmed), m);
    char *cut = strpbrk(trimmed, "-+ ");
    if (cut) {
        *cut = '\0';
        for (int i = 0; i < GENERIC_ID_COUNT; i++) {
            if (strcmp(trimmed, GENERIC_IDS[i].material) == 0) return GENERIC_IDS[i].idx;
        }
    }
    return "";
}

// ============================================================
// Zustand
// ============================================================

static constexpr int SLOT_PRESET_MAX = 16;
static constexpr uint32_t RETRY_AFTER_ERROR_MS = 8000;

struct slot_preset_t {
    int32_t ams_id;
    int32_t tray_id;
    char preset_id[24];
    // Der Klartextname kommt aus derselben Antwort. Ihn mitzunehmen erspart
    // das Nachschlagen in der Profilliste — und er steht auch dann schon zur
    // Verfuegung, wenn die Liste noch gar nicht geladen ist.
    char preset_name[48];
};

struct configure_request_t {
    volatile bool pending;
    int32_t ams_id;
    int32_t tray_id;
    int preset_index; // -1 = zuruecksetzen
    uint32_t color_rgb;
};

// Die Profilliste ist rund 11 KB gross. Im internen RAM waere das ein
// spuerbarer Anteil des knappen Vorrats; ueber malloc landet ein Block
// dieser Groesse im PSRAM (alles ueber 4 KB), wo genug Platz ist.
static bambuddy_filament_preset_t *presets = nullptr;
static int preset_count = 0;

static slot_preset_t slot_presets[SLOT_PRESET_MAX];
static int slot_preset_count = 0;

static SemaphoreHandle_t list_mutex = nullptr;
static volatile bool visible = false;
static volatile bool loaded = false;
static volatile bool list_fresh = false;
static volatile bool busy = false;
static volatile bool read_request_active = false;
static uint32_t last_error_ms = 0;

static configure_request_t request = {false, 0, 0, -1, 0};

static char message[80] = "";
static volatile uint32_t message_ms = 0;

static void ensure_mutex()
{
    if (!list_mutex) list_mutex = xSemaphoreCreateMutex();
}

static void set_message(const char *text)
{
    strncpy(message, text, sizeof(message) - 1);
    message[sizeof(message) - 1] = '\0';
    message_ms = millis();
}

static void read_error_detail(HTTPClient &http, char *out, size_t out_len)
{
    if (out_len) out[0] = '\0';

    JsonDocument filter;
    filter["detail"] = true;

    JsonDocument doc;
    if (deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter))) return;

    const char *detail = doc["detail"] | "";
    if (detail[0]) bambuddy_copy_field(out, out_len, detail);
}

// ============================================================
// Abrufe
// ============================================================

static bool begin_get(BambuddyHttp &session, const char *url)
{
    if (!session.begin(url, true)) return false;
    HTTPClient &http = session.http();
    http.setTimeout(4000);
    http.setConnectTimeout(4000);
    return true;
}

static bool fetch_builtin(int &count)
{
    BambuddyHttp &session = bambuddy_http_shared();
    const char *url = bambuddy_url("/cloud/builtin-filaments");
    if (!begin_get(session, url)) return false;

    HTTPClient &http = session.http();
    read_request_active = true;
    const int code = http.GET();
    if (code != 200) {
        char detail[80];
        read_error_detail(http, detail, sizeof(detail));
        session.end(false);
        read_request_active = false;
        Serial.printf("[Filament] Integrierte Liste HTTP %d%s%s\n", code,
                      detail[0] ? " | " : "", detail);
        // Der haeufigste Fall: dem API-Key fehlt der Cloud-Zugriff. Ohne
        // Klartext sucht man den Grund sonst im Netzwerk statt im Schluessel.
        if (code == 403) set_message("API-Key ohne Cloud-Zugriff");
        return false;
    }

    JsonDocument filter;
    JsonObject entry = filter.add<JsonObject>();
    entry["filament_id"] = true;
    entry["name"] = true;

    JsonDocument doc;
    const DeserializationError err =
        deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
    session.end(false);
    read_request_active = false;
    if (err) {
        Serial.printf("[Filament] Integrierte Liste nicht lesbar: %s\n", err.c_str());
        return false;
    }

    for (JsonObject obj : doc.as<JsonArray>()) {
        if (count >= BB_FILAMENT_MAX_PRESETS) break;

        const char *fid = obj["filament_id"] | "";
        const char *name = obj["name"] | "";
        if (!fid[0] || !name[0]) continue;

        bambuddy_filament_preset_t &p = presets[count++];
        memset(&p, 0, sizeof(p));
        snprintf(p.id, sizeof(p.id), "builtin_%s", fid);
        strip_variant(name, p.name, sizeof(p.name));
        derive_material(name, p.material, sizeof(p.material));
        p.local = false;
    }
    return true;
}

static bool fetch_local(int &count)
{
    BambuddyHttp &session = bambuddy_http_shared();
    const char *url = bambuddy_url("/local-presets/");
    if (!begin_get(session, url)) return false;

    HTTPClient &http = session.http();
    read_request_active = true;
    const int code = http.GET();
    if (code != 200) {
        session.end(false);
        read_request_active = false;
        Serial.printf("[Filament] Lokale Presets HTTP %d\n", code);
        return false;
    }

    JsonDocument filter;
    JsonObject entry = filter["filament"].add<JsonObject>();
    entry["id"] = true;
    entry["name"] = true;
    entry["filament_type"] = true;
    entry["nozzle_temp_min"] = true;
    entry["nozzle_temp_max"] = true;

    JsonDocument doc;
    const DeserializationError err =
        deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
    session.end(false);
    read_request_active = false;
    if (err) {
        Serial.printf("[Filament] Lokale Presets nicht lesbar: %s\n", err.c_str());
        return false;
    }

    for (JsonObject obj : doc["filament"].as<JsonArray>()) {
        if (count >= BB_FILAMENT_MAX_PRESETS) break;

        const int32_t id = obj["id"] | 0;
        const char *name = obj["name"] | "";
        if (id == 0 || !name[0]) continue;

        bambuddy_filament_preset_t &p = presets[count++];
        memset(&p, 0, sizeof(p));
        snprintf(p.id, sizeof(p.id), "local_%d", (int)id);
        strip_variant(name, p.name, sizeof(p.name));

        // Der ausdrueckliche Typ des Presets schlaegt die Namensdeutung.
        const char *type = obj["filament_type"] | "";
        if (type[0]) {
            bambuddy_copy_field(p.material, sizeof(p.material), type);
        } else {
            derive_material(name, p.material, sizeof(p.material));
        }

        p.temp_min = obj["nozzle_temp_min"] | 0;
        p.temp_max = obj["nozzle_temp_max"] | 0;
        p.local = true;
    }
    return true;
}

static void fetch_slot_presets()
{
    BambuddyHttp &session = bambuddy_http_shared();
    const char *url = bambuddy_url("/printers/%d/slot-presets", bambuddy_printer_id());
    if (!begin_get(session, url)) return;

    HTTPClient &http = session.http();
    read_request_active = true;
    const int code = http.GET();
    if (code != 200) {
        session.end(false);
        read_request_active = false;
        return;
    }

    JsonDocument doc;
    const DeserializationError err = deserializeJson(doc, http.getStream());
    session.end(false);
    read_request_active = false;
    if (err) return;

    slot_preset_count = 0;
    // Die Antwort ist ein Objekt, dessen Schluessel die laufende Slotnummer
    // ist. Die Nummer selbst wird nicht gebraucht — ams_id und tray_id
    // stehen im Wert.
    for (JsonPair kv : doc.as<JsonObject>()) {
        if (slot_preset_count >= SLOT_PRESET_MAX) break;

        JsonObject obj = kv.value().as<JsonObject>();
        if (obj.isNull()) continue;

        slot_preset_t &s = slot_presets[slot_preset_count++];
        s.ams_id = obj["ams_id"] | 0;
        s.tray_id = obj["tray_id"] | 0;
        bambuddy_copy_field(s.preset_id, sizeof(s.preset_id), obj["preset_id"] | "");
        bambuddy_copy_field(s.preset_name, sizeof(s.preset_name),
                            obj["preset_name"] | "");
    }
}

static void load_presets()
{
    if (!presets) {
        presets = (bambuddy_filament_preset_t *)malloc(
            sizeof(bambuddy_filament_preset_t) * BB_FILAMENT_MAX_PRESETS);
        if (!presets) {
            Serial.println("[Filament] Kein Speicher fuer die Profilliste");
            set_message("Zu wenig Speicher");
            last_error_ms = millis();
            return;
        }
    }

    int count = 0;
    // Lokale Profile zuerst: Sie sind die bewusst angelegten und stehen
    // deshalb oben, genau wie in Bambuddy.
    const bool local_ok = fetch_local(count);
    const bool builtin_ok = fetch_builtin(count);

    if (!local_ok && !builtin_ok) {
        last_error_ms = millis();
        if (!message[0]) set_message("Profile nicht abrufbar");
        return;
    }

    fetch_slot_presets();

    ensure_mutex();
    xSemaphoreTake(list_mutex, portMAX_DELAY);
    preset_count = count;
    loaded = true;
    list_fresh = true;
    xSemaphoreGive(list_mutex);

    last_error_ms = 0;
    Serial.printf("[Filament] %d Profile geladen (%d Slot-Zuordnungen)\n", count,
                  slot_preset_count);
}

// ============================================================
// Schreiben
// ============================================================

// Alles, was in eine URL geht, muss kodiert werden: Profilnamen enthalten
// Leerzeichen ("Bambu PLA Basic") und Schraegstriche ("Support For PA/PET").
// Unkodiert bricht der Pfad auseinander und der Server antwortet mit 404.
static void url_encode(const char *src, char *out, size_t out_len)
{
    // Nicht HEX nennen: Arduinos Print.h belegt den Namen als Makro.
    static const char *hex_digits = "0123456789ABCDEF";
    size_t o = 0;
    for (const char *p = src; *p && o + 4 < out_len; p++) {
        const unsigned char c = (unsigned char)*p;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = hex_digits[c >> 4];
            out[o++] = hex_digits[c & 0x0F];
        }
    }
    out[o < out_len ? o : out_len - 1] = '\0';
}

static int send_post(const char *url, char *detail, size_t detail_len)
{
    if (detail_len) detail[0] = '\0';

    BambuddyHttp &session = bambuddy_http_shared();
    if (!session.begin(url, true)) return -1;

    HTTPClient &http = session.http();
    http.setTimeout(6000);
    http.setConnectTimeout(6000);

    const int code = http.POST((uint8_t *)nullptr, 0);
    if (code < 200 || code >= 300) read_error_detail(http, detail, detail_len);
    session.end(false);
    return code;
}

static int send_put(const char *url)
{
    BambuddyHttp &session = bambuddy_http_shared();
    if (!session.begin(url, true)) return -1;

    HTTPClient &http = session.http();
    http.setTimeout(6000);
    http.setConnectTimeout(6000);

    const int code = http.sendRequest("PUT", (uint8_t *)nullptr, 0);
    session.end(false);
    return code;
}

// Zuordnung Slot -> Profil merken. Der Drucker kennt nur die Kurz-ID; welches
// Profil dahinter stand, weiss allein Bambuddy. Ohne diesen zweiten Aufruf
// steht beim naechsten Oeffnen nichts vorausgewaehlt — auch nicht im Browser.
static void save_slot_preset(const bambuddy_filament_preset_t &p, int32_t ams_id,
                             int32_t tray_id)
{
    char name_enc[144];
    url_encode(p.name, name_enc, sizeof(name_enc));

    char url[512];
    snprintf(url, sizeof(url),
             "%s/api/v1/printers/%d/slot-presets/%d/%d"
             "?preset_id=%s&preset_name=%s&preset_source=%s",
             bambuddy_base_url(), bambuddy_printer_id(), (int)ams_id, (int)tray_id,
             p.id, name_enc, p.local ? "local" : "builtin");

    const int code = send_put(url);
    if (code < 200 || code >= 300) {
        Serial.printf("[Filament] Zuordnung nicht gespeichert (HTTP %d)\n", code);
    }
}

static void configure_slot(const configure_request_t &req)
{
    bambuddy_filament_preset_t p;
    if (!bambuddy_filament_get(req.preset_index, &p)) {
        set_message("Profil nicht gefunden");
        return;
    }

    // Integriert: die eigene Kurz-ID. Lokal: die generische des Materials —
    // die Preset-ID kennt der Drucker nicht.
    char tray_idx[24];
    if (p.local) {
        bambuddy_copy_field(tray_idx, sizeof(tray_idx), generic_tray_idx(p.material));
    } else {
        bambuddy_copy_field(tray_idx, sizeof(tray_idx), p.id + 8); // "builtin_" ueberspringen
    }
    if (!tray_idx[0]) {
        set_message("Material dem Drucker unbekannt");
        return;
    }

    int16_t lo = 0;
    int16_t hi = 0;
    bambuddy_filament_effective_temps(p, &lo, &hi);

    char type_enc[32];
    char brand_enc[144];
    url_encode(p.material, type_enc, sizeof(type_enc));
    url_encode(p.name, brand_enc, sizeof(brand_enc));

    // Eigener Puffer statt bambuddy_url(): Der gemeinsame Puffer fasst 256
    // Zeichen. Diese URL wird laenger — ein kodierter Profilname belegt bis
    // zum Dreifachen seiner Laenge ("Support For PA/PET" -> 24 Zeichen).
    char url[512];
    snprintf(url, sizeof(url),
             "%s/api/v1/printers/%d/slots/%d/%d/configure"
             // tray_color ist RRGGBBAA — die Deckkraft steht als "FF" fest
             // hinter dem Farbwert.
             "?tray_info_idx=%s&tray_type=%s&tray_sub_brands=%s&tray_color=%06XFF"
             "&nozzle_temp_min=%d&nozzle_temp_max=%d&cali_idx=-1&nozzle_diameter=0.4",
             bambuddy_base_url(), bambuddy_printer_id(), (int)req.ams_id,
             (int)req.tray_id, tray_idx, type_enc, brand_enc,
             (unsigned)(req.color_rgb & 0xFFFFFF), (int)lo, (int)hi);

    char detail[80];
    const int code = send_post(url, detail, sizeof(detail));
    if (code >= 200 && code < 300) {
        save_slot_preset(p, req.ams_id, req.tray_id);
        char text[80];
        snprintf(text, sizeof(text), "%s gesetzt", p.name);
        set_message(text);
        return;
    }

    Serial.printf("[Filament] configure -> HTTP %d%s%s\n", code, detail[0] ? " | " : "",
                  detail);
    if (detail[0]) {
        set_message(detail);
    } else {
        char text[48];
        snprintf(text, sizeof(text), "Fehlgeschlagen (HTTP %d)", code);
        set_message(text);
    }
}

static void reset_slot(const configure_request_t &req)
{
    char url[240];
    snprintf(url, sizeof(url), "%s/api/v1/printers/%d/ams/%d/tray/%d/reset",
             bambuddy_base_url(), bambuddy_printer_id(), (int)req.ams_id,
             (int)req.tray_id);

    char detail[80];
    const int code = send_post(url, detail, sizeof(detail));
    if (code >= 200 && code < 300) {
        set_message("Slot zurueckgesetzt");
        return;
    }

    Serial.printf("[Filament] reset -> HTTP %d%s%s\n", code, detail[0] ? " | " : "",
                  detail);
    if (detail[0]) {
        set_message(detail);
    } else {
        char text[48];
        snprintf(text, sizeof(text), "Fehlgeschlagen (HTTP %d)", code);
        set_message(text);
    }
}

// ============================================================
// Schnittstelle
// ============================================================

void bambuddy_filament_set_visible(bool value)
{
    visible = value;
    if (value) {
        last_error_ms = 0;
    } else if (read_request_active) {
        // Der laufende GET steckt im Netzwerk-Task. Das Schliessen des
        // Sockets laesst ihn sofort zurueckkehren, statt die Oberflaeche
        // sekundenlang auf eine Antwort warten zu lassen, die niemand mehr
        // sehen will.
        bambuddy_http_shared().cancel();
    }
}

bool bambuddy_filament_visible()
{
    return visible;
}

void bambuddy_filament_update()
{
    if (WiFi.status() != WL_CONNECTED || !bambuddy_config_complete()) return;

    if (request.pending) {
        busy = true;
        configure_request_t req = request;
        request.pending = false;

        if (req.preset_index < 0) {
            reset_slot(req);
        } else {
            configure_slot(req);
        }

        // Die Zuordnungen haben sich geaendert — beim naechsten Oeffnen soll
        // die Vorauswahl stimmen.
        fetch_slot_presets();
        list_fresh = true;
        busy = false;
        return;
    }

    if (!visible || loaded) return;
    if (last_error_ms && (millis() - last_error_ms) < RETRY_AFTER_ERROR_MS) return;

    load_presets();
}

int bambuddy_filament_count()
{
    return loaded ? preset_count : 0;
}

bool bambuddy_filament_get(int index, bambuddy_filament_preset_t *out)
{
    if (!out || !presets || !list_mutex) return false;

    xSemaphoreTake(list_mutex, portMAX_DELAY);
    const bool ok = index >= 0 && index < preset_count;
    if (ok) *out = presets[index];
    xSemaphoreGive(list_mutex);
    return ok;
}

bool bambuddy_filament_ready()
{
    return loaded;
}

bool bambuddy_filament_take_fresh()
{
    if (!list_fresh) return false;
    list_fresh = false;
    return true;
}

int bambuddy_filament_slot_preset_index(int32_t ams_id, int32_t tray_id)
{
    if (!loaded || !presets) return -1;

    const char *wanted = nullptr;
    for (int i = 0; i < slot_preset_count; i++) {
        if (slot_presets[i].ams_id == ams_id && slot_presets[i].tray_id == tray_id) {
            wanted = slot_presets[i].preset_id;
            break;
        }
    }
    if (!wanted || !wanted[0]) return -1;

    for (int i = 0; i < preset_count; i++) {
        if (strcmp(presets[i].id, wanted) == 0) return i;
    }
    return -1;
}

void bambuddy_filament_effective_temps(const bambuddy_filament_preset_t &preset,
                                       int16_t *lo, int16_t *hi)
{
    if (!lo || !hi) return;

    if (preset.temp_min > 0 && preset.temp_max > 0) {
        *lo = preset.temp_min;
        *hi = preset.temp_max;
        return;
    }
    material_temps(preset.material, lo, hi);
}

const char *bambuddy_filament_slot_preset_name(int32_t ams_id, int32_t tray_id)
{
    for (int i = 0; i < slot_preset_count; i++) {
        if (slot_presets[i].ams_id == ams_id && slot_presets[i].tray_id == tray_id) {
            return slot_presets[i].preset_name;
        }
    }
    return "";
}

void bambuddy_filament_request_configure(int32_t ams_id, int32_t tray_id,
                                         int preset_index, uint32_t color_rgb)
{
    if (preset_index < 0) return;

    request.ams_id = ams_id;
    request.tray_id = tray_id;
    request.preset_index = preset_index;
    request.color_rgb = color_rgb;
    request.pending = true;
}

void bambuddy_filament_request_reset(int32_t ams_id, int32_t tray_id)
{
    request.ams_id = ams_id;
    request.tray_id = tray_id;
    request.preset_index = -1;
    request.color_rgb = 0;
    request.pending = true;
}

bool bambuddy_filament_busy()
{
    return busy || request.pending;
}

const char *bambuddy_filament_message()
{
    return message;
}

uint32_t bambuddy_filament_message_age()
{
    return message_ms ? millis() - message_ms : UINT32_MAX;
}
