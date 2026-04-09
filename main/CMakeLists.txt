idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash bt esp_wifi esp_event esp_netif esp_coex mqtt led_strip driver esp_timer esp_driver_i2s
)

