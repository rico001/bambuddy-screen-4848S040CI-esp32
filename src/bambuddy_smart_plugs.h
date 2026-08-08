#pragma once

#include <stdint.h>

#define BB_SMART_PLUG_MAX_ITEMS 6

struct bambuddy_smart_plug_t {
    int32_t id;
    char name[64];
    bool state_known;
    bool is_on;
    bool reachable;
};

void bambuddy_smart_plugs_set_visible(bool visible);
bool bambuddy_smart_plugs_visible();
void bambuddy_smart_plugs_update();

int bambuddy_smart_plugs_copy(bambuddy_smart_plug_t *out, int max_items);
bool bambuddy_smart_plugs_has_data();
bool bambuddy_smart_plugs_take_fresh();
void bambuddy_smart_plugs_request_control(int32_t plug_id, bool turn_on);

const char *bambuddy_smart_plugs_message();
uint32_t bambuddy_smart_plugs_message_age();
