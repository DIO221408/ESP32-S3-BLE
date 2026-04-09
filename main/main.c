/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <math.h>
#include <sys/time.h>
#include "nvs_flash.h"

// BLE stack headers.
#include "esp_bt.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_gatt_defs.h"
#include "esp_bt_main.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_bt_defs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

// Wi-Fi stack headers.
// Reference: https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32s3/api-reference/network/esp_wifi.html
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "esp_sntp.h"

// Coexistence control headers.
// Reference: https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32s3/api-reference/system/esp_coexist.html
#include "esp_coexist.h"

// MQTT client headers.
#include "mqtt_client.h"
#include "esp_mac.h"
#include "led_strip.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"

/*
 * Module responsibilities:
 * 1) Advertise BLE beacon frames for nearby discovery.
 * 2) Connect to Wi-Fi in station mode for IP backhaul.
 * 3) Establish MQTT channel for command/telemetry exchange.
 *
 * Startup order is intentional:
 * - Wi-Fi/netif/event loop is initialized first.
 * - BLE stack starts after Wi-Fi is stable.
 * - MQTT starts only after a valid IP is acquired.
 */
#define ADV_CONFIG_FLAG      (1 << 0)
#define SCAN_RSP_CONFIG_FLAG (1 << 1)
#define URI_PREFIX_HTTPS     (0x17)

// Wi-Fi event bits for connection state synchronization.
#define WIFI_CONNECTED_BIT   BIT0
#define WIFI_FAIL_BIT        BIT1
#define WIFI_MAX_RETRY       20

// Wi-Fi reconnect and recovery tuning.
#define WIFI_BACKOFF_BASE_MS             1000
#define WIFI_BACKOFF_MAX_MS             30000
#define WIFI_COMEBACK_WINDOW_MS        120000
#define WIFI_COMEBACK_BACKOFF_CAP_MS     8000
#define WIFI_COOLDOWN_TRIGGER_FAILS         6
#define WIFI_COOLDOWN_WINDOW_MS         45000
#define WIFI_FORCE_RESET_TRIGGER_FAILS       8
#define WIFI_MAX_STATE_MACHINE_RESETS        3

// Wi-Fi credentials for station mode.
#define WIFI_SSID            "fangdehuanxue"
#define WIFI_PASS            "xa125878"

// MQTT broker connection settings.
#define MQTT_BROKER_URI      "mqtt://192.168.1.198:1883"
#define MQTT_USERNAME        "admin"
#define MQTT_PASSWORD        "public"
#define MQTT_KEEPALIVE_SEC   60

// RGB status LED (WS2812) settings.
#define STATUS_LED_GPIO      48
#define STATUS_LED_PIXEL_NUM 1

// DHT11 settings.
#define DHT11_GPIO           GPIO_NUM_8
#define DHT11_SAMPLE_PERIOD_MS 7000

// INMP411 (I2S) pin mapping.
#define INMP411_I2S_BCLK_GPIO GPIO_NUM_12
#define INMP411_I2S_WS_GPIO   GPIO_NUM_13
#define INMP411_I2S_SD_GPIO   GPIO_NUM_14
#define INMP411_SAMPLE_RATE_HZ 16000
#define INMP411_FRAME_SAMPLES 1024
#define INMP411_NOISE_PERIOD_MS 5000
#define INMP411_TASK_STACK_SIZE 12288
#define INMP411_NOISE_DB_OFFSET 94.0f
#define INMP411_NOISE_EMA_ALPHA 0.20f

// Telemetry payload defaults (JSON fields required by backend).
#define TELEMETRY_DEFAULT_RSSI    -53

static void esp_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
static void ip_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);
static const char *wifi_disconnect_reason_to_str(uint8_t reason);
static void wifi_connect_with_backoff(bool comeback_aware);
static void wifi_pause_ble_advertising(void);
static void wifi_resume_ble_advertising(void);
static bool wifi_force_state_machine_reset(void);
static void mqtt_start(void);
static void status_led_init(void);
static void status_led_set_rgb(uint8_t red, uint8_t green, uint8_t blue);
static void sntp_time_sync_notification_cb(struct timeval *tv);
static void tour_sntp_init(void);
static esp_err_t dht11_read(int *temperature_c, int *humidity_pct);
static void dht11_task(void *arg);
static esp_err_t inmp411_i2s_init(void);
static float inmp411_measure_noise_db(void);
static void inmp411_noise_task(void *arg);
static void ble_status_task(void *arg);

static const char *DEMO_TAG = "BLE_BEACON";
static const char *WIFI_TAG = "WIFI_STATION";
static const char device_name[] = "Bluedroid_Beacon";

