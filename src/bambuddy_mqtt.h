#pragma once

// Statusdaten per MQTT statt HTTP-Polling. Bambuddy veroeffentlicht den
// Druckerstatus selbst — damit kommt jede Aenderung sofort an, statt dass
// das Display alle paar Sekunden nachfragt.
//
// Alle Funktionen laufen im Netzwerk-Task, nicht im LVGL-Thread.

// Haelt die Verbindung am Leben und verarbeitet eingehende Nachrichten.
// Muss haeufig aufgerufen werden (alle ~50 ms).
void bambuddy_mqtt_loop();

// Trennt die Verbindung und gibt den Client frei (beim Umschalten auf HTTP).
void bambuddy_mqtt_stop();

bool bambuddy_mqtt_connected();
