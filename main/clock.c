#include <stdlib.h>
#include <time.h>
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "settings.h"
#include "clock.h"

static const char *TAG = "clock";
static char s_server[64];      /* SNTP keeps the pointer */

bool clock_valid(void) {
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    return tm.tm_year + 1900 >= 2025;
}

static void apply_tz(void) {
    settings_t cfg;
    settings_copy(&cfg);
    setenv("TZ", cfg.tz[0] ? cfg.tz : "UTC0", 1);
    tzset();
    ESP_LOGI(TAG, "timezone %s", cfg.tz);
}

void clock_init(void) {
    settings_t cfg;
    settings_copy(&cfg);
    apply_tz();
    settings_subscribe(APPLY_SYSTEM, apply_tz);

    strlcpy(s_server, cfg.ntp[0] ? cfg.ntp : "pool.ntp.org", sizeof(s_server));
    esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG(s_server);
    sc.start = true;
    if (esp_netif_sntp_init(&sc) != ESP_OK) ESP_LOGE(TAG, "SNTP init failed");
    else ESP_LOGI(TAG, "SNTP via %s", s_server);
}