// Runtime state shared by asynchronous callbacks.
static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;
static int s_wifi_state_machine_resets = 0;
static uint32_t s_wifi_consecutive_failures = 0;
static int64_t s_wifi_last_ip_ok_us = 0;
static bool s_ble_adv_active = false;
static bool s_ble_adv_ready = false;

// MQTT runtime objects and per-device topics.
static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static char s_device_id[32] = {0};
static char s_topic_cmd[64] = {0};
static char s_topic_telemetry[64] = {0};
static uint32_t s_telemetry_seq = 0;
static bool s_mqtt_connected = false;
static bool s_time_synced = false;
static bool s_sntp_inited = false;
static i2s_chan_handle_t s_i2s_rx_chan = NULL;
static int32_t s_inmp411_samples[INMP411_FRAME_SAMPLES] = {0};
static float s_inmp411_noise_ema_db = -1.0f;
static led_strip_handle_t s_status_led = NULL;

static void status_led_set_rgb(uint8_t red, uint8_t green, uint8_t blue)
{
    if (s_status_led == NULL) {
        return;
    }

    led_strip_set_pixel(s_status_led, 0, red, green, blue);
    led_strip_refresh(s_status_led);
}

static void status_led_init(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = STATUS_LED_GPIO,
        .max_leds = STATUS_LED_PIXEL_NUM,
    };

    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000,
    };

    esp_err_t ret = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_status_led);
    if (ret != ESP_OK) {
        ESP_LOGW(DEMO_TAG, "Status LED init failed on GPIO %d: %s", STATUS_LED_GPIO, esp_err_to_name(ret));
        return;
    }

    status_led_set_rgb(0, 0, 0);
    ESP_LOGI(DEMO_TAG, "Status LED initialized on GPIO %d", STATUS_LED_GPIO);
}

static uint8_t adv_config_done = 0;
static esp_bd_addr_t local_addr;
static uint8_t local_addr_type;

static esp_ble_adv_params_t adv_params = {
    .adv_int_min = 0x12C0,  // 3000ms (3s, reduce radio duty cycle)
    .adv_int_max = 0x12C0,  // 3000ms
    .adv_type = ADV_TYPE_SCAN_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

// Advertising payload (ADV packet body).
static uint8_t adv_raw_data[] = {
    0x02, ESP_BLE_AD_TYPE_FLAG, 0x06,
    0x11, ESP_BLE_AD_TYPE_NAME_CMPL, 'B', 'l', 'u', 'e', 'd', 'r', 'o', 'i', 'd', '_', 'B', 'e', 'a', 'c', 'o', 'n',
    0x02, ESP_BLE_AD_TYPE_TX_PWR, 0x09,
    0x03, ESP_BLE_AD_TYPE_APPEARANCE, 0x00,0x02,
    0x02, ESP_BLE_AD_TYPE_LE_ROLE, 0x00,
};

// Scan response payload. Device MAC bytes [2..7] are patched at runtime.
static uint8_t scan_rsp_raw_data[] = {
    0x08, ESP_BLE_AD_TYPE_LE_DEV_ADDR, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x11, ESP_BLE_AD_TYPE_URI, URI_PREFIX_HTTPS, '/', '/', 'e', 's', 'p', 'r', 'e', 's', 's', 'i', 'f', '.', 'c', 'o', 'm',
};

// ========== Wi-Fi event handlers ==========
// Reference: https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32s3/api-reference/system/esp_event.html

static void wifi_pause_ble_advertising(void)
{
    if (!s_ble_adv_active) {
        return;
    }

    esp_err_t ret = esp_ble_gap_stop_advertising();
    if (ret != ESP_OK) {
        ESP_LOGW(WIFI_TAG, "Failed to pause BLE advertising during Wi-Fi outage: %s", esp_err_to_name(ret));
        return;
    }

    s_ble_adv_active = false;
    ESP_LOGI(WIFI_TAG, "BLE advertising paused during Wi-Fi outage");
}

static void wifi_resume_ble_advertising(void)
{
    if (!s_ble_adv_ready || s_ble_adv_active || s_wifi_event_group == NULL) {
        return;
    }

    EventBits_t bits = xEventGroupGetBits(s_wifi_event_group);
    if ((bits & WIFI_CONNECTED_BIT) == 0) {
        return;
    }

    esp_err_t ret = esp_ble_gap_start_advertising(&adv_params);
    if (ret != ESP_OK) {
        ESP_LOGW(WIFI_TAG, "Failed to resume BLE advertising after Wi-Fi recovery: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(WIFI_TAG, "BLE advertising resume requested after Wi-Fi recovery");
}

static void wifi_connect_with_backoff(bool comeback_aware)
{
    int64_t now_us = esp_timer_get_time();
    uint32_t effective_failures = s_wifi_consecutive_failures;
    uint32_t cap_ms = WIFI_BACKOFF_MAX_MS;

    if (comeback_aware &&
        s_wifi_last_ip_ok_us > 0 &&
        (now_us - s_wifi_last_ip_ok_us) <= (int64_t)WIFI_COMEBACK_WINDOW_MS * 1000) {
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
        ESP_LOGI(WIFI_TAG,
                 "Reconnect backoff %lu ms (consecutive_failures=%lu, comeback_aware=%s)",
                 (unsigned long)backoff_ms,
                 (unsigned long)s_wifi_consecutive_failures,
                 comeback_aware ? "yes" : "no");
        vTaskDelay(pdMS_TO_TICKS(backoff_ms));
    }

    esp_err_t ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGW(WIFI_TAG, "esp_wifi_connect failed: %s", esp_err_to_name(ret));
    }
}

static bool wifi_force_state_machine_reset(void)
{
    if (s_wifi_state_machine_resets >= WIFI_MAX_STATE_MACHINE_RESETS) {
        ESP_LOGE(WIFI_TAG, "Skip Wi-Fi reset: reset budget exhausted (%d)", WIFI_MAX_STATE_MACHINE_RESETS);
        return false;
    }

    s_wifi_state_machine_resets++;
    ESP_LOGW(WIFI_TAG,
             "Forcing Wi-Fi state machine reset (%d/%d)",
             s_wifi_state_machine_resets,
             WIFI_MAX_STATE_MACHINE_RESETS);

    esp_err_t ret = esp_wifi_stop();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_INIT && ret != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGW(WIFI_TAG, "esp_wifi_stop failed during reset: %s", esp_err_to_name(ret));
    }

    vTaskDelay(pdMS_TO_TICKS(200));

    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(WIFI_TAG, "esp_wifi_start failed during reset: %s", esp_err_to_name(ret));
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(120));
    return true;
}

/**
 * Handle Wi-Fi driver events.
 * - Start the first connection attempt when STA starts.
 * - Retry on disconnect with an upper bound.
 * - Signal failure through `WIFI_FAIL_BIT` once retries are exhausted.
 */
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(WIFI_TAG, "WiFi station started, connecting to AP...");
        wifi_connect_with_backoff(false);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disconn = (wifi_event_sta_disconnected_t *)event_data;
        int64_t now_us = esp_timer_get_time();

        s_wifi_consecutive_failures++;
        s_retry_num++;

        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        wifi_pause_ble_advertising();

        bool comeback_aware = (s_wifi_last_ip_ok_us > 0) &&
                              ((now_us - s_wifi_last_ip_ok_us) <= (int64_t)WIFI_COMEBACK_WINDOW_MS * 1000);

        ESP_LOGW(WIFI_TAG,
                 "Disconnected from AP, reason=%u(%s), failures=%lu, retry=%d/%d",
                 (unsigned int)disconn->reason,
                 wifi_disconnect_reason_to_str((uint8_t)disconn->reason),
                 (unsigned long)s_wifi_consecutive_failures,
                 s_retry_num,
                 WIFI_MAX_RETRY);

        if (s_retry_num >= WIFI_MAX_RETRY) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            status_led_set_rgb(64, 0, 0);
            ESP_LOGE(WIFI_TAG, "Failed to connect to AP after %d attempts", WIFI_MAX_RETRY);
            return;
        }

        bool force_reset = false;
        if (s_wifi_consecutive_failures >= WIFI_FORCE_RESET_TRIGGER_FAILS) {
            force_reset = true;
        } else if (s_wifi_consecutive_failures >= WIFI_COOLDOWN_TRIGGER_FAILS &&
                   s_wifi_last_ip_ok_us > 0 &&
                   (now_us - s_wifi_last_ip_ok_us) >= (int64_t)WIFI_COOLDOWN_WINDOW_MS * 1000) {
            force_reset = true;
        }

        if (force_reset) {
            if (wifi_force_state_machine_reset()) {
                s_wifi_consecutive_failures = 0;
                return;
            }
            ESP_LOGW(WIFI_TAG, "Wi-Fi reset failed, fallback to normal reconnect path");
        }

        wifi_connect_with_backoff(comeback_aware);
        ESP_LOGI(WIFI_TAG, "Reconnect attempt requested (attempt %d/%d)", s_retry_num, WIFI_MAX_RETRY);
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

/**
 * Handle IP events.
 * On `IP_EVENT_STA_GOT_IP`, mark Wi-Fi as connected and start MQTT.
 */
static void ip_event_handler(void* arg, esp_event_base_t event_base,
                             int32_t event_id, void* event_data)
{
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(WIFI_TAG, "✓ Connected! Got IP address: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        s_wifi_consecutive_failures = 0;
        s_wifi_state_machine_resets = 0;
        s_wifi_last_ip_ok_us = esp_timer_get_time();

        xEventGroupClearBits(s_wifi_event_group, WIFI_FAIL_BIT);
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        status_led_set_rgb(0, 64, 0);

        wifi_resume_ble_advertising();
        tour_sntp_init();
        mqtt_start();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_LOST_IP) {
        ESP_LOGW(WIFI_TAG, "Lost IP address, waiting for reconnect");
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        wifi_pause_ble_advertising();
    }
}

