#include <string.h>

#include "wifi_manager.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/err.h"
#include "lwip/sys.h"

#include "app_config.h"
#include "ble_beacon.h"
#include "mqtt_service.h"
#include "status_led.h"
#include "time_sync.h"

static const char *TAG = "WIFI_STATION";

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data);
static void ip_event_handler(void *arg,
                             esp_event_base_t event_base,
                             int32_t event_id,
                             void *event_data);
static void wifi_connect_with_backoff(app_context_t *ctx, bool comeback_aware);
static bool wifi_force_state_machine_reset(app_context_t *ctx);
static const char *wifi_disconnect_reason_to_str(uint8_t reason);

bool wifi_manager_init_station(app_context_t *ctx)
{
    ctx->wifi_event_group = xEventGroupCreate();
    if (ctx->wifi_event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create Wi-Fi event group");
        return false;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, ctx));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_event_handler, ctx));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP, &ip_event_handler, ctx));

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA3_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = true,
            },
        },
    };

    strncpy((char *)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, WIFI_PASS, sizeof(wifi_config.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi initialization finished. Connecting to SSID:%s", WIFI_SSID);

    EventBits_t bits = xEventGroupWaitBits(ctx->wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Successfully connected to SSID:%s", WIFI_SSID);
        return true;
    }

    if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s", WIFI_SSID);
        return false;
    }

    ESP_LOGE(TAG, "Unexpected Wi-Fi wait result");
    return false;
}

static void wifi_connect_with_backoff(app_context_t *ctx, bool comeback_aware)
{
    int64_t now_us = esp_timer_get_time();
    uint32_t effective_failures = ctx->wifi_consecutive_failures;
    uint32_t cap_ms = WIFI_BACKOFF_MAX_MS;

    if (comeback_aware &&
        ctx->wifi_last_ip_ok_us > 0 &&
        (now_us - ctx->wifi_last_ip_ok_us) <= (int64_t)WIFI_COMEBACK_WINDOW_MS * 1000) {
        effective_failures = effective_failures > 1 ? (effective_failures - 1) : 1;
        cap_ms = WIFI_COMEBACK_BACKOFF_CAP_MS;
    }

    uint32_t backoff_ms = 0;
    if (effective_failures > 0) {
        backoff_ms = WIFI_BACKOFF_BASE_MS;
        if (effective_failures > 1) {
            uint32_t shift = effective_failures - 1;
            if (shift > 10) {
                shift = 10;
            }
            backoff_ms = WIFI_BACKOFF_BASE_MS << shift;
        }
    }

    if (backoff_ms > cap_ms) {
        backoff_ms = cap_ms;
    }

    if (backoff_ms > 0) {
        ESP_LOGI(TAG,
                 "Reconnect backoff %lu ms (consecutive_failures=%lu, comeback_aware=%s)",
                 (unsigned long)backoff_ms,
                 (unsigned long)ctx->wifi_consecutive_failures,
                 comeback_aware ? "yes" : "no");
        vTaskDelay(pdMS_TO_TICKS(backoff_ms));
    }

    esp_err_t ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(ret));
    }
}

