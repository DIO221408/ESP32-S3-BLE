#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "driver/i2s_std.h"
#include "esp_bt_defs.h"
#include "led_strip.h"
#include "mqtt_client.h"

#include "app_config.h"

typedef struct {
    EventGroupHandle_t wifi_event_group;
    int retry_num;
    int wifi_state_machine_resets;
    uint32_t wifi_consecutive_failures;
    int64_t wifi_last_ip_ok_us;
    bool ble_adv_active;
    bool ble_adv_ready;

    esp_mqtt_client_handle_t mqtt_client;
    char device_id[32];
    char topic_cmd[64];
    char topic_telemetry[64];
    uint32_t telemetry_seq;
    bool mqtt_connected;
    bool time_synced;
    bool sntp_inited;

    i2s_chan_handle_t i2s_rx_chan;
    int32_t inmp411_samples[INMP411_FRAME_SAMPLES];
    float inmp411_noise_ema_db;
    led_strip_handle_t status_led;

    uint8_t adv_config_done;
    esp_bd_addr_t local_addr;
    uint8_t local_addr_type;
} app_context_t;

void app_context_init(app_context_t *ctx);
