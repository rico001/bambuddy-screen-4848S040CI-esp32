#pragma once

// Sprünge zwischen Kacheln.
//
// Die Kacheln kennt nur main.cpp — die Screens sollen sich nicht gegenseitig
// oder das Tileview kennen muessen. Deshalb liegt hier die schmale
// Schnittstelle dafuer, umgesetzt in main.cpp.

// Direkt auf eine Kachel springen (0 = AMS ... 4 = System). Fuer die
// Navigationsleiste am unteren Rand.
void ui_nav_tile(int index);

// Wechselt auf die Systemkachel und oeffnet dort direkt die Smart Plugs.
void ui_nav_smart_plugs();

// Dasselbe fuer die Jog-Steuerung.
void ui_nav_jog();
