#include "bambuddy_http.h"

#include <Arduino.h>
#include <string.h>

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
        return http_.begin(secure_, url);
    }

    plain_.setTimeout(HTTP_TIMEOUT_MS / 1000);
    return http_.begin(plain_, url);
}

void BambuddyHttp::end(bool close)
{
    http_.end();
    if (close) {
        secure_.stop();
        plain_.stop();
    }
}
