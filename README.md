# 景区智能导览系统设备端固件

基于 ESP32 的景区导览设备端固件，负责 BLE 广播、WiFi 回传、MQTT 通信以及环境传感器采集。

当前主目标芯片为 `esp32s3`，同时保留 `esp32c3`、`esp32c2` 的配置文件。

## 功能概览

- BLE Beacon 广播，供游客端 App 识别设备与位置
- WiFi Station 连接景区网络，并带有断线重连与状态恢复逻辑
- MQTT 上下行通信，按设备 MAC 生成独立主题
- DHT11 温湿度采集
- INMP411 麦克风噪声采集
- WS2812 状态灯反馈 WiFi / MQTT 状态
- BLE 与 WiFi 共存参数调优

## 目录结构

```text
project-name/
├─ main/
│  ├─ main.c               # 应用启动编排
│  ├─ app_config.h         # 项目级宏配置
│  ├─ app_context.h/.c     # 跨模块共享运行时上下文
│  ├─ wifi_manager.h/.c    # WiFi 初始化、重连、IP 事件
│  ├─ ble_beacon.h/.c      # BLE 栈初始化、广播、状态任务
│  ├─ mqtt_service.h/.c    # MQTT 连接、订阅、遥测发布
│  ├─ sensor_tasks.h/.c    # DHT11 / INMP411 采集任务
│  ├─ time_sync.h/.c       # SNTP 时间同步
│  ├─ status_led.h/.c      # WS2812 状态灯控制
│  ├─ CMakeLists.txt
│  └─ idf_component.yml
├─ partitions.csv
├─ sdkconfig.defaults*
└─ README.md
```

## 模块说明

### `main.c`

仅保留启动流程：

1. 初始化运行时上下文
2. 初始化 NVS
3. 初始化 WiFi
4. 初始化 BLE 栈
5. 配置 BLE/WiFi 共存
6. 启动 BLE 广播
7. 启动传感器任务与 BLE 状态任务

### `wifi_manager`

- 负责 WiFi Station 初始化
- 处理断线重连指数退避
- 在异常失败次数过多时强制重置 WiFi 状态机
- 在拿到 IP 后触发 SNTP 与 MQTT 启动

### `ble_beacon`

- 初始化 BLE 控制器与 Bluedroid
- 配置广播包与扫描响应包
- 在 WiFi 恢复后恢复广播，在 WiFi 中断时暂停广播
- 周期输出 BLE 状态日志

### `mqtt_service`

- 根据设备 MAC 生成 `device/<MAC>/cmd` 与 `device/<MAC>/telemetry`
- 处理 MQTT 连接事件
- 封装遥测 JSON 生成与发布逻辑

### `sensor_tasks`

- DHT11 周期采集温湿度
- INMP411 通过 I2S 计算噪声值
- 将温湿度与噪声组合成遥测数据上报

## 构建与烧录

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p COM8 flash monitor
```

Linux / macOS 示例：

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

## 关键配置

当前仍然使用宏硬编码配置，集中在 [main/app_config.h](main/app_config.h)：

- `WIFI_SSID`
- `WIFI_PASS`
- `MQTT_BROKER_URI`
- `MQTT_USERNAME`
- `MQTT_PASSWORD`
- DHT11 / INMP411 / WS2812 引脚定义

后续如果要继续工程化，建议优先迁移到 NVS 或独立配网流程。

## 遥测格式

发布到 `device/<MAC>/telemetry` 的消息格式如下：

```json
{
  "type": "telemetry",
  "deviceId": "<MAC>",
  "ts": 1710000000000,
  "seq": 1,
  "data": {
    "temperature": 25.0,
    "humidity": 60.0,
    "noise": 45.3,
    "rssi": -53
  }
}
```

## 说明

- 当前项目没有单元测试工程
- 本地验证主要依赖 `idf.py build` 与串口日志
- `managed_components/` 中的 `espressif/led_strip` 由 ESP-IDF 自动管理
