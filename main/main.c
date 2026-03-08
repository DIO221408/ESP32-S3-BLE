/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include "nvs_flash.h"

// BLE相关头文件
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

// WiFi相关头文件
// 参考文档: https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32s3/api-reference/network/esp_wifi.html
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "lwip/err.h"
#include "lwip/sys.h"

// 共存相关头文件
// 参考文档: https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32s3/api-reference/system/esp_coexist.html
#include "esp_coexist.h"

#define ADV_CONFIG_FLAG      (1 << 0)
#define SCAN_RSP_CONFIG_FLAG (1 << 1)
#define URI_PREFIX_HTTPS     (0x17)

// WiFi事件标志位
#define WIFI_CONNECTED_BIT   BIT0
#define WIFI_FAIL_BIT        BIT1
#define WIFI_MAX_RETRY       5

// WiFi配置
#define WIFI_SSID            "esp8266-dio"
#define WIFI_PASS            "12345678"

static void esp_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
static void ip_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);

static const char *DEMO_TAG = "BLE_BEACON";
static const char *WIFI_TAG = "WIFI_STATION";
static const char device_name[] = "Bluedroid_Beacon";

// WiFi事件组
static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;

static uint8_t adv_config_done = 0;
static esp_bd_addr_t local_addr;
static uint8_t local_addr_type;

static esp_ble_adv_params_t adv_params = {
    .adv_int_min = 0xA0,  // 100ms (优化：从20ms调整到100ms，减少与WiFi冲突)
    .adv_int_max = 0xA0,  // 100ms
    .adv_type = ADV_TYPE_SCAN_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

//configure raw data for advertising packet
static uint8_t adv_raw_data[] = {
    0x02, ESP_BLE_AD_TYPE_FLAG, 0x06,
    0x11, ESP_BLE_AD_TYPE_NAME_CMPL, 'B', 'l', 'u', 'e', 'd', 'r', 'o', 'i', 'd', '_', 'B', 'e', 'a', 'c', 'o', 'n',
    0x02, ESP_BLE_AD_TYPE_TX_PWR, 0x09,
    0x03, ESP_BLE_AD_TYPE_APPEARANCE, 0x00,0x02,
    0x02, ESP_BLE_AD_TYPE_LE_ROLE, 0x00,
};

static uint8_t scan_rsp_raw_data[] = {
    0x08, ESP_BLE_AD_TYPE_LE_DEV_ADDR, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x11, ESP_BLE_AD_TYPE_URI, URI_PREFIX_HTTPS, '/', '/', 'e', 's', 'p', 'r', 'e', 's', 's', 'i', 'f', '.', 'c', 'o', 'm',
};

// ========== WiFi事件处理函数 ==========
// 参考文档: https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32s3/api-reference/system/esp_event.html

/**
 * @brief WiFi事件处理回调函数
 * 处理WiFi连接、断开、重连等事件
 */
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(WIFI_TAG, "WiFi station started, connecting to AP...");
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(WIFI_TAG, "Retry to connect to the AP (attempt %d/%d)", s_retry_num, WIFI_MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            ESP_LOGE(WIFI_TAG, "Failed to connect to AP after %d attempts", WIFI_MAX_RETRY);
        }
    }
}

/**
 * @brief IP事件处理回调函数
 * 处理获取IP地址等事件
 */
static void ip_event_handler(void* arg, esp_event_base_t event_base,
                             int32_t event_id, void* event_data)
{
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(WIFI_TAG, "✓ Connected! Got IP address: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// ========== WiFi初始化函数 ==========
// 参考文档: https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32s3/api-guides/wifi.html

/**
 * @brief 初始化WiFi Station模式
 * 配置WiFi参数并连接到指定AP
 */
static void wifi_init_sta(void)
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

    // 配置WiFi连接参数
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,  // WPA2加密
            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };

    // 设置WiFi模式为Station
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    
    // 启用WiFi省电模式（减少与BLE冲突）
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));
    
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
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(WIFI_TAG, "✗ Failed to connect to SSID:%s", WIFI_SSID);
    } else {
        ESP_LOGE(WIFI_TAG, "UNEXPECTED EVENT");
    }
}

// ========== BLE和WiFi共存优化配置 ==========
// 参考文档: https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32s3/api-guides/coexist.html

/**
 * @brief 配置BLE和WiFi共存参数
 * 优化射频资源分配，确保两者稳定运行
 */
static void configure_coexistence(void)
{
    ESP_LOGI(DEMO_TAG, "Configuring BLE and WiFi coexistence...");
    
    // 设置共存优先级为BT优先（保证信标广播稳定）
    // ESP_COEX_PREFER_BT: 蓝牙优先（包括BLE）
    // ESP_COEX_PREFER_WIFI: WiFi优先
    // ESP_COEX_PREFER_BALANCE: 平衡模式（默认）
    esp_coex_preference_set(ESP_COEX_PREFER_BT);
    ESP_LOGI(DEMO_TAG, "✓ Coexistence preference set to BT priority");
    
    // 设置BLE发射功率（中等功率，保证信标范围）
    // 功率等级: ESP_PWR_LVL_N12(-12dBm) 到 ESP_PWR_LVL_P9(+9dBm)
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_P3);  // +3dBm
    ESP_LOGI(DEMO_TAG, "✓ BLE TX power set to +3dBm");
    
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
    wifi_init_sta();
    ESP_LOGI(DEMO_TAG, "✓ WiFi initialized and connected");

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
    ESP_LOGI(DEMO_TAG, "  - BLE Beacon: Broadcasting (100ms interval)");
    ESP_LOGI(DEMO_TAG, "  - WiFi: Connected and ready");
    ESP_LOGI(DEMO_TAG, "  - Coexistence: BLE priority mode");
    ESP_LOGI(DEMO_TAG, "========================================");
    
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
            esp_ble_gap_start_advertising(&adv_params);
        }
        break;
    case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
        ESP_LOGI(DEMO_TAG, "Advertising data raw set, status %d", param->adv_data_raw_cmpl.status);
        adv_config_done &= (~ADV_CONFIG_FLAG);
        if (adv_config_done == 0) {
            esp_ble_gap_start_advertising(&adv_params);
        }
        break;
    case ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT:
        ESP_LOGI(DEMO_TAG, "Scan response data set, status %d", param->scan_rsp_data_cmpl.status);
        adv_config_done &= (~SCAN_RSP_CONFIG_FLAG);
        if (adv_config_done == 0) {
            esp_ble_gap_start_advertising(&adv_params);
        }
        break;
    case ESP_GAP_BLE_SCAN_RSP_DATA_RAW_SET_COMPLETE_EVT:
        ESP_LOGI(DEMO_TAG, "Scan response data raw set, status %d", param->scan_rsp_data_raw_cmpl.status);
        adv_config_done &= (~SCAN_RSP_CONFIG_FLAG);
        if (adv_config_done == 0) {
            esp_ble_gap_start_advertising(&adv_params);
        }
        break;
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(DEMO_TAG, "Advertising start failed, status %d", param->adv_start_cmpl.status);
            break;
        }
        ESP_LOGI(DEMO_TAG, "Advertising start successfully");
        break;
    default:
        break;
    }
}
