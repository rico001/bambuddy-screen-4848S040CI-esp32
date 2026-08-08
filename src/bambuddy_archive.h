#pragma once

#include <stddef.h>
#include <stdint.h>

// Fuenf umfangreiche LVGL-Zeilen lassen genug Platz fuer Aktionsdialoge.
#define BB_ARCHIVE_PAGE_SIZE 5

struct bambuddy_archive_item_t {
    int32_t id;
    char name[64];
    int32_t print_seconds;
    float grams;
    char filament[16];
    uint32_t color;
    char status[16];
    int32_t run_count;
};

void bambuddy_archive_set_visible(bool visible);
bool bambuddy_archive_visible();

void bambuddy_archive_update();

int bambuddy_archive_copy(bambuddy_archive_item_t *out, int max_items);
bool bambuddy_archive_take_fresh();

int bambuddy_archive_current_page();
bool bambuddy_archive_has_prev_page();
bool bambuddy_archive_has_next_page();
void bambuddy_archive_prev_page();
void bambuddy_archive_next_page();

void bambuddy_archive_request_reprint(int32_t archive_id, bool clear_plate);
void bambuddy_archive_request_delete(int32_t archive_id);

const char *bambuddy_archive_message();
uint32_t bambuddy_archive_message_age();
