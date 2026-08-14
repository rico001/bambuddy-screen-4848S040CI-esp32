#pragma once

#include <lvgl.h>

// Navigationsleiste am unteren Rand.
//
// Fuenf Symbole mit Beschriftung, eines je Kachel. Antippen springt direkt
// hin, statt sich durch die Zwischenkacheln zu wischen — vom AMS zum System
// waren das bisher vier Wischgesten.
//
// Die Leiste liegt neben dem Tileview auf dem Bildschirm, nicht darin: Sie
// soll stehen bleiben, waehrend die Kacheln darunter durchlaufen.

#define NAV_TILE_COUNT 5

void nav_bar_create(lv_obj_t *parent);

// Hervorhebung auf diese Kachel setzen (0 = AMS ... 4 = System). Wird beim
// Wischen ebenso gerufen wie beim Antippen — sonst zeigte die Leiste nach
// einer Wischgeste noch auf die vorherige Kachel.
void nav_bar_set_active(int index);
