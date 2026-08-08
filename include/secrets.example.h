#pragma once

// Vorlage fuer include/secrets.h — Datei kopieren, Werte eintragen.
// secrets.h ist in .gitignore und landet nicht im Repo.
//
// Diese Werte sind nur Startwerte: Sie werden beim allerersten Boot ins NVS
// geschrieben. Danach gilt immer, was im Einstellungs-Screen steht.

#define BAMBUDDY_DEFAULT_URL        "https://bambuddy.example.com"
#define BAMBUDDY_DEFAULT_API_KEY    ""
#define BAMBUDDY_DEFAULT_PRINTER_ID 1
#define BAMBUDDY_DEFAULT_CAM_TOKEN  ""

#define BAMBUDDY_DEFAULT_MQTT_HOST  ""
#define BAMBUDDY_DEFAULT_MQTT_PORT  1883
#define BAMBUDDY_DEFAULT_MQTT_USER  ""
#define BAMBUDDY_DEFAULT_MQTT_PASS  ""
#define BAMBUDDY_DEFAULT_MQTT_TOPIC "bambuddy/printers/<SERIENNUMMER>/status"