static void sntp_time_sync_notification_cb(struct timeval *tv)
{
    (void)tv;
    s_time_synced = true;
    ESP_LOGI(DEMO_TAG, "SNTP time synchronized. telemetry ts will use epoch_ms.");
}

static void tour_sntp_init(void)
{
    if (s_sntp_inited) {
        return;
    }

    s_sntp_inited = true;
    s_time_synced = false;

    /* Use a public NTP server pool.
     * If your network environment blocks public NTP, replace the server string
     * with a reachable internal NTP address.
     */
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    config.start = true;
    config.sync_cb = sntp_time_sync_notification_cb;

    ESP_ERROR_CHECK(esp_netif_sntp_init(&config));
    ESP_LOGI(DEMO_TAG, "SNTP initialized (waiting for sync)...");
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(DEMO_TAG, "MQTT connected, subscribing: %s", s_topic_cmd);
        s_mqtt_connected = true;
        status_led_set_rgb(0, 0, 64);
        esp_mqtt_client_subscribe(s_mqtt_client, s_topic_cmd, 0);
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_mqtt_connected = false;
        status_led_set_rgb(0, 64, 0);
        ESP_LOGW(DEMO_TAG, "MQTT disconnected");
        break;
    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(DEMO_TAG, "MQTT subscribed, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(DEMO_TAG, "MQTT data on topic: %.*s", event->topic_len, event->topic);
        ESP_LOGI(DEMO_TAG, "MQTT payload: %.*s", event->data_len, event->data);
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGE(DEMO_TAG, "MQTT event error");
        break;
    default:
        break;
    }
}

static void mqtt_start(void)
{
    if (s_mqtt_client != NULL) {
        return;
    }

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_device_id, sizeof(s_device_id), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    snprintf(s_topic_cmd, sizeof(s_topic_cmd), "device/%s/cmd", s_device_id);
    snprintf(s_topic_telemetry, sizeof(s_topic_telemetry), "device/%s/telemetry", s_device_id);

    ESP_LOGI(DEMO_TAG, "MQTT client id: %s", s_device_id);
    ESP_LOGI(DEMO_TAG, "MQTT cmd topic: %s", s_topic_cmd);
    ESP_LOGI(DEMO_TAG, "MQTT telemetry topic: %s", s_topic_telemetry);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
        .credentials.client_id = s_device_id,
        .credentials.username = MQTT_USERNAME,
        .credentials.authentication.password = MQTT_PASSWORD,
        .session.keepalive = MQTT_KEEPALIVE_SEC,
    };

    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(s_mqtt_client));

    ESP_LOGI(DEMO_TAG, "MQTT start requested");
}

static esp_err_t dht11_read(int *temperature_c, int *humidity_pct)
{
    uint8_t data[5] = {0};

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << DHT11_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    gpio_set_level(DHT11_GPIO, 0);
    esp_rom_delay_us(20000);
    gpio_set_level(DHT11_GPIO, 1);
    esp_rom_delay_us(30);

    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    int64_t start = esp_timer_get_time();
    while (gpio_get_level(DHT11_GPIO) == 1) {
        if (esp_timer_get_time() - start > 100) {
            return ESP_ERR_TIMEOUT;
        }
    }

    start = esp_timer_get_time();
    while (gpio_get_level(DHT11_GPIO) == 0) {
        if (esp_timer_get_time() - start > 100) {
            return ESP_ERR_TIMEOUT;
        }
    }

    start = esp_timer_get_time();
    while (gpio_get_level(DHT11_GPIO) == 1) {
        if (esp_timer_get_time() - start > 100) {
            return ESP_ERR_TIMEOUT;
        }
    }

    for (int i = 0; i < 40; i++) {
        start = esp_timer_get_time();
        while (gpio_get_level(DHT11_GPIO) == 0) {
            if (esp_timer_get_time() - start > 80) {
                return ESP_ERR_TIMEOUT;
            }
        }

        start = esp_timer_get_time();
        while (gpio_get_level(DHT11_GPIO) == 1) {
            if (esp_timer_get_time() - start > 120) {
                return ESP_ERR_TIMEOUT;
            }
        }

        int high_us = (int)(esp_timer_get_time() - start);
        data[i / 8] <<= 1;
        if (high_us > 40) {
            data[i / 8] |= 1;
        }
    }

    uint8_t checksum = (uint8_t)(data[0] + data[1] + data[2] + data[3]);
    if (checksum != data[4]) {
        return ESP_ERR_INVALID_CRC;
    }

    *humidity_pct = data[0];
    *temperature_c = data[2];

    return ESP_OK;
}