static bool wifi_force_state_machine_reset(app_context_t *ctx)
{
    if (ctx->wifi_state_machine_resets >= WIFI_MAX_STATE_MACHINE_RESETS) {
        ESP_LOGE(TAG, "Skip Wi-Fi reset: reset budget exhausted (%d)", WIFI_MAX_STATE_MACHINE_RESETS);
        return false;
    }

    ctx->wifi_state_machine_resets++;
    ESP_LOGW(TAG,
             "Forcing Wi-Fi state machine reset (%d/%d)",
             ctx->wifi_state_machine_resets,
             WIFI_MAX_STATE_MACHINE_RESETS);

    esp_err_t ret = esp_wifi_stop();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_INIT && ret != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGW(TAG, "esp_wifi_stop failed during reset: %s", esp_err_to_name(ret));
    }

    vTaskDelay(pdMS_TO_TICKS(200));

    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed during reset: %s", esp_err_to_name(ret));
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(120));
    return true;
}

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    app_context_t *ctx = (app_context_t *)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WiFi station started, connecting to AP...");
        wifi_connect_with_backoff(ctx, false);
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disconn = (wifi_event_sta_disconnected_t *)event_data;
        int64_t now_us = esp_timer_get_time();

        ctx->wifi_consecutive_failures++;
        ctx->retry_num++;

        xEventGroupClearBits(ctx->wifi_event_group, WIFI_CONNECTED_BIT);
        ble_beacon_pause(ctx);

        bool comeback_aware = (ctx->wifi_last_ip_ok_us > 0) &&
                              ((now_us - ctx->wifi_last_ip_ok_us) <= (int64_t)WIFI_COMEBACK_WINDOW_MS * 1000);

        ESP_LOGW(TAG,
                 "Disconnected from AP, reason=%u(%s), failures=%lu, retry=%d/%d",
                 (unsigned int)disconn->reason,
                 wifi_disconnect_reason_to_str((uint8_t)disconn->reason),
                 (unsigned long)ctx->wifi_consecutive_failures,
                 ctx->retry_num,
                 WIFI_MAX_RETRY);

        if (ctx->retry_num >= WIFI_MAX_RETRY) {
            xEventGroupSetBits(ctx->wifi_event_group, WIFI_FAIL_BIT);
            status_led_set_rgb(ctx, 64, 0, 0);
            ESP_LOGE(TAG, "Failed to connect to AP after %d attempts", WIFI_MAX_RETRY);
            return;
        }

        bool force_reset = false;
        if (ctx->wifi_consecutive_failures >= WIFI_FORCE_RESET_TRIGGER_FAILS) {
            force_reset = true;
        } else if (ctx->wifi_consecutive_failures >= WIFI_COOLDOWN_TRIGGER_FAILS &&
                   ctx->wifi_last_ip_ok_us > 0 &&
                   (now_us - ctx->wifi_last_ip_ok_us) >= (int64_t)WIFI_COOLDOWN_WINDOW_MS * 1000) {
            force_reset = true;
        }

        if (force_reset) {
            if (wifi_force_state_machine_reset(ctx)) {
                ctx->wifi_consecutive_failures = 0;
                return;
            }
            ESP_LOGW(TAG, "Wi-Fi reset failed, fallback to normal reconnect path");
        }

        wifi_connect_with_backoff(ctx, comeback_aware);
        ESP_LOGI(TAG, "Reconnect attempt requested (attempt %d/%d)", ctx->retry_num, WIFI_MAX_RETRY);
    }
}

static const char *wifi_disconnect_reason_to_str(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_UNSPECIFIED:
        return "UNSPECIFIED";
    case WIFI_REASON_AUTH_EXPIRE:
        return "AUTH_EXPIRE";
    case WIFI_REASON_AUTH_FAIL:
        return "AUTH_FAIL";
    case WIFI_REASON_ASSOC_EXPIRE:
        return "ASSOC_EXPIRE";
    case WIFI_REASON_ASSOC_FAIL:
        return "ASSOC_FAIL";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        return "4WAY_HANDSHAKE_TIMEOUT";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "HANDSHAKE_TIMEOUT";
    case WIFI_REASON_NO_AP_FOUND:
        return "NO_AP_FOUND";
    case WIFI_REASON_BEACON_TIMEOUT:
        return "BEACON_TIMEOUT";
#ifdef WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        return "NO_AP_FOUND_IN_AUTHMODE_THRESHOLD";
#endif
#ifdef WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
        return "NO_AP_FOUND_W_COMPATIBLE_SECURITY";
#endif
    default:
        return "UNKNOWN";
    }
}

static void ip_event_handler(void *arg,
                             esp_event_base_t event_base,
                             int32_t event_id,
                             void *event_data)
{
    app_context_t *ctx = (app_context_t *)arg;

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Connected. Got IP address: " IPSTR, IP2STR(&event->ip_info.ip));

        ctx->retry_num = 0;
        ctx->wifi_consecutive_failures = 0;
        ctx->wifi_state_machine_resets = 0;
        ctx->wifi_last_ip_ok_us = esp_timer_get_time();

        xEventGroupClearBits(ctx->wifi_event_group, WIFI_FAIL_BIT);
        xEventGroupSetBits(ctx->wifi_event_group, WIFI_CONNECTED_BIT);
        status_led_set_rgb(ctx, 0, 64, 0);

        ble_beacon_resume_if_wifi_ready(ctx);
        time_sync_start(ctx);
        mqtt_service_start(ctx);
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_LOST_IP) {
        ESP_LOGW(TAG, "Lost IP address, waiting for reconnect");
        xEventGroupClearBits(ctx->wifi_event_group, WIFI_CONNECTED_BIT);
        ble_beacon_pause(ctx);
    }
}
