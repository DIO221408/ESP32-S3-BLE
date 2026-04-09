# 景区智能导览系统（设备端）

基于 ESP32 的 BLE 信标固件，面向景区导览场景。当前版本实现了 **BLE 广播 + WiFi 连接 + BLE/WiFi 共存优化**，用于在景点侧提供位置识别与后续联网扩展能力。

---

## 1. 项目定位

本项目用于部署在景区各景点的设备端节点，核心职责是：

- 持续发出 BLE 广播，供游客手机 App 扫描识别位置
- 连接 WiFi 网络，为后续云端通信/状态上报预留能力
- 通过共存参数优化 BLE 与 WiFi 同时工作时的稳定性

---

## 2. 当前已实现功能

### BLE 侧
- 设备名：`Bluedroid_Beacon`
- 广播类型：`ADV_SCAN_IND`
- 广播间隔：`100ms`（已从示例常见的 20ms 调整为 100ms，降低与 WiFi 冲突）
- 扫描响应包含：
  - 本机 BLE 地址
  - URI 字段（当前为 `https://espressif.com`）

### WiFi 侧
- Station 模式连接指定 AP
- 断线自动重连（最大重试次数：5）
- 获取 IP 后输出日志

### 共存优化
- 启用软件共存配置（见 `sdkconfig.defaults.esp32s3`）
- 运行时设置 BT 优先策略
- 设置 BLE / WiFi 发射功率
- 启用 WiFi 省电模式减少射频冲突

---

## 3. 开发环境

- 框架：ESP-IDF（当前工程配置为 v5.5.x）
- 芯片：支持 ESP32 系列，当前主要配置为 `esp32s3`
- 系统：Windows / Linux / macOS（按 ESP-IDF 官方流程）

---

## 4. 目录结构

```text
project-name/
├─ main/
│  ├─ main.c                 # 主程序（BLE + WiFi + 共存逻辑）
│  └─ CMakeLists.txt
├─ CMakeLists.txt            # 工程入口
├─ partitions.csv            # 自定义分区表
├─ sdkconfig.defaults        # 通用默认配置
├─ sdkconfig.defaults.esp32c2
├─ sdkconfig.defaults.esp32c3
├─ sdkconfig.defaults.esp32s3
├─ .vscode/                  # VS Code + ESP-IDF 插件配置
└─ .devcontainer/            # 容器化开发配置
```

---

## 5. 快速开始

## 5.1 设置目标芯片

```bash
idf.py set-target esp32s3
```

> 如使用其它芯片，请改为对应 target（如 `esp32c3`、`esp32c2`）。

## 5.2 编译

```bash
idf.py build
```

## 5.3 烧录并查看日志

Windows 示例：

```bash
idf.py -p COM8 flash monitor
```

Linux/macOS 示例：

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

退出监视器：`Ctrl-]`

---

## 6. 关键配置项

当前 WiFi 参数在 `main/main.c` 中通过宏定义：

- `WIFI_SSID`
- `WIFI_PASS`
- `WIFI_MAX_RETRY`

当前 BLE 参数在 `main/main.c` 中可直接修改：

- `device_name`
- `adv_params`（广播间隔、广播类型等）
- `adv_raw_data`
- `scan_rsp_raw_data`

---

## 7. 运行后预期日志

你应能看到类似信息：

- WiFi 启动并连接成功，打印 IP 地址
- BLE 广播数据配置成功
- 扫描响应配置成功
- `Advertising start successfully`

说明设备已进入稳定广播状态。

---

## 8. 面向景区业务的下一步改造建议

建议优先做以下最小改造：

1. 将设备名/广播数据替换为景点唯一标识（如景点 ID、区域 ID）
2. 将 WiFi 凭据改为可配置（NVS/配网），避免硬编码
3. 设计统一广播载荷格式（便于 App 端解析）
4. 增加设备在线状态上报任务（连接后定时上报）
5. 预留 OTA 升级能力

---

## 9. 参考文档

- ESP-IDF 编程指南：
  https://docs.espressif.com/projects/esp-idf/zh_CN/latest/
- WiFi API：
  https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32s3/api-reference/network/esp_wifi.html
- 共存机制：
  https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32s3/api-guides/coexist.html

---

## 10. 许可

本项目基于 ESP-IDF 示例演进，遵循 `Unlicense OR CC0-1.0`。