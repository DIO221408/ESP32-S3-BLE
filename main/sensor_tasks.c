#include <math.h>

#include "sensor_tasks.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"
#include "mqtt_service.h"

static const char *TAG = "SENSORS";

static esp_err_t dht11_read(int *temperature_c, int *humidity_pct);
static void dht11_task(void *arg);
static esp_err_t inmp411_i2s_init(app_context_t *ctx);
static float inmp411_measure_noise_db(app_context_t *ctx);
static void inmp411_noise_task(void *arg);

void sensor_tasks_start(app_context_t *ctx)
{
    xTaskCreate(dht11_task,
                "dht11_task",
                DHT11_TASK_STACK_SIZE,
                ctx,
                SENSOR_TASK_PRIORITY,
                NULL);

    if (inmp411_i2s_init(ctx) == ESP_OK) {
        xTaskCreate(inmp411_noise_task,
                    "inmp411_task",
                    INMP411_TASK_STACK_SIZE,
                    ctx,
                    SENSOR_TASK_PRIORITY,
                    NULL);
        return;
    }

    ESP_LOGE(TAG, "INMP411 init failed, noise task not started");
}

static esp_err_t dht11_read(int *temperature_c, int *humidity_pct)
{
    uint8_t data[5] = {0};

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << DHT11_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    gpio_set_level(DHT11_GPIO, 0);
    esp_rom_delay_us(20000);
    gpio_set_level(DHT11_GPIO, 1);
    esp_rom_delay_us(30);

    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    int64_t start = esp_timer_get_time();
    while (gpio_get_level(DHT11_GPIO) == 1) {
        if (esp_timer_get_time() - start > 100) {
            return ESP_ERR_TIMEOUT;
        }
    }

    start = esp_timer_get_time();
    while (gpio_get_level(DHT11_GPIO) == 0) {
        if (esp_timer_get_time() - start > 100) {
            return ESP_ERR_TIMEOUT;
        }
    }

    start = esp_timer_get_time();
    while (gpio_get_level(DHT11_GPIO) == 1) {
        if (esp_timer_get_time() - start > 100) {
            return ESP_ERR_TIMEOUT;
        }
    }

    for (int i = 0; i < 40; i++) {
        start = esp_timer_get_time();
        while (gpio_get_level(DHT11_GPIO) == 0) {
            if (esp_timer_get_time() - start > 80) {
                return ESP_ERR_TIMEOUT;
            }
        }

        start = esp_timer_get_time();
        while (gpio_get_level(DHT11_GPIO) == 1) {
            if (esp_timer_get_time() - start > 120) {
                return ESP_ERR_TIMEOUT;
            }
        }

        int high_us = (int)(esp_timer_get_time() - start);
        data[i / 8] <<= 1;
        if (high_us > 40) {
            data[i / 8] |= 1;
        }
    }

    uint8_t checksum = (uint8_t)(data[0] + data[1] + data[2] + data[3]);
    if (checksum != data[4]) {
        return ESP_ERR_INVALID_CRC;
    }

    *humidity_pct = data[0];
    *temperature_c = data[2];
    return ESP_OK;
}

static void dht11_task(void *arg)
{
    app_context_t *ctx = (app_context_t *)arg;

    ESP_LOGI(TAG, "DHT11 task started on GPIO %d, period %d ms", DHT11_GPIO, DHT11_SAMPLE_PERIOD_MS);

    while (true) {
        int temperature_c = 0;
        int humidity_pct = 0;
        esp_err_t ret = dht11_read(&temperature_c, &humidity_pct);

        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "DHT11 -> Temperature: %d C, Humidity: %d %%", temperature_c, humidity_pct);

            esp_err_t publish_ret = mqtt_service_publish_telemetry(ctx,
                                                                   (float)temperature_c,
                                                                   (float)humidity_pct,
                                                                   ctx->inmp411_noise_ema_db,
                                                                   TELEMETRY_DEFAULT_RSSI);
            if (publish_ret != ESP_OK && publish_ret != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG, "Telemetry publish failed: %s", esp_err_to_name(publish_ret));
            }
        } else if (ret == ESP_ERR_INVALID_CRC) {
            ESP_LOGW(TAG, "DHT11 checksum mismatch");
        } else {
            ESP_LOGW(TAG, "DHT11 read timeout");
        }

        vTaskDelay(pdMS_TO_TICKS(DHT11_SAMPLE_PERIOD_MS));
    }
}

