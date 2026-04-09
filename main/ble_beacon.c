#include "ble_beacon.h"

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_coexist.h"
#include "esp_gap_ble_api.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"

static const char *TAG = "BLE_BEACON";
static app_context_t *s_ble_ctx = NULL;

static void esp_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);
static void ble_status_task(void *arg);

static esp_ble_adv_params_t s_adv_params = {
    .adv_int_min = 0x12C0,
    .adv_int_max = 0x12C0,
    .adv_type = ADV_TYPE_SCAN_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static uint8_t s_adv_raw_data[] = {
    0x02, ESP_BLE_AD_TYPE_FLAG, 0x06,
    0x11, ESP_BLE_AD_TYPE_NAME_CMPL, 'B', 'l', 'u', 'e', 'd', 'r', 'o', 'i', 'd', '_', 'B', 'e', 'a', 'c', 'o', 'n',
    0x02, ESP_BLE_AD_TYPE_TX_PWR, 0x09,
    0x03, ESP_BLE_AD_TYPE_APPEARANCE, 0x00, 0x02,
    0x02, ESP_BLE_AD_TYPE_LE_ROLE, 0x00,
};

static uint8_t s_scan_rsp_raw_data[] = {
    0x08, ESP_BLE_AD_TYPE_LE_DEV_ADDR, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x11, ESP_BLE_AD_TYPE_URI, URI_PREFIX_HTTPS, '/', '/', 'e', 's', 'p', 'r', 'e', 's', 's', 'i', 'f', '.', 'c', 'o', 'm',
};

esp_err_t ble_beacon_stack_init(void)
{
    esp_err_t ret = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (ret != ESP_OK) {
        return ret;
    }

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_bluedroid_init();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_bluedroid_enable();
    if (ret != ESP_OK) {
        return ret;
    }

    return ESP_OK;
}

void ble_beacon_configure_coexistence(void)
{
    ESP_LOGI(TAG, "Configuring BLE and WiFi coexistence...");
    esp_coex_preference_set(ESP_COEX_PREFER_WIFI);
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_N0);
    esp_wifi_set_max_tx_power(52);
    ESP_LOGI(TAG, "Coexistence configuration completed");
}

esp_err_t ble_beacon_init(app_context_t *ctx)
{
    esp_err_t ret;

    s_ble_ctx = ctx;
    ctx->adv_config_done = ADV_CONFIG_FLAG | SCAN_RSP_CONFIG_FLAG;

    ret = esp_ble_gap_register_callback(esp_gap_cb);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_ble_gap_set_device_name(APP_BLE_DEVICE_NAME);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_ble_gap_config_adv_data_raw(s_adv_raw_data, sizeof(s_adv_raw_data));
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_ble_gap_get_local_used_addr(ctx->local_addr, &ctx->local_addr_type);
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGI(TAG,
             "BLE local addr: %02X:%02X:%02X:%02X:%02X:%02X (type=%u)",
             ctx->local_addr[0],
             ctx->local_addr[1],
             ctx->local_addr[2],
             ctx->local_addr[3],
             ctx->local_addr[4],
             ctx->local_addr[5],
             (unsigned int)ctx->local_addr_type);

    s_scan_rsp_raw_data[2] = ctx->local_addr[5];
    s_scan_rsp_raw_data[3] = ctx->local_addr[4];
    s_scan_rsp_raw_data[4] = ctx->local_addr[3];
    s_scan_rsp_raw_data[5] = ctx->local_addr[2];
    s_scan_rsp_raw_data[6] = ctx->local_addr[1];
    s_scan_rsp_raw_data[7] = ctx->local_addr[0];

    return esp_ble_gap_config_scan_rsp_data_raw(s_scan_rsp_raw_data, sizeof(s_scan_rsp_raw_data));
}

void ble_beacon_pause(app_context_t *ctx)
{
    if (!ctx->ble_adv_active) {
        return;
    }

    esp_err_t ret = esp_ble_gap_stop_advertising();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to pause BLE advertising during Wi-Fi outage: %s", esp_err_to_name(ret));
        return;
    }

    ctx->ble_adv_active = false;
    ESP_LOGI(TAG, "BLE advertising paused during Wi-Fi outage");
}

