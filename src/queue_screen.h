#pragma once

#include <lvgl.h>

// Druckwarteschlange: anstehende Auftraege mit Startknopf.
void queue_screen_create(lv_obj_t *parent);

// Wird von main.cpp gerufen, wenn die Kachel ein- oder ausgeblendet wird.
// Abgefragt wird nur, solange der Screen tatsaechlich zu sehen ist.
void queue_screen_set_visible(bool visible);