static esp_err_t inmp411_i2s_init(app_context_t *ctx)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &ctx->i2s_rx_chan));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(INMP411_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = INMP411_I2S_BCLK_GPIO,
            .ws = INMP411_I2S_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din = INMP411_I2S_SD_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(ctx->i2s_rx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(ctx->i2s_rx_chan));

    ESP_LOGI(TAG,
             "INMP411 I2S initialized (BCLK=%d, WS=%d, SD=%d)",
             INMP411_I2S_BCLK_GPIO,
             INMP411_I2S_WS_GPIO,
             INMP411_I2S_SD_GPIO);

    return ESP_OK;
}

static float inmp411_measure_noise_db(app_context_t *ctx)
{
    size_t bytes_read = 0;

    esp_err_t ret = i2s_channel_read(ctx->i2s_rx_chan,
                                     ctx->inmp411_samples,
                                     sizeof(ctx->inmp411_samples),
                                     &bytes_read,
                                     pdMS_TO_TICKS(200));
    if (ret != ESP_OK || bytes_read == 0) {
        return -1.0f;
    }

    int sample_count = (int)(bytes_read / sizeof(int32_t));
    if (sample_count <= 0) {
        return -1.0f;
    }

    double mean = 0.0;
    for (int i = 0; i < sample_count; i++) {
        int32_t s24 = ctx->inmp411_samples[i] >> 8;
        mean += (double)s24;
    }
    mean /= (double)sample_count;

    double sum_sq = 0.0;
    for (int i = 0; i < sample_count; i++) {
        int32_t s24 = ctx->inmp411_samples[i] >> 8;
        double centered = (double)s24 - mean;
        double norm = centered / 8388608.0;
        sum_sq += norm * norm;
    }

    double rms = sqrt(sum_sq / (double)sample_count);
    if (rms < 1e-9) {
        rms = 1e-9;
    }

    float raw_dbfs = 20.0f * log10f((float)rms);
    float noise_db_est = raw_dbfs + INMP411_NOISE_DB_OFFSET;

    if (ctx->inmp411_noise_ema_db < 0.0f) {
        ctx->inmp411_noise_ema_db = noise_db_est;
    } else {
        ctx->inmp411_noise_ema_db = (INMP411_NOISE_EMA_ALPHA * noise_db_est) +
                                    ((1.0f - INMP411_NOISE_EMA_ALPHA) * ctx->inmp411_noise_ema_db);
    }

    ESP_LOGI(TAG,
             "INMP411 raw_dbfs=%.1f dBFS, noise_db_est=%.1f dB (offset=%.1f)",
             raw_dbfs,
             ctx->inmp411_noise_ema_db,
             INMP411_NOISE_DB_OFFSET);

    return ctx->inmp411_noise_ema_db;
}

static void inmp411_noise_task(void *arg)
{
    app_context_t *ctx = (app_context_t *)arg;

    ESP_LOGI(TAG,
             "INMP411 noise task started, sample_rate=%dHz, period=%dms",
             INMP411_SAMPLE_RATE_HZ,
             INMP411_NOISE_PERIOD_MS);

    while (true) {
        float noise_db = inmp411_measure_noise_db(ctx);
        if (noise_db >= 0.0f) {
            ESP_LOGI(TAG, "INMP411 -> Noise level: %.1f dB", noise_db);
        } else {
            ESP_LOGW(TAG, "INMP411 read failed");
        }

        UBaseType_t stack_words = uxTaskGetStackHighWaterMark(NULL);
        ESP_LOGI(TAG, "INMP411 task stack high watermark: %lu words", (unsigned long)stack_words);

        vTaskDelay(pdMS_TO_TICKS(INMP411_NOISE_PERIOD_MS));
    }
}
