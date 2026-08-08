#pragma once

#include <ArduinoJson.h>
#include <string.h>

#include "bambuddy_api.h"

// HTTP-Antwort und MQTT-Payload tragen dieselben Feldnamen — deshalb wertet
// beides dieselbe Funktion aus. Einziger Unterschied: im MQTT-Payload heisst
// der Druckername "printer_name", ueber HTTP schlicht "name".

// Farbangaben der API sind "#RRGGBBAA" (Auftraege) oder "RRGGBBAA" (AMS).
// Beide Formen landen als 0xRRGGBB, Grau als Rueckfallwert.
static inline uint32_t bambuddy_parse_hex_color(const char *value)
{
    if (!value) return 0x9E9E9E;
    if (*value == '#') value++;
    if (strlen(value) < 6) return 0x9E9E9E;

    char buf[7];
    memcpy(buf, value, 6);
    buf[6] = '\0';
    return (uint32_t)strtoul(buf, nullptr, 16);
}

static inline void bambuddy_copy_field(char *dst, size_t dst_len, const char *src)
{
    strncpy(dst, src ? src : "", dst_len - 1);
    dst[dst_len - 1] = '\0';
}

static inline uint8_t bambuddy_hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

static inline uint32_t bambuddy_parse_color(const char *text)
{
    if (!text) return 0x546E7A;
    if (text[0] == '#') text++;
    if (strlen(text) < 6) return 0x546E7A;

    uint32_t color = 0;
    for (int i = 0; i < 6; i++) {
        color = (color << 4) | bambuddy_hex_nibble(text[i]);
    }
    return color;
}

static inline void bambuddy_status_from_json(JsonDocument &doc, bambuddy_status_t *out)
{
    memset(out, 0, sizeof(*out));

    out->printer_connected = doc["connected"] | false;

    const char *name = doc["name"] | doc["printer_name"] | "Drucker";
    bambuddy_copy_field(out->name, sizeof(out->name), name);
    bambuddy_copy_field(out->state, sizeof(out->state), doc["state"] | "");
    bambuddy_copy_field(out->job, sizeof(out->job), doc["subtask_name"] | "");

    out->progress = doc["progress"] | 0.0f;
    out->remaining_min = doc["remaining_time"] | 0;
    out->layer = doc["layer_num"] | 0;
    out->total_layers = doc["total_layers"] | 0;

    JsonObject temps = doc["temperatures"];
    out->nozzle = temps["nozzle"] | 0.0f;
    out->nozzle_target = temps["nozzle_target"] | 0.0f;
    out->bed = temps["bed"] | 0.0f;
    out->bed_target = temps["bed_target"] | 0.0f;

    out->chamber_light = doc["chamber_light"] | false;
    out->ams_data_present = !doc["ams"].isNull();
    out->ams_exists = doc["ams_exists"] | false;
    out->tray_now = doc["tray_now"] | 255;

    for (JsonObject unit_json : doc["ams"].as<JsonArray>()) {
        if (out->ams_count >= BB_AMS_MAX_UNITS) break;

        bambuddy_ams_unit_t &unit = out->ams[out->ams_count++];
        unit.id = unit_json["id"] | (out->ams_count - 1);
        unit.humidity = unit_json["humidity"].isNull()
                            ? -1
                            : unit_json["humidity"].as<int32_t>();
        unit.temperature_known = !unit_json["temp"].isNull();
        unit.temperature = unit_json["temp"] | 0.0f;
        unit.is_ht = unit_json["is_ams_ht"] | false;

        for (JsonObject tray_json : unit_json["tray"].as<JsonArray>()) {
            const int32_t tray_id = tray_json["id"] | unit.tray_count;
            if (tray_id < 0 || tray_id >= BB_AMS_MAX_TRAYS) continue;

            bambuddy_ams_tray_t &tray = unit.trays[tray_id];
            tray.id = tray_id;
            const char *type = tray_json["tray_type"] | "";
            bambuddy_copy_field(tray.type, sizeof(tray.type), type);
            tray.exists = tray_json["exists"].isNull()
                              ? tray.type[0] != '\0'
                              : tray_json["exists"].as<bool>();
            tray.color = bambuddy_parse_color(tray_json["tray_color"] | "");
            tray.remain = tray_json["remain"] | 0;
            if (tray.remain < 0) tray.remain = 0;
            if (tray.remain > 100) tray.remain = 100;
            if (tray_id + 1 > unit.tray_count) unit.tray_count = tray_id + 1;
        }
    }
    out->awaiting_plate_clear = doc["awaiting_plate_clear"] | false;
}