static void dht11_task(void *arg)
{
    ESP_LOGI(DEMO_TAG, "DHT11 task started on GPIO %d, period %d ms", DHT11_GPIO, DHT11_SAMPLE_PERIOD_MS);

    while (true) {
        int temperature_c = 0;
        int humidity_pct = 0;
        esp_err_t ret = dht11_read(&temperature_c, &humidity_pct);

        if (ret == ESP_OK) {
            ESP_LOGI(DEMO_TAG, "DHT11 -> Temperature: %d C, Humidity: %d %%", temperature_c, humidity_pct);

            if (s_mqtt_client != NULL && s_mqtt_connected && s_time_synced) {
                struct timeval tv = {0};
                gettimeofday(&tv, NULL);
                int64_t ts_ms = (int64_t)tv.tv_sec * 1000LL + (int64_t)tv.tv_usec / 1000LL;
                s_telemetry_seq++;

                float temperature = (float)temperature_c;
                float humidity = (float)humidity_pct;
                float noise_db = s_inmp411_noise_ema_db;

                char payload[256] = {0};
                int written = snprintf(
                    payload,
                    sizeof(payload),
                    "{\"type\":\"telemetry\",\"deviceId\":\"%s\",\"ts\":%" PRId64 ",\"seq\":%" PRIu32 ",\"data\":{\"temperature\":%.1f,\"humidity\":%.1f,\"noise\":%.1f,\"rssi\":%d}}",
                    s_device_id,
                    ts_ms,
                    s_telemetry_seq,
                    temperature,
                    humidity,
                    noise_db,
                    TELEMETRY_DEFAULT_RSSI
                );

                if (written > 0 && written < (int)sizeof(payload)) {
                    int msg_id = esp_mqtt_client_publish(s_mqtt_client, s_topic_telemetry, payload, 0, 0, 0);
                    ESP_LOGI(DEMO_TAG, "Telemetry published, msg_id=%d, payload=%s", msg_id, payload);
                } else {
                    ESP_LOGE(DEMO_TAG, "Telemetry payload build failed");
                }
            }
        } else if (ret == ESP_ERR_INVALID_CRC) {
            ESP_LOGW(DEMO_TAG, "DHT11 checksum mismatch");
        } else {
            ESP_LOGW(DEMO_TAG, "DHT11 read timeout");
        }

        vTaskDelay(pdMS_TO_TICKS(DHT11_SAMPLE_PERIOD_MS));
    }
}

static esp_err_t inmp411_i2s_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &s_i2s_rx_chan));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(INMP411_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = INMP411_I2S_BCLK_GPIO,
            .ws = INMP411_I2S_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din = INMP411_I2S_SD_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_i2s_rx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_i2s_rx_chan));

    ESP_LOGI(DEMO_TAG, "INMP411 I2S initialized (BCLK=%d, WS=%d, SD=%d)",
             INMP411_I2S_BCLK_GPIO, INMP411_I2S_WS_GPIO, INMP411_I2S_SD_GPIO);

    return ESP_OK;
}

static float inmp411_measure_noise_db(void)
{
    size_t bytes_read = 0;

    esp_err_t ret = i2s_channel_read(s_i2s_rx_chan, s_inmp411_samples, sizeof(s_inmp411_samples), &bytes_read, pdMS_TO_TICKS(200));
    if (ret != ESP_OK || bytes_read == 0) {
        return -1.0f;
    }

    int sample_count = (int)(bytes_read / sizeof(int32_t));
    if (sample_count <= 0) {
        return -1.0f;
    }

    double mean = 0.0;
    for (int i = 0; i < sample_count; i++) {
        int32_t s24 = s_inmp411_samples[i] >> 8;
        mean += (double)s24;
    }
    mean /= (double)sample_count;

    double sum_sq = 0.0;
    for (int i = 0; i < sample_count; i++) {
        int32_t s24 = s_inmp411_samples[i] >> 8;
        double centered = (double)s24 - mean;
        double norm = centered / 8388608.0;
        sum_sq += norm * norm;
    }

    double rms = sqrt(sum_sq / (double)sample_count);
    if (rms < 1e-9) {
        rms = 1e-9;
    }

    float raw_dbfs = 20.0f * log10f((float)rms);
    float noise_db_est = raw_dbfs + INMP411_NOISE_DB_OFFSET;

    if (s_inmp411_noise_ema_db < 0.0f) {
        s_inmp411_noise_ema_db = noise_db_est;
    } else {
        s_inmp411_noise_ema_db = (INMP411_NOISE_EMA_ALPHA * noise_db_est) + ((1.0f - INMP411_NOISE_EMA_ALPHA) * s_inmp411_noise_ema_db);
    }

    ESP_LOGI(DEMO_TAG, "INMP411 raw_dbfs=%.1f dBFS, noise_db_est=%.1f dB (offset=%.1f)",
             raw_dbfs, s_inmp411_noise_ema_db, INMP411_NOISE_DB_OFFSET);

    return s_inmp411_noise_ema_db;
}