void ble_beacon_resume_if_wifi_ready(app_context_t *ctx)
{
    if (!ctx->ble_adv_ready || ctx->ble_adv_active || ctx->wifi_event_group == NULL) {
        return;
    }

    EventBits_t bits = xEventGroupGetBits(ctx->wifi_event_group);
    if ((bits & WIFI_CONNECTED_BIT) == 0) {
        return;
    }

    esp_err_t ret = esp_ble_gap_start_advertising(&s_adv_params);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to resume BLE advertising after Wi-Fi recovery: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "BLE advertising resume requested after Wi-Fi recovery");
}

void ble_beacon_start_status_task(app_context_t *ctx)
{
    xTaskCreate(ble_status_task,
                "ble_status_task",
                BLE_STATUS_TASK_STACK_SIZE,
                ctx,
                BLE_STATUS_TASK_PRIORITY,
                NULL);
}

static void ble_status_task(void *arg)
{
    app_context_t *ctx = (app_context_t *)arg;

    while (true) {
        EventBits_t bits = 0;
        if (ctx->wifi_event_group != NULL) {
            bits = xEventGroupGetBits(ctx->wifi_event_group);
        }

        ESP_LOGI(TAG,
                 "BLE_STATUS ready=%d active=%d wifi_connected=%d wifi_fail=%d adv_int=%.1fms addr=%02X:%02X:%02X:%02X:%02X:%02X type=%u",
                 ctx->ble_adv_ready ? 1 : 0,
                 ctx->ble_adv_active ? 1 : 0,
                 (bits & WIFI_CONNECTED_BIT) ? 1 : 0,
                 (bits & WIFI_FAIL_BIT) ? 1 : 0,
                 (double)s_adv_params.adv_int_min * 0.625,
                 ctx->local_addr[0],
                 ctx->local_addr[1],
                 ctx->local_addr[2],
                 ctx->local_addr[3],
                 ctx->local_addr[4],
                 ctx->local_addr[5],
                 (unsigned int)ctx->local_addr_type);

        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

static void esp_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    app_context_t *ctx = s_ble_ctx;

    if (ctx == NULL) {
        return;
    }

    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        ESP_LOGI(TAG, "Advertising data set, status %d", param->adv_data_cmpl.status);
        ctx->adv_config_done &= (~ADV_CONFIG_FLAG);
        if (ctx->adv_config_done == 0) {
            ctx->ble_adv_ready = true;
            ble_beacon_resume_if_wifi_ready(ctx);
        }
        break;
    case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
        ESP_LOGI(TAG, "Advertising data raw set, status %d", param->adv_data_raw_cmpl.status);
        ctx->adv_config_done &= (~ADV_CONFIG_FLAG);
        if (ctx->adv_config_done == 0) {
            ctx->ble_adv_ready = true;
            ble_beacon_resume_if_wifi_ready(ctx);
        }
        break;
    case ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT:
        ESP_LOGI(TAG, "Scan response data set, status %d", param->scan_rsp_data_cmpl.status);
        ctx->adv_config_done &= (~SCAN_RSP_CONFIG_FLAG);
        if (ctx->adv_config_done == 0) {
            ctx->ble_adv_ready = true;
            ble_beacon_resume_if_wifi_ready(ctx);
        }
        break;
    case ESP_GAP_BLE_SCAN_RSP_DATA_RAW_SET_COMPLETE_EVT:
        ESP_LOGI(TAG, "Scan response data raw set, status %d", param->scan_rsp_data_raw_cmpl.status);
        ctx->adv_config_done &= (~SCAN_RSP_CONFIG_FLAG);
        if (ctx->adv_config_done == 0) {
            ctx->ble_adv_ready = true;
            ble_beacon_resume_if_wifi_ready(ctx);
        }
        break;
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(TAG, "Advertising start failed, status %d", param->adv_start_cmpl.status);
            ctx->ble_adv_active = false;
            break;
        }
        ctx->ble_adv_active = true;
        ESP_LOGI(TAG, "Advertising start successfully");
        break;
    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
        if (param->adv_stop_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGW(TAG, "Advertising stop failed, status %d", param->adv_stop_cmpl.status);
            break;
        }
        ctx->ble_adv_active = false;
        ESP_LOGI(TAG, "Advertising stopped");
        break;
    default:
        break;
    }
}
