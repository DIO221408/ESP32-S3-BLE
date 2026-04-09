#include <sys/time.h>

#include "time_sync.h"

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"

static const char *TAG = "TIME_SYNC";
static app_context_t *s_time_sync_ctx = NULL;

static void time_sync_notification_cb(struct timeval *tv)
{
    (void)tv;

    if (s_time_sync_ctx == NULL) {
        return;
    }

    s_time_sync_ctx->time_synced = true;
    ESP_LOGI(TAG, "SNTP time synchronized. telemetry ts will use epoch_ms.");
}

void time_sync_start(app_context_t *ctx)
{
    if (ctx->sntp_inited) {
        return;
    }

    s_time_sync_ctx = ctx;
    ctx->sntp_inited = true;
    ctx->time_synced = false;

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    config.start = true;
    config.sync_cb = time_sync_notification_cb;

    ESP_ERROR_CHECK(esp_netif_sntp_init(&config));
    ESP_LOGI(TAG, "SNTP initialized (waiting for sync)...");
}
