idf_component_register(
    SRCS "sensor_tasks.c" "wifi_manager.c" "ble_beacon.c" "time_sync.c" "mqtt_service.c" "status_led.c" "app_context.c" "main.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash bt esp_wifi esp_event esp_netif esp_coex mqtt led_strip driver esp_timer esp_driver_i2s
)

