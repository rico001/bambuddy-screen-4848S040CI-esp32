#pragma once

#include <stdint.h>

// Gemeinsame Farbbedeutungen und Massangaben.
//
// Diese Werte lagen vorher in jeder Screen-Datei einzeln — zehnmal dieselbe
// Zeile. Sobald ein Ton angepasst wird, driften solche Kopien auseinander,
// und man sieht es erst auf dem Geraet.
//
// Screen-eigene Akzente (Kachelfarben, Materialfarben) bleiben bewusst
// lokal: die gelten nur an einer Stelle und tragen keine allgemeine
// Bedeutung.
//
// --- Zur Gestaltung ------------------------------------------------------
//
// Flaechig, nicht verlaufend. Der Bildpuffer liegt im PSRAM, aus dem das
// Panel gleichzeitig per DMA liest; jede Neuzeichnung konkurriert mit diesem
// Datenstrom. Gefuellte Rechtecke mit Radius kosten dabei praktisch nichts,
// Verlaeufe und Schatten ueber grosse Flaechen dagegen sichtbar Bildruhe.
// Die Anmutung entsteht deshalb aus Farbabstufung, Abstand und Typografie —
// nicht aus Effekten.

// --- Hell oder dunkel -----------------------------------------------------
//
// Die Werte unten sind keine Konstanten mehr, sondern Variablen: Beim Start
// legt ui_theme_set_dark() fest, welcher Satz gilt. Anders ginge es nicht —
// ein helles Schema mit dunklen Kartenfarben waere kein Schema, sondern ein
// Fehler, und die Farben stecken inzwischen in jedem Screen.
//
// Umgeschaltet wird nur beim Start: Die Screens tragen ihre Farben in den
// Objekten, und die bestehen weiter, bis sie neu gebaut werden. Der Schalter
// in den Einstellungen startet das Geraet deshalb neu — sichtbar angekuendigt,
// statt eine halb umgefaerbte Oberflaeche zu hinterlassen.
void ui_theme_set_dark(bool dark);
bool ui_theme_is_dark();

// --- Flaechen -------------------------------------------------------------
// Drei Ebenen, von unten nach oben heller. Mehr braucht es nicht: Wer eine
// vierte einfuehrt, unterscheidet Toene, die man auf diesem Panel bei 30 %
// Helligkeit nicht mehr auseinanderhaelt.
extern uint32_t COL_BG;      // Bildschirmgrund
extern uint32_t COL_SURFACE; // Karten
extern uint32_t COL_RAISED;  // Elemente auf Karten
extern uint32_t COL_LINE;    // Raender und Trenner

// --- Schrift --------------------------------------------------------------
extern uint32_t COL_TEXT;  // Hauptaussage
extern uint32_t COL_MUTED; // Nebeninformation

// --- Bedeutungen ----------------------------------------------------------
extern uint32_t COL_OK;      // laeuft, verbunden, fertig
extern uint32_t COL_WARN;    // Aufmerksamkeit noetig
extern uint32_t COL_ERR;     // Fehler, zerstoerende Aktion
extern uint32_t COL_ACCENT;  // Hervorhebung, aktiver Wert
extern uint32_t COL_NEUTRAL; // zurueckhaltender Knopf
extern uint32_t COL_PLUG;    // Smart Plugs, ueberall gleich
extern uint32_t COL_JOG;     // Jog-Steuerung, ueberall gleich

// --- Masse ----------------------------------------------------------------
// Eine Stufenleiter statt frei gewaehlter Zahlen: Abstaende, die sich um zwei
// Pixel unterscheiden, sieht niemand als Absicht — nur als Unruhe.
static constexpr int GAP_XS = 4;
static constexpr int GAP_S = 8;
static constexpr int GAP_M = 12;
static constexpr int GAP_L = 16;
static constexpr int GAP_XL = 24;

static constexpr int RADIUS_CARD = 16;
static constexpr int RADIUS_CTRL = 12; // Knoepfe, Eingabefelder
static constexpr int RADIUS_PILL = 999; // wird auf halbe Hoehe begrenzt
