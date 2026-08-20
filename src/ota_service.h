#pragma once

#include <stdint.h>

// Firmware-Update ueber eine kleine Webseite auf dem Geraet.
//
// Das Display haengt an der Wand; ein Update per USB heisst abschrauben.
// Ist der Dienst eingeschaltet, laeuft auf Port 80 ein Webserver, der eine
// Seite mit den Angaben zur laufenden Firmware und einem Upload-Feld fuer
// eine `firmware.bin` ausliefert. Geschrieben wird in die freie der beiden
// OTA-Partitionen; nach dem letzten Byte startet das Geraet neu und bootet
// von dort.
//
// Standardmaessig aus. Wer den Schalter umlegt, oeffnet einen Weg, ueber den
// jeder im selben WLAN beliebige Firmware aufspielen kann — es gibt kein
// Passwort. Das ist Absicht: ein Passwort auf diesem Display einzugeben ist
// laestiger als der Schalter, und der Schalter ist die ehrlichere Aussage.
//
// Daneben liegt unter /screen eine Bedienseite: Sie zeigt das Display, so wie
// es im Moment des Abrufs aussieht (das Bild allein unter /screenshot.bmp),
// und schaltet die Kacheln um. Damit laesst sich ein Geraet an der Wand
// ansehen und bedienen, ohne davorzustehen.
//
// Der Dienst laeuft in einem eigenen Task auf Core 0, nicht im LVGL-Loop:
// Ein Upload haelt den Server ueber Sekunden beschaeftigt, und der
// UI-Watchdog in loop() schlaegt nach zehn Sekunden zu.

// Dienst an- oder abschalten. Darf jederzeit gerufen werden, auch bevor das
// WLAN steht — der Task wartet selbst auf die Verbindung.
void ota_service_set_enabled(bool on);
bool ota_service_enabled();

// Laeuft der Server gerade und ist damit erreichbar?
bool ota_service_online();

// "http://192.168.1.23" solange erreichbar, sonst "".
const char *ota_service_address();

// Fortschritt eines laufenden Uploads in Prozent, -1 wenn keiner laeuft.
int ota_service_progress();

// Klartext zum letzten Versuch: "" solange nichts passiert ist, sonst der
// Grund des Abbruchs oder die Erfolgsmeldung.
const char *ota_service_message();
