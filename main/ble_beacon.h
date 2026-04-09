#pragma once

#include "app_context.h"

esp_err_t ble_beacon_stack_init(void);
void ble_beacon_configure_coexistence(void);
esp_err_t ble_beacon_init(app_context_t *ctx);
void ble_beacon_pause(app_context_t *ctx);
void ble_beacon_resume_if_wifi_ready(app_context_t *ctx);
void ble_beacon_start_status_task(app_context_t *ctx);
