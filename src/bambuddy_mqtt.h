#pragma once

// Statusdaten per MQTT statt HTTP-Polling. Bambuddy veroeffentlicht den
// Druckerstatus selbst — damit kommt jede Aenderung sofort an, statt dass
// das Display alle paar Sekunden nachfragt.
//
// Laeuft in einem eigenen Task, nicht im LVGL-Thread und auch nicht im
// Netzwerk-Task der HTTP-Abrufe.

// Startet den MQTT-Task. Bewusst ein eigener Task: MQTT muss alle paar
// Millisekunden bedient werden, damit der Broker die Verbindung haelt.
// Teilt es sich den Task mit den HTTP-Abrufen, reicht ein einziger langsamer
// Request, um die Verbindung abreissen zu lassen — und man sieht es nicht
// einmal, weil der zuletzt gemeldete Zustand stehen bleibt.
void bambuddy_mqtt_start();

// Verbindungsaufbau, Abo und Umschalten der Quelle erledigt der Task
// selbst — nach aussen gibt es deshalb nichts weiter zu steuern. Den
// Verbindungszustand meldet das Modul ueber bambuddy_api_report_link().
