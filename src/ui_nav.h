#pragma once

// Sprünge zwischen Kacheln.
//
// Die Kacheln kennt nur main.cpp — die Screens sollen sich nicht gegenseitig
// oder das Tileview kennen muessen. Deshalb liegt hier die schmale
// Schnittstelle dafuer, umgesetzt in main.cpp.

// Wechselt auf die Systemkachel und oeffnet dort direkt die Smart Plugs.
void ui_nav_smart_plugs();

// Dasselbe fuer die Jog-Steuerung.
void ui_nav_jog();

// Zurueck auf die Statuskachel — fuer den Rueckweg aus einer Ansicht, die
// von dort aus aufgerufen wurde.
void ui_nav_status();