static void inmp411_noise_task(void *arg)
{
    ESP_LOGI(DEMO_TAG, "INMP411 noise task started, sample_rate=%dHz, period=%dms",
             INMP411_SAMPLE_RATE_HZ, INMP411_NOISE_PERIOD_MS);

    while (true) {
        float noise_db = inmp411_measure_noise_db();
        if (noise_db >= 0.0f) {
            ESP_LOGI(DEMO_TAG, "INMP411 -> Noise level: %.1f dB", noise_db);
        } else {
            ESP_LOGW(DEMO_TAG, "INMP411 read failed");
        }

        UBaseType_t stack_words = uxTaskGetStackHighWaterMark(NULL);
        ESP_LOGI(DEMO_TAG, "INMP411 task stack high watermark: %lu words", (unsigned long)stack_words);

        vTaskDelay(pdMS_TO_TICKS(INMP411_NOISE_PERIOD_MS));
    }
}

static void ble_status_task(void *arg)
{
    while (true) {
        EventBits_t bits = 0;
        if (s_wifi_event_group != NULL) {
            bits = xEventGroupGetBits(s_wifi_event_group);
        }

        ESP_LOGI(
            DEMO_TAG,
            "BLE_STATUS ready=%d active=%d wifi_connected=%d wifi_fail=%d adv_int=%.1fms addr=%02X:%02X:%02X:%02X:%02X:%02X type=%u",
            s_ble_adv_ready ? 1 : 0,
            s_ble_adv_active ? 1 : 0,
            (bits & WIFI_CONNECTED_BIT) ? 1 : 0,
            (bits & WIFI_FAIL_BIT) ? 1 : 0,
            (double)adv_params.adv_int_min * 0.625,
            local_addr[0],
            local_addr[1],
            local_addr[2],
            local_addr[3],
            local_addr[4],
            local_addr[5],
            (unsigned int)local_addr_type
        );

        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

// ========== Wi-Fi initialization ==========
// Reference: https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32s3/api-guides/wifi.html

/**
 * Initialize Wi-Fi station mode and wait for a terminal link result.
 * This function sets up netif/event loop/Wi-Fi driver, then blocks until one
 * of two bits is set by callbacks: `WIFI_CONNECTED_BIT` or `WIFI_FAIL_BIT`.
 */
static bool wifi_init_sta(void)
{
    // 创建WiFi事件组
    s_wifi_event_group = xEventGroupCreate();

    // 初始化TCP/IP协议栈
    ESP_ERROR_CHECK(esp_netif_init());

    // 创建默认事件循环
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 创建默认WiFi Station网络接口
    esp_netif_create_default_wifi_sta();

    // 使用默认配置初始化WiFi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // 注册WiFi事件处理器
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP, &ip_event_handler, NULL));

    // 配置WiFi连接参数
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA3_PSK,  // 仅连接WPA3-Personal
            .pmf_cfg = {
                .capable = true,
                .required = true
            },
        },
    };

    // 设置WiFi模式为Station
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    
    // 关闭WiFi省电模式，提高连接稳定性
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    
    // 启动WiFi
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(WIFI_TAG, "WiFi initialization finished. Connecting to SSID:%s", WIFI_SSID);

    // 等待连接结果
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    // 检查连接结果
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(WIFI_TAG, "✓ Successfully connected to SSID:%s", WIFI_SSID);
        return true;
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(WIFI_TAG, "✗ Failed to connect to SSID:%s", WIFI_SSID);
        return false;
    } else {
        ESP_LOGE(WIFI_TAG, "UNEXPECTED EVENT");
        return false;
    }
}

// ========== BLE/Wi-Fi coexistence tuning ==========
// Reference: https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32s3/api-guides/coexist.html

/**
 * Tune BLE/Wi-Fi coexistence parameters for stable dual-radio operation.
 * Strategy: prioritize BLE airtime and cap Wi-Fi TX power to reduce RF contention.
 */
