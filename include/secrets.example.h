#pragma once

// Vorlage fuer include/secrets.h — Datei kopieren, Werte eintragen.
// secrets.h ist in .gitignore und landet nicht im Repo.
//
// Diese Werte sind nur Startwerte: Sie werden beim allerersten Boot ins NVS
// geschrieben. Danach gilt immer, was im Einstellungs-Screen steht.

// Im Heimnetz http:// empfohlen. Jeder https-Abruf handelt eine TLS-Sitzung
// aus; das kostet auf diesem Board so viel Rechenzeit und Speicherbandbreite,
// dass das Bild sichtbar "zuckt". Der Schalter "Zertifikat pruefen" hilft
// dagegen nicht — er ueberspringt nur die Pruefung, der Handshake bleibt.
// Fuer den Zugriff von aussen (Tunnel, Reverse Proxy) natuerlich https, dann
// aber im Einstellungs-Screen eintragen statt hier.
#define BAMBUDDY_DEFAULT_URL        "http://192.168.178.23:2342"
#define BAMBUDDY_DEFAULT_API_KEY    "bb_...."
#define BAMBUDDY_DEFAULT_PRINTER_ID 1
#define BAMBUDDY_DEFAULT_CAM_TOKEN  "bblt_..."

#define BAMBUDDY_DEFAULT_MQTT_HOST  "192.168.178.10"
#define BAMBUDDY_DEFAULT_MQTT_PORT  1883
#define BAMBUDDY_DEFAULT_MQTT_USER  ""
#define BAMBUDDY_DEFAULT_MQTT_PASS  ""
#define BAMBUDDY_DEFAULT_MQTT_TOPIC "bambuddy/printers/<PRINTER_SERIENNUMMER>/status"

// 1 = Status per MQTT (empfohlen), 0 = per HTTP abfragen
#define BAMBUDDY_DEFAULT_SOURCE_MQTT 0
