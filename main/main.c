#include "nvs_flash.h"

#include "esp_log.h"

#include "app_context.h"
#include "ble_beacon.h"
#include "sensor_tasks.h"
#include "status_led.h"
#include "wifi_manager.h"

static const char *TAG = "APP";
static app_context_t s_app_ctx;

static void init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    ESP_ERROR_CHECK(ret);
}

static void log_startup_banner(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  Smart Tour Guide Device Firmware");
    ESP_LOGI(TAG, "  BLE + WiFi coexistence mode");
    ESP_LOGI(TAG, "========================================");
}

void app_main(void)
{
    app_context_init(&s_app_ctx);
    log_startup_banner();

    status_led_init(&s_app_ctx);

    ESP_LOGI(TAG, "[1/5] Initializing NVS...");
    init_nvs();

    ESP_LOGI(TAG, "[2/5] Initializing WiFi...");
    bool wifi_connected = wifi_manager_init_station(&s_app_ctx);

    ESP_LOGI(TAG, "[3/5] Initializing BLE stack...");
    ESP_ERROR_CHECK(ble_beacon_stack_init());

    ESP_LOGI(TAG, "[4/5] Configuring BLE-WiFi coexistence...");
    ble_beacon_configure_coexistence();

    ESP_LOGI(TAG, "[5/5] Configuring BLE advertising...");
    ESP_ERROR_CHECK(ble_beacon_init(&s_app_ctx));

    sensor_tasks_start(&s_app_ctx);
    ble_beacon_start_status_task(&s_app_ctx);

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "System initialization completed");
    ESP_LOGI(TAG, "WiFi state: %s", wifi_connected ? "connected" : "connect failed, waiting for recovery");
    ESP_LOGI(TAG, "BLE beacon: active only while Wi-Fi has IP");
    ESP_LOGI(TAG, "DHT11 period: %d ms on GPIO %d", DHT11_SAMPLE_PERIOD_MS, DHT11_GPIO);
    ESP_LOGI(TAG,
             "INMP411 pins: BCLK=%d, WS=%d, SD=%d",
             INMP411_I2S_BCLK_GPIO,
             INMP411_I2S_WS_GPIO,
             INMP411_I2S_SD_GPIO);
    ESP_LOGI(TAG, "========================================");
}
