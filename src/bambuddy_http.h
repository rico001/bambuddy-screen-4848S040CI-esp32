#pragma once

#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

// Verbindung passend zum URL-Schema.
//
// Die Bambuddy-Instanz kann per https (ueber einen Tunnel) oder per http
// (lokal im Netz) erreichbar sein. Ein TLS-Client an einem Klartext-Port
// scheitert schon beim Handshake — deshalb entscheidet das Schema der URL,
// welcher Client benutzt wird, und nicht eine feste Annahme im Code.
class BambuddyHttp {
public:
    // Setzt zugleich den API-Key-Header: den brauchen alle Endpunkte ausser
    // den Bild-Abrufen, die ueber den Kamera-Token in der URL gehen — dort
    // stoert er nicht. Damit steht die Anmeldung an genau einer Stelle.
    // keep_alive: Verbindung nach end(false) offen halten (fuer Bildfolgen).
    bool begin(const char *url, bool keep_alive = false);

    // Fertige Anfragen ohne Rumpf. Liefern den HTTP-Code (< 0 = Transportfehler).
    int get();
    int post(const char *body = "");
    int del();

    HTTPClient &http() { return http_; }

    // close = true trennt auch die TCP/TLS-Verbindung.
    void end(bool close = true);

    // Bricht einen gerade blockierenden Transfer durch Schliessen des
    // zugrunde liegenden Sockets ab. Das Objekt bleibt danach verwendbar.
    void cancel();

    bool is_tls() const { return is_tls_; }

private:
    void add_auth();

    HTTPClient http_;
    WiFiClient plain_;
    WiFiClientSecure secure_;
    bool is_tls_ = false;
};
