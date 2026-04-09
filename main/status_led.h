#pragma once

#include <stdint.h>

#include "app_context.h"

void status_led_init(app_context_t *ctx);
void status_led_set_rgb(app_context_t *ctx, uint8_t red, uint8_t green, uint8_t blue);
