| 支持的目标芯片 | ESP32 | ESP32-C2 | ESP32-C3 | ESP32-C5 | ESP32-C6 | ESP32-C61 | ESP32-H2 | ESP32-S3 |
| ----------------- | ----- | -------- | -------- | -------- | -------- | --------- | -------- | -------- |

# 景区智能导览系统 - 设备端（BLE信标）

## 项目概述

这是一个基于ESP32的BLE蓝牙信标（Beacon）项目，属于"景区智能导览系统"的设备端部分。该设备作为蓝牙信标部署在景区各个景点，为游客提供位置识别和信息推送服务。

## 技术架构

### 硬件平台
- 基于ESP32系列芯片（支持ESP32、ESP32-C2、ESP32-C3、ESP32-S3等多个型号）
- 使用ESP-IDF开发框架（乐鑫官方物联网开发框架）

### 核心技术
- BLE（低功耗蓝牙）技术
- Bluedroid蓝牙协议栈
- FreeRTOS实时操作系统

## 主要功能

1. **蓝牙广播**：设备名称为"Bluedroid_Beacon"，以20ms的间隔持续广播
2. **扫描响应**：当接收到扫描请求时，返回包含设备地址和URI信息的响应数据
3. **位置标识**：作为蓝牙信标供游客手机APP扫描识别

## 应用场景

- **景点定位**：部署在景区各个景点作为位置标识
- **位置识别**：游客的手机APP扫描信标，识别当前所在位置
- **信息推送**：根据信标位置触发相应的景点介绍和导览信息
- **智能导览**：配合移动端APP实现自动化景点讲解

## 工作原理

1. 初始化NVS闪存和蓝牙控制器
2. 启动Bluedroid蓝牙协议栈
3. 配置广播数据和扫描响应数据
4. 开始BLE广播，使周围的设备可以扫描到这个信标

测试时可以使用任何BLE扫描应用程序。

## 快速开始

### 设置目标芯片

在配置和编译项目之前，请确保设置正确的目标芯片：

```shell
idf.py set-target <芯片型号>
```

例如，如果使用ESP32S3芯片：

```shell
idf.py set-target esp32s3
```

### 编译和烧录

运行以下命令来编译、烧录和监控项目：

```shell
idf.py -p <端口号> flash monitor
```

例如，在Windows系统上，如果对应的串口是`COM3`：

```shell
idf.py -p COM3 flash monitor
```

在Linux/Mac系统上：

```shell
idf.py -p /dev/ttyUSB0 flash monitor
```

（退出串口监视器，按 `Ctrl-]`）

更多详细步骤请参考 [ESP-IDF 入门指南](https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32/get-started/index.html)

## 代码说明

### 程序概览

1. 初始化NVS闪存、Bluedroid协议栈和GAP服务；配置Bluedroid协议栈并启动蓝牙任务线程
2. 设置广播数据和扫描响应数据，然后配置广播参数并开始广播

### 程序入口

`main.c` 中的 `app_main` 是所有ESP32应用程序的入口点。通常，应用程序的初始化应该在这里完成。

首先，调用 `nvs_flash_init`、`esp_bt_controller_init` 和 `esp_bt_controller_enable` 函数来初始化NVS闪存和蓝牙控制器。

```C
void app_main(void) {
    esp_err_t ret;

    // 初始化NVS
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
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
    ...
}
```

然后，调用 `esp_bluedroid_init` 和 `esp_bluedroid_enable` 函数来初始化Bluedroid协议栈。

```C
void app_main(void) {
    ...

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

    ...
}
```

之后，调用 `esp_ble_gap_register_callback` 注册 `esp_gap_cb` 函数作为GAP服务的回调函数。从此，所有GAP事件都将由 `esp_gap_cb` 函数处理。

```C
void app_main(void) {
    ...
    
    ret = esp_ble_gap_register_callback(esp_gap_cb);
    if (ret) {
        ESP_LOGE(DEMO_TAG, "gap register error, error code = %x", ret);
        return;
    }

    ...
}
```

### 开始广播

作为信标设备，我们将开始广播，并在收到扫描请求时发送扫描响应。为了实现这一点，我们需要在广播开始之前设置广播数据和扫描响应数据。具体步骤如下：