static void configure_coexistence(void)
{
    ESP_LOGI(DEMO_TAG, "Configuring BLE and WiFi coexistence...");
    
    // 设置共存优先级为WiFi优先（优先保障WiFi连接稳定）
    // ESP_COEX_PREFER_BT: 蓝牙优先（包括BLE）
    // ESP_COEX_PREFER_WIFI: WiFi优先
    // ESP_COEX_PREFER_BALANCE: 平衡模式（默认）
    esp_coex_preference_set(ESP_COEX_PREFER_WIFI);
    ESP_LOGI(DEMO_TAG, "✓ Coexistence preference set to WiFi priority mode");
    
    // 设置BLE发射功率（适度降低功率，减少干扰）
    // 功率等级: ESP_PWR_LVL_N12(-12dBm) 到 ESP_PWR_LVL_P9(+9dBm)
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_N0);  // 0dBm
    ESP_LOGI(DEMO_TAG, "✓ BLE TX power set to 0dBm");
    
    // 设置WiFi发射功率（降低功率，减少干扰）
    // 范围: 8-84 (单位0.25dBm), 例如: 52 = 13dBm
    esp_wifi_set_max_tx_power(52);  // 13dBm
    ESP_LOGI(DEMO_TAG, "✓ WiFi TX power set to 13dBm");
    
    ESP_LOGI(DEMO_TAG, "✓ Coexistence configuration completed");
}

