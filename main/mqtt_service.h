#pragma once

#include "app_context.h"

void mqtt_service_start(app_context_t *ctx);
esp_err_t mqtt_service_publish_telemetry(app_context_t *ctx,
                                         float temperature,
                                         float humidity,
                                         float noise_db,
                                         int rssi);