1. 初始化广播和扫描响应字段结构体 `adv_raw_data` 和 `scan_rsp_raw_data`，以及广播参数结构体 `adv_params`。
2. 根据需求设置广播参数：
    - 广播间隔设置为20ms
    - 广播PDU类型设置为 `ADV_SCAN_IND`
    - 广播地址类型为公共地址
    - 广播信道设置为所有信道（37、38、39信道都将用于广播）
3. 设置广播原始数据和扫描响应原始数据

```C
static esp_ble_adv_params_t adv_params = {
    .adv_int_min = 0x20,  // 20ms
    .adv_int_max = 0x20,  // 20ms
    .adv_type = ADV_TYPE_SCAN_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

// 配置广播数据包的原始数据
static uint8_t adv_raw_data[] = {
    0x02, ESP_BLE_AD_TYPE_FLAG, 0x06,
    0x11, ESP_BLE_AD_TYPE_NAME_CMPL, 'B', 'l', 'u', 'e', 'd', 'r', 'o', 'i', 'd', '_', 'B', 'e', 'a', 'c', 'o', 'n',
    0x02, ESP_BLE_AD_TYPE_TX_PWR, 0x09,
    0x03, ESP_BLE_AD_TYPE_APPEARANCE, 0x00,0x02,
    0x02, ESP_BLE_AD_TYPE_LE_ROLE, 0x00,
};

static uint8_t scan_rsp_raw_data[] = {
    0x08, ESP_BLE_AD_TYPE_LE_DEV_ADDR, 0x46, 0xF5, 0x06, 0xBD, 0xF5, 0xF0, 0x00,
    0x11, ESP_BLE_AD_TYPE_URI, 0x17, 0x2F, 0x2F, 0x65, 0x73, 0x70, 0x72, 0x65, 0x73, 0x73, 0x69, 0x66, 0x2E, 0x63, 0x6F, 0x6D,
};
```

4. 使用 `esp_ble_gap_config_adv_data_raw` 配置广播原始数据。在响应原始数据中设置设备地址，并调用 `esp_ble_gap_config_scan_rsp_data_raw` 配置扫描响应原始数据。
    - 由于广播数据包中的 `AdvData` **不能超过31字节**，额外的信息必须放在扫描响应数据包中
    - 我们将espressif的官方网站链接放入URI字段
    - 注意设备地址的字节序

```C
void app_main(void) {
    ...
    
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
}
```

5. 当广播原始数据和扫描响应原始数据都成功设置后，通过调用 `esp_ble_gap_start_advertising` 开始广播

```C
static void esp_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    ...
    case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
        ESP_LOGI(DEMO_TAG, "Advertising data raw set, status %d", param->adv_data_raw_cmpl.status);
        adv_config_done &= (~ADV_CONFIG_FLAG);
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
}
```

6. 如果广播成功启动，您将收到 `ESP_GAP_BLE_ADV_START_COMPLETE_EVT` GAP事件

```C
static void esp_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    ...
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(DEMO_TAG, "Advertising start failed, status %d", param->adv_start_cmpl.status);
            break;
        }
        ESP_LOGI(DEMO_TAG, "Advertising start successfully");
        break;
}
```

### 测试观察

如果一切正常，您应该能够在BLE扫描设备上看到名为 `Bluedroid_Beacon` 的设备，它会广播大量信息，包括一个URI "https://espressif.com"（espressif的官方网站），这正是我们所期望的。

## 后续开发计划

为了将此项目应用于实际的景区智能导览系统，建议进行以下定制开发：

1. **修改设备标识**
   - 自定义设备名称，使用景点相关的命名
   - 添加景点唯一标识UUID
   - 配置景点特定的广播数据

2. **优化功耗**
   - 调整广播间隔以平衡响应速度和电池寿命
   - 实现低功耗模式
   - 添加电源管理功能

3. **扩展功能**
   - 集成温湿度传感器（监测环境数据）
   - 添加GPS模块（精确定位）
   - 实现OTA固件升级功能

4. **数据格式**
   - 设计统一的数据格式协议
   - 添加景点ID、类型等元数据
   - 支持多语言信息广播

## 故障排除

如有任何技术问题，请在GitHub上提交 [issue](https://github.com/espressif/esp-idf/issues)。我们会尽快回复您。

## 许可证

本项目基于ESP-IDF示例代码，遵循 Unlicense 或 CC0-1.0 许可证。
