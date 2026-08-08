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
    // keep_alive: Verbindung nach end(false) offen halten (fuer Bildfolgen).
    bool begin(const char *url, bool keep_alive = false);

    HTTPClient &http() { return http_; }

    // close = true trennt auch die TCP/TLS-Verbindung.
    void end(bool close = true);

    bool is_tls() const { return is_tls_; }

private:
    HTTPClient http_;
    WiFiClient plain_;
    WiFiClientSecure secure_;
    bool is_tls_ = false;
};
