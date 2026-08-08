#include "bambuddy_http.h"

#include <Arduino.h>
#include <string.h>

#include "bambuddy_config.h"
#include "certs/isrg_roots.h"
#include "settings_screen.h"

static constexpr uint32_t HTTP_TIMEOUT_MS = 8000;

bool BambuddyHttp::begin(const char *url, bool keep_alive)
{
    if (!url || url[0] == '\0') return false;

    is_tls_ = (strncmp(url, "https://", 8) == 0);

    http_.setTimeout(HTTP_TIMEOUT_MS);
    http_.setConnectTimeout(HTTP_TIMEOUT_MS);
    http_.setReuse(keep_alive);

    if (is_tls_) {
        if (settings_tls_verify()) {
            secure_.setCACert(ISRG_ROOT_CERTS);
        } else {
            secure_.setInsecure();
        }
        secure_.setTimeout(HTTP_TIMEOUT_MS / 1000);
        if (!http_.begin(secure_, url)) return false;
        add_auth();
        return true;
    }

    plain_.setTimeout(HTTP_TIMEOUT_MS / 1000);
    if (!http_.begin(plain_, url)) return false;
    add_auth();
    return true;
}

// Der Header schadet auch dort nicht, wo der Kamera-Token in der URL steht —
// dafuer entfaellt die Gefahr, ihn irgendwo zu vergessen.
void BambuddyHttp::add_auth()
{
    const char *key = bambuddy_api_key();
    if (key && key[0]) http_.addHeader("X-API-Key", key);
}

int BambuddyHttp::get()
{
    return http_.GET();
}

int BambuddyHttp::post(const char *body)
{
    return http_.POST(body ? body : "");
}

int BambuddyHttp::del()
{
    return http_.sendRequest("DELETE", (uint8_t *)nullptr, 0);
}

void BambuddyHttp::end(bool close)
{
    http_.end();
    if (close) {
        secure_.stop();
        plain_.stop();
    }
}

void BambuddyHttp::cancel()
{
    secure_.stop();
    plain_.stop();
}
