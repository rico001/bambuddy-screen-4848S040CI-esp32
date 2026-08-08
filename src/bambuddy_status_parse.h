#pragma once

#include <ArduinoJson.h>
#include <string.h>

#include "bambuddy_api.h"

// HTTP-Antwort und MQTT-Payload tragen dieselben Feldnamen — deshalb wertet
// beides dieselbe Funktion aus. Einziger Unterschied: im MQTT-Payload heisst
// der Druckername "printer_name", ueber HTTP schlicht "name".

static inline void bambuddy_copy_field(char *dst, size_t dst_len, const char *src)
{
    strncpy(dst, src ? src : "", dst_len - 1);
    dst[dst_len - 1] = '\0';
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

    out->awaiting_plate_clear = doc["awaiting_plate_clear"] | false;
}
