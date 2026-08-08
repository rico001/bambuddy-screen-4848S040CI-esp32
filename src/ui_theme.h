#pragma once

#include <stdint.h>

// Gemeinsame Farbbedeutungen.
//
// Diese fuenf Werte lagen vorher in jeder Screen-Datei einzeln — zehnmal
// dieselbe Zeile. Sobald ein Ton angepasst wird, driften solche Kopien
// auseinander, und man sieht es erst auf dem Geraet.
//
// Screen-eigene Akzente (Kachelfarben, Materialfarben) bleiben bewusst
// lokal: die gelten nur an einer Stelle und tragen keine allgemeine
// Bedeutung.

static constexpr uint32_t COL_MUTED = 0x9E9E9E;   // Nebeninformation
static constexpr uint32_t COL_OK = 0x4CAF50;      // laeuft, verbunden, fertig
static constexpr uint32_t COL_WARN = 0xFFB300;    // Aufmerksamkeit noetig
static constexpr uint32_t COL_ERR = 0xE53935;     // Fehler, zerstoerende Aktion
static constexpr uint32_t COL_ACCENT = 0x2196F3;  // Hervorhebung, aktiver Wert
static constexpr uint32_t COL_NEUTRAL = 0x546E7A; // zurueckhaltender Knopf
