#include "bambuddy_config.h"

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "secrets.h"

// NVS-Schluessel duerfen maximal 15 Zeichen lang sein
static constexpr const char *NS = "bambuddy";
static constexpr const char *K_URL = "url";
static constexpr const char *K_KEY = "key";
static constexpr const char *K_PID = "pid";
static constexpr const char *K_CAM = "cam";
static constexpr const char *K_MHOST = "mhost";
static constexpr const char *K_MPORT = "mport";
static constexpr const char *K_MUSER = "muser";
static constexpr const char *K_MPASS = "mpass";
static constexpr const char *K_MTOPIC = "mtopic";
static constexpr const char *K_SRC = "src";

static char cfg_url[129];
static char cfg_key[97];
static char cfg_cam[97];
static int cfg_printer_id = 1;

static char cfg_mqtt_host[65];
static char cfg_mqtt_user[33];
static char cfg_mqtt_pass[65];
static char cfg_mqtt_topic[129];
static int cfg_mqtt_port = 1883;
static bool cfg_source_mqtt = false;

static char url_buf[256];

static constexpr uint32_t HOST_CACHE_MS = 600000; // 10 Minuten

static char cfg_url_resolved[129];
static uint32_t resolved_at_ms = 0;

static bool looks_like_ip(const char *host)
{
    for (const char *p = host; *p; p++) {
        if (*p != '.' && (*p < '0' || *p > '9')) return false;
    }
    return host[0] != '\0';
}

void bambuddy_config_forget_host()
{
    cfg_url_resolved[0] = '\0';
    resolved_at_ms = 0;
}

static const char *effective_base_url()
{
    if (strncmp(cfg_url, "http://", 7) != 0) return cfg_url; // https: Name behalten

    if (cfg_url_resolved[0] && (millis() - resolved_at_ms) < HOST_CACHE_MS) {
        return cfg_url_resolved;
    }

    const char *host_start = cfg_url + 7;
    const char *host_end = host_start;
    while (*host_end && *host_end != ':' && *host_end != '/') host_end++;

    char host[80];
    const size_t host_len = (size_t)(host_end - host_start);
    if (host_len == 0 || host_len >= sizeof(host)) return cfg_url;
    memcpy(host, host_start, host_len);
    host[host_len] = '\0';

    if (looks_like_ip(host)) return cfg_url;
    if (WiFi.status() != WL_CONNECTED) return cfg_url;

    IPAddress ip;
    if (!WiFi.hostByName(host, ip)) return cfg_url; // beim naechsten Mal wieder

    snprintf(cfg_url_resolved, sizeof(cfg_url_resolved), "http://%s%s",
             ip.toString().c_str(), host_end);
    resolved_at_ms = millis();
    Serial.printf("[Bambuddy] %s aufgeloest zu %s\n", host, cfg_url_resolved);
    return cfg_url_resolved;
}

static Preferences prefs;

static void copy_str(char *dst, size_t dst_len, const char *src)
{
    strncpy(dst, src ? src : "", dst_len - 1);
    dst[dst_len - 1] = '\0';
}

// Abschliessende Slashes entfernen, damit beim Zusammenbauen keine
// doppelten entstehen ("https://host/" + "/printers" -> "//printers").
static void copy_trimmed_url(char *dst, size_t dst_len, const char *src)
{
    strncpy(dst, src, dst_len - 1);
    dst[dst_len - 1] = '\0';

    size_t len = strlen(dst);
    while (len > 0 && (dst[len - 1] == '/' || dst[len - 1] == ' ')) {
        dst[--len] = '\0';
    }
}

// Fehlende Schluessel einzeln aus secrets.h nachziehen — nicht ueber ein
// globales "schon initialisiert"-Flag. Sonst bekommt ein Geraet, das schon
// einmal geflasht wurde, spaeter hinzugefuegte Felder nie mit.
// Ein bewusst geleertes Feld bleibt leer: der Schluessel existiert dann ja.
static String load_or_seed(const char *key, const char *fallback)
{
    if (!prefs.isKey(key)) {
        prefs.putString(key, fallback);
        Serial.printf("[Bambuddy] '%s' mit Startwert belegt\n", key);
        return String(fallback);
    }
    return prefs.getString(key, fallback);
}

