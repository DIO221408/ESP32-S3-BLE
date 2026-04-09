#pragma once

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"

#define ADV_CONFIG_FLAG      (1 << 0)
#define SCAN_RSP_CONFIG_FLAG (1 << 1)
#define URI_PREFIX_HTTPS     (0x17)

#define WIFI_CONNECTED_BIT   BIT0
#define WIFI_FAIL_BIT        BIT1
#define WIFI_MAX_RETRY       20

#define WIFI_BACKOFF_BASE_MS             1000
#define WIFI_BACKOFF_MAX_MS             30000
#define WIFI_COMEBACK_WINDOW_MS        120000
#define WIFI_COMEBACK_BACKOFF_CAP_MS     8000
#define WIFI_COOLDOWN_TRIGGER_FAILS         6
#define WIFI_COOLDOWN_WINDOW_MS         45000
#define WIFI_FORCE_RESET_TRIGGER_FAILS       8
#define WIFI_MAX_STATE_MACHINE_RESETS        3

#define WIFI_SSID            "fangdehuanxue"
#define WIFI_PASS            "xa125878"

#define MQTT_BROKER_URI      "mqtt://192.168.1.198:1883"
#define MQTT_USERNAME        "admin"
#define MQTT_PASSWORD        "public"
#define MQTT_KEEPALIVE_SEC   60

#define APP_BLE_DEVICE_NAME "Bluedroid_Beacon"

#define STATUS_LED_GPIO      48
#define STATUS_LED_PIXEL_NUM 1

#define DHT11_GPIO             GPIO_NUM_8
#define DHT11_SAMPLE_PERIOD_MS 7000
#define DHT11_TASK_STACK_SIZE  4096

#define INMP411_I2S_BCLK_GPIO   GPIO_NUM_12
#define INMP411_I2S_WS_GPIO     GPIO_NUM_13
#define INMP411_I2S_SD_GPIO     GPIO_NUM_14
#define INMP411_SAMPLE_RATE_HZ  16000
#define INMP411_FRAME_SAMPLES   1024
#define INMP411_NOISE_PERIOD_MS 5000
#define INMP411_TASK_STACK_SIZE 12288
#define INMP411_NOISE_DB_OFFSET 94.0f
#define INMP411_NOISE_EMA_ALPHA 0.20f

#define BLE_STATUS_TASK_STACK_SIZE 3072
#define BLE_STATUS_TASK_PRIORITY   4
#define SENSOR_TASK_PRIORITY       5

#define TELEMETRY_DEFAULT_RSSI -53
