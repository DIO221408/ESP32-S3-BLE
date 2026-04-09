#include "status_led.h"

#include "esp_log.h"

static const char *TAG = "STATUS_LED";

void status_led_set_rgb(app_context_t *ctx, uint8_t red, uint8_t green, uint8_t blue)
{
    if (ctx->status_led == NULL) {
        return;
    }

    led_strip_set_pixel(ctx->status_led, 0, red, green, blue);
    led_strip_refresh(ctx->status_led);
}

void status_led_init(app_context_t *ctx)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = STATUS_LED_GPIO,
        .max_leds = STATUS_LED_PIXEL_NUM,
    };

    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000,
    };

    esp_err_t ret = led_strip_new_rmt_device(&strip_config, &rmt_config, &ctx->status_led);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Status LED init failed on GPIO %d: %s", STATUS_LED_GPIO, esp_err_to_name(ret));
        return;
    }

    status_led_set_rgb(ctx, 0, 0, 0);
    ESP_LOGI(TAG, "Status LED initialized on GPIO %d", STATUS_LED_GPIO);
}