static bool load_or_seed_bool(const char *key, bool fallback)
{
    if (!prefs.isKey(key)) {
        prefs.putBool(key, fallback);
        Serial.printf("[Bambuddy] '%s' mit Startwert belegt\n", key);
        return fallback;
    }
    return prefs.getBool(key, fallback);
}

static int load_or_seed_int(const char *key, int fallback)
{
    if (!prefs.isKey(key)) {
        prefs.putInt(key, fallback);
        Serial.printf("[Bambuddy] '%s' mit Startwert belegt\n", key);
        return fallback;
    }
    return prefs.getInt(key, fallback);
}

void bambuddy_config_load()
{
    prefs.begin(NS, false);

    String url = load_or_seed(K_URL, BAMBUDDY_DEFAULT_URL);
    String key = load_or_seed(K_KEY, BAMBUDDY_DEFAULT_API_KEY);
    String cam = load_or_seed(K_CAM, BAMBUDDY_DEFAULT_CAM_TOKEN);
    String mhost = load_or_seed(K_MHOST, BAMBUDDY_DEFAULT_MQTT_HOST);
    String muser = load_or_seed(K_MUSER, BAMBUDDY_DEFAULT_MQTT_USER);
    String mpass = load_or_seed(K_MPASS, BAMBUDDY_DEFAULT_MQTT_PASS);
    String mtopic = load_or_seed(K_MTOPIC, BAMBUDDY_DEFAULT_MQTT_TOPIC);
    cfg_printer_id = load_or_seed_int(K_PID, BAMBUDDY_DEFAULT_PRINTER_ID);
    cfg_mqtt_port = load_or_seed_int(K_MPORT, BAMBUDDY_DEFAULT_MQTT_PORT);
    cfg_source_mqtt = load_or_seed_bool(K_SRC, BAMBUDDY_DEFAULT_SOURCE_MQTT);

    prefs.end();

    copy_trimmed_url(cfg_url, sizeof(cfg_url), url.c_str());
    copy_str(cfg_key, sizeof(cfg_key), key.c_str());
    copy_str(cfg_cam, sizeof(cfg_cam), cam.c_str());
    copy_str(cfg_mqtt_host, sizeof(cfg_mqtt_host), mhost.c_str());
    copy_str(cfg_mqtt_user, sizeof(cfg_mqtt_user), muser.c_str());
    copy_str(cfg_mqtt_pass, sizeof(cfg_mqtt_pass), mpass.c_str());
    copy_str(cfg_mqtt_topic, sizeof(cfg_mqtt_topic), mtopic.c_str());

    Serial.printf("[Bambuddy] Quelle=%s, %s, Drucker %d, API-Key %s\n",
                  cfg_source_mqtt ? "MQTT" : "HTTP",
                  cfg_url, cfg_printer_id, cfg_key[0] ? "gesetzt" : "FEHLT");
    Serial.printf("[Bambuddy] MQTT %s:%d Benutzer '%s' Passwort %s Topic '%s'\n",
                  cfg_mqtt_host, cfg_mqtt_port, cfg_mqtt_user,
                  cfg_mqtt_pass[0] ? "gesetzt" : "FEHLT", cfg_mqtt_topic);
}

const char *bambuddy_base_url() { return effective_base_url(); }
const char *bambuddy_configured_base_url() { return cfg_url; }
const char *bambuddy_api_key() { return cfg_key; }
const char *bambuddy_cam_token() { return cfg_cam; }
int bambuddy_printer_id() { return cfg_printer_id; }

void bambuddy_set_base_url(const char *value)
{
    copy_trimmed_url(cfg_url, sizeof(cfg_url), value ? value : "");
    bambuddy_config_forget_host();
    prefs.begin(NS, false);
    prefs.putString(K_URL, cfg_url);
    prefs.end();
}

