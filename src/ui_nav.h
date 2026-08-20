#pragma once

// Sprünge zwischen Kacheln.
//
// Die Kacheln kennt nur main.cpp — die Screens sollen sich nicht gegenseitig
// oder das Tileview kennen muessen. Deshalb liegt hier die schmale
// Schnittstelle dafuer, umgesetzt in main.cpp.

// Direkt auf eine Kachel springen (0 = AMS ... 4 = System). Fuer die
// Navigationsleiste am unteren Rand. Nur aus dem LVGL-Thread rufen.
void ui_nav_tile(int index);

// Dasselbe aus einem fremden Task anfordern — die Webseite des Geraets
// schaltet darueber die Kacheln um. LVGL ist nicht thread-fest, deshalb wird
// hier nur vermerkt, wohin es gehen soll.
void ui_nav_tile_from_task(int index);

// Im LVGL-Thread rufen: fuehrt einen angeforderten Wechsel aus.
void ui_nav_poll();

// Welche Kachel gerade vorne ist (0 ... 4), oder -1, solange keine steht.
// Wird beim Wechsel mitgefuehrt und darf aus jedem Task gelesen werden.
int ui_nav_active_tile();

// Wechselt auf die Systemkachel und oeffnet dort direkt die Smart Plugs.
void ui_nav_smart_plugs();

// Dasselbe fuer die Jog-Steuerung.
void ui_nav_jog();

// Protokoll der Druckermeldungen. Sitzt bewusst nicht mehr auf der
// Systemkachel: Was der Drucker gemeldet hat, sucht man beim Drucker.
void ui_nav_messages();