void app_main(void)
{
    esp_err_t ret;

    ESP_LOGI(DEMO_TAG, "========================================");
    ESP_LOGI(DEMO_TAG, "  景区智能导览系统 - BLE信标设备");
    ESP_LOGI(DEMO_TAG, "  BLE + WiFi 共存模式");
    ESP_LOGI(DEMO_TAG, "========================================");

    status_led_init();

    // ========== 步骤1: 初始化NVS ==========
    ESP_LOGI(DEMO_TAG, "[1/6] Initializing NVS...");
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(DEMO_TAG, "✓ NVS initialized");

    // ========== 步骤2: 初始化WiFi ==========
    // 注意：WiFi必须在蓝牙之前初始化（网络接口和事件循环）
    ESP_LOGI(DEMO_TAG, "[2/6] Initializing WiFi...");
    bool wifi_connected = wifi_init_sta();
    if (wifi_connected) {
        ESP_LOGI(DEMO_TAG, "✓ WiFi initialized and connected");
    } else {
        ESP_LOGW(DEMO_TAG, "⚠ WiFi initialization finished but AP connect failed (BLE paused until IP)");
    }

    // ========== 步骤3: 初始化蓝牙控制器 ==========
    ESP_LOGI(DEMO_TAG, "[3/6] Initializing Bluetooth controller...");
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret) {
        ESP_LOGE(DEMO_TAG, "%s initialize controller failed: %s", __func__, esp_err_to_name(ret));
        return;
    }

    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret) {
        ESP_LOGE(DEMO_TAG, "%s enable controller failed: %s", __func__, esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(DEMO_TAG, "✓ Bluetooth controller initialized");

    // ========== 步骤4: 初始化Bluedroid协议栈 ==========
    ESP_LOGI(DEMO_TAG, "[4/6] Initializing Bluedroid stack...");
    ret = esp_bluedroid_init();
    if (ret) {
        ESP_LOGE(DEMO_TAG, "%s init bluetooth failed: %s", __func__, esp_err_to_name(ret));
        return;
    }

    ret = esp_bluedroid_enable();
    if (ret) {
        ESP_LOGE(DEMO_TAG, "%s enable bluetooth failed: %s", __func__, esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(DEMO_TAG, "✓ Bluedroid stack initialized");

    // ========== 步骤5: 配置BLE和WiFi共存 ==========
    ESP_LOGI(DEMO_TAG, "[5/6] Configuring BLE-WiFi coexistence...");
    configure_coexistence();

    // ========== 步骤6: 配置并启动BLE广播 ==========
    ESP_LOGI(DEMO_TAG, "[6/6] Configuring BLE advertising...");
    ret = esp_ble_gap_register_callback(esp_gap_cb);
    if (ret) {
        ESP_LOGE(DEMO_TAG, "gap register error, error code = %x", ret);
        return;
    }

    ret = esp_ble_gap_set_device_name(device_name);
    if (ret) {
        ESP_LOGE(DEMO_TAG, "set device name error, error code = %x", ret);
        return;
    }

    //config adv data
    adv_config_done |= ADV_CONFIG_FLAG;
    adv_config_done |= SCAN_RSP_CONFIG_FLAG;
    ret = esp_ble_gap_config_adv_data_raw(adv_raw_data, sizeof(adv_raw_data));
    if (ret) {
        ESP_LOGE(DEMO_TAG, "config adv data failed, error code = %x", ret);
        return;
    }

    ret = esp_ble_gap_get_local_used_addr(local_addr, &local_addr_type);
    if (ret) {
        ESP_LOGE(DEMO_TAG, "get local used address failed, error code = %x", ret);
        return;
    }
    ESP_LOGI(DEMO_TAG,
             "BLE local addr: %02X:%02X:%02X:%02X:%02X:%02X (type=%u)",
             local_addr[0], local_addr[1], local_addr[2],
             local_addr[3], local_addr[4], local_addr[5],
             (unsigned int)local_addr_type);

    // Prepare dynamic scan response payload by injecting local MAC in little-endian order.
    scan_rsp_raw_data[2] = local_addr[5];
    scan_rsp_raw_data[3] = local_addr[4];
    scan_rsp_raw_data[4] = local_addr[3];
    scan_rsp_raw_data[5] = local_addr[2];
    scan_rsp_raw_data[6] = local_addr[1];
    scan_rsp_raw_data[7] = local_addr[0];
    ret = esp_ble_gap_config_scan_rsp_data_raw(scan_rsp_raw_data, sizeof(scan_rsp_raw_data));
    if (ret) {
        ESP_LOGE(DEMO_TAG, "config scan rsp data failed, error code = %x", ret);
    }

    ESP_LOGI(DEMO_TAG, "========================================");
    ESP_LOGI(DEMO_TAG, "✓ System initialization completed!");
    if (wifi_connected) {
        ESP_LOGI(DEMO_TAG, "  - WiFi: Connected and ready");
        ESP_LOGI(DEMO_TAG, "  - BLE Beacon: Will broadcast only while Wi-Fi has IP");
    } else {
        ESP_LOGI(DEMO_TAG, "  - WiFi: AP connect failed, waiting for recovery");
        ESP_LOGI(DEMO_TAG, "  - BLE Beacon: Paused until Wi-Fi gets IP");
    }
    ESP_LOGI(DEMO_TAG, "  - DHT11: Sampling every %d ms on GPIO %d", DHT11_SAMPLE_PERIOD_MS, DHT11_GPIO);
    ESP_LOGI(DEMO_TAG, "  - INMP411: I2S BCLK=%d, WS=%d, SD=%d", INMP411_I2S_BCLK_GPIO, INMP411_I2S_WS_GPIO, INMP411_I2S_SD_GPIO);
    ESP_LOGI(DEMO_TAG, "  - Coexistence: BLE priority mode");
    ESP_LOGI(DEMO_TAG, "========================================");

    xTaskCreate(dht11_task, "dht11_task", 4096, NULL, 5, NULL);

    if (inmp411_i2s_init() == ESP_OK) {
        xTaskCreate(inmp411_noise_task, "inmp411_task", INMP411_TASK_STACK_SIZE, NULL, 5, NULL);
    } else {
        ESP_LOGE(DEMO_TAG, "INMP411 init failed, noise task not started");
    }

    xTaskCreate(ble_status_task, "ble_status_task", 3072, NULL, 4, NULL);

    // TODO: 未来可在此添加定期上报设备状态到服务器的任务
    // 示例: xTaskCreate(report_status_task, "report", 4096, NULL, 5, NULL);
}

static void esp_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        ESP_LOGI(DEMO_TAG, "Advertising data set, status %d", param->adv_data_cmpl.status);
        adv_config_done &= (~ADV_CONFIG_FLAG);
        if (adv_config_done == 0) {
            s_ble_adv_ready = true;
            wifi_resume_ble_advertising();
        }
        break;
    case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
        ESP_LOGI(DEMO_TAG, "Advertising data raw set, status %d", param->adv_data_raw_cmpl.status);
        adv_config_done &= (~ADV_CONFIG_FLAG);
        if (adv_config_done == 0) {
            s_ble_adv_ready = true;
            wifi_resume_ble_advertising();
        }
        break;
    case ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT:
        ESP_LOGI(DEMO_TAG, "Scan response data set, status %d", param->scan_rsp_data_cmpl.status);
        adv_config_done &= (~SCAN_RSP_CONFIG_FLAG);
        if (adv_config_done == 0) {
            s_ble_adv_ready = true;
            wifi_resume_ble_advertising();
        }
        break;
    case ESP_GAP_BLE_SCAN_RSP_DATA_RAW_SET_COMPLETE_EVT:
        ESP_LOGI(DEMO_TAG, "Scan response data raw set, status %d", param->scan_rsp_data_raw_cmpl.status);
        adv_config_done &= (~SCAN_RSP_CONFIG_FLAG);
        if (adv_config_done == 0) {
            s_ble_adv_ready = true;
            wifi_resume_ble_advertising();
        }
        break;
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(DEMO_TAG, "Advertising start failed, status %d", param->adv_start_cmpl.status);
            s_ble_adv_active = false;
            break;
        }
        s_ble_adv_active = true;
        ESP_LOGI(DEMO_TAG, "Advertising start successfully");
        break;
    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
        if (param->adv_stop_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGW(DEMO_TAG, "Advertising stop failed, status %d", param->adv_stop_cmpl.status);
            break;
        }
        s_ble_adv_active = false;
        ESP_LOGI(DEMO_TAG, "Advertising stopped");
        break;
    default:
        break;
    }
}