void bambuddy_set_api_key(const char *value)
{
    strncpy(cfg_key, value ? value : "", sizeof(cfg_key) - 1);
    cfg_key[sizeof(cfg_key) - 1] = '\0';
    prefs.begin(NS, false);
    prefs.putString(K_KEY, cfg_key);
    prefs.end();
}

void bambuddy_set_cam_token(const char *value)
{
    strncpy(cfg_cam, value ? value : "", sizeof(cfg_cam) - 1);
    cfg_cam[sizeof(cfg_cam) - 1] = '\0';
    prefs.begin(NS, false);
    prefs.putString(K_CAM, cfg_cam);
    prefs.end();
}

void bambuddy_set_printer_id(const char *value)
{
    const int id = value ? atoi(value) : 0;
    if (id <= 0) return; // ungueltige Eingabe ignorieren, alter Wert bleibt

    cfg_printer_id = id;
    prefs.begin(NS, false);
    prefs.putInt(K_PID, cfg_printer_id);
    prefs.end();
}

// --- MQTT ----------------------------------------------------------------

const char *bambuddy_mqtt_host() { return cfg_mqtt_host; }
const char *bambuddy_mqtt_user() { return cfg_mqtt_user; }
const char *bambuddy_mqtt_pass() { return cfg_mqtt_pass; }
const char *bambuddy_mqtt_topic() { return cfg_mqtt_topic; }
int bambuddy_mqtt_port() { return cfg_mqtt_port; }
bool bambuddy_source_mqtt() { return cfg_source_mqtt; }

// Ein Setter fuer alle Textwerte: Wert uebernehmen und sofort ins NVS,
// damit ein Stromausfall direkt nach der Eingabe nichts verschluckt.
static void store_string(char *dst, size_t dst_len, const char *key, const char *value)
{
    copy_str(dst, dst_len, value);
    prefs.begin(NS, false);
    prefs.putString(key, dst);
    prefs.end();
}

void bambuddy_set_mqtt_host(const char *value)
{
    store_string(cfg_mqtt_host, sizeof(cfg_mqtt_host), K_MHOST, value);
}

void bambuddy_set_mqtt_user(const char *value)
{
    store_string(cfg_mqtt_user, sizeof(cfg_mqtt_user), K_MUSER, value);
}

void bambuddy_set_mqtt_pass(const char *value)
{
    store_string(cfg_mqtt_pass, sizeof(cfg_mqtt_pass), K_MPASS, value);
}

void bambuddy_set_mqtt_topic(const char *value)
{
    store_string(cfg_mqtt_topic, sizeof(cfg_mqtt_topic), K_MTOPIC, value);
}

void bambuddy_set_mqtt_port(const char *value)
{
    const int port = value ? atoi(value) : 0;
    if (port <= 0 || port > 65535) return; // ungueltig: alter Wert bleibt

    cfg_mqtt_port = port;
    prefs.begin(NS, false);
    prefs.putInt(K_MPORT, cfg_mqtt_port);
    prefs.end();
}

void bambuddy_set_source_mqtt(bool use_mqtt)
{
    cfg_source_mqtt = use_mqtt;
    prefs.begin(NS, false);
    prefs.putBool(K_SRC, cfg_source_mqtt);
    prefs.end();
}

bool bambuddy_config_complete()
{
    return cfg_url[0] != '\0' && cfg_key[0] != '\0' && cfg_printer_id > 0;
}

bool bambuddy_mqtt_config_complete()
{
    return cfg_mqtt_host[0] != '\0' && cfg_mqtt_topic[0] != '\0' && cfg_mqtt_port > 0;
}

const char *bambuddy_url(const char *path_fmt, ...)
{
    const int base_len =
        snprintf(url_buf, sizeof(url_buf), "%s/api/v1", effective_base_url());
    if (base_len < 0 || (size_t)base_len >= sizeof(url_buf)) {
        url_buf[0] = '\0';
        return url_buf;
    }

    va_list args;
    va_start(args, path_fmt);
    vsnprintf(url_buf + base_len, sizeof(url_buf) - base_len, path_fmt, args);
    va_end(args);

    return url_buf;
}
