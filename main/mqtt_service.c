#include <inttypes.h>
#include <stdio.h>
#include <sys/time.h>

#include "mqtt_service.h"

#include "esp_log.h"
#include "esp_mac.h"

#include "app_config.h"
#include "status_led.h"

static const char *TAG = "MQTT";

static void mqtt_event_handler(void *handler_args,
                               esp_event_base_t base,
                               int32_t event_id,
                               void *event_data)
{
    app_context_t *ctx = (app_context_t *)handler_args;
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    (void)base;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected, subscribing: %s", ctx->topic_cmd);
        ctx->mqtt_connected = true;
        status_led_set_rgb(ctx, 0, 0, 64);
        esp_mqtt_client_subscribe(ctx->mqtt_client, ctx->topic_cmd, 0);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ctx->mqtt_connected = false;
        status_led_set_rgb(ctx, 0, 64, 0);
        ESP_LOGW(TAG, "MQTT disconnected");
        break;
    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT subscribed, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT data on topic: %.*s", event->topic_len, event->topic);
        ESP_LOGI(TAG, "MQTT payload: %.*s", event->data_len, event->data);
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT event error");
        break;
    default:
        break;
    }
}

void mqtt_service_start(app_context_t *ctx)
{
    if (ctx->mqtt_client != NULL) {
        return;
    }

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    snprintf(ctx->device_id,
             sizeof(ctx->device_id),
             "%02X%02X%02X%02X%02X%02X",
             mac[0],
             mac[1],
             mac[2],
             mac[3],
             mac[4],
             mac[5]);

    snprintf(ctx->topic_cmd, sizeof(ctx->topic_cmd), "device/%s/cmd", ctx->device_id);
    snprintf(ctx->topic_telemetry, sizeof(ctx->topic_telemetry), "device/%s/telemetry", ctx->device_id);

    ESP_LOGI(TAG, "MQTT client id: %s", ctx->device_id);
    ESP_LOGI(TAG, "MQTT cmd topic: %s", ctx->topic_cmd);
    ESP_LOGI(TAG, "MQTT telemetry topic: %s", ctx->topic_telemetry);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
        .credentials.client_id = ctx->device_id,
        .credentials.username = MQTT_USERNAME,
        .credentials.authentication.password = MQTT_PASSWORD,
        .session.keepalive = MQTT_KEEPALIVE_SEC,
    };

    ctx->mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(ctx->mqtt_client,
                                                   ESP_EVENT_ANY_ID,
                                                   mqtt_event_handler,
                                                   ctx));
    ESP_ERROR_CHECK(esp_mqtt_client_start(ctx->mqtt_client));

    ESP_LOGI(TAG, "MQTT start requested");
}

esp_err_t mqtt_service_publish_telemetry(app_context_t *ctx,
                                         float temperature,
                                         float humidity,
                                         float noise_db,
                                         int rssi)
{
    if (ctx->mqtt_client == NULL || !ctx->mqtt_connected || !ctx->time_synced) {
        return ESP_ERR_INVALID_STATE;
    }

    struct timeval tv = {0};
    gettimeofday(&tv, NULL);

    int64_t ts_ms = (int64_t)tv.tv_sec * 1000LL + (int64_t)tv.tv_usec / 1000LL;
    ctx->telemetry_seq++;

    char payload[256] = {0};
    int written = snprintf(payload,
                           sizeof(payload),
                           "{\"type\":\"telemetry\",\"deviceId\":\"%s\",\"ts\":%" PRId64 ",\"seq\":%" PRIu32 ",\"data\":{\"temperature\":%.1f,\"humidity\":%.1f,\"noise\":%.1f,\"rssi\":%d}}",
                           ctx->device_id,
                           ts_ms,
                           ctx->telemetry_seq,
                           temperature,
                           humidity,
                           noise_db,
                           rssi);

    if (written <= 0 || written >= (int)sizeof(payload)) {
        ESP_LOGE(TAG, "Telemetry payload build failed");
        return ESP_ERR_INVALID_SIZE;
    }

    int msg_id = esp_mqtt_client_publish(ctx->mqtt_client, ctx->topic_telemetry, payload, 0, 0, 0);
    ESP_LOGI(TAG, "Telemetry published, msg_id=%d, payload=%s", msg_id, payload);
    return ESP_OK;
}
