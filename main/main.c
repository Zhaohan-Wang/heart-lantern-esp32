/*
 * 心灯 ESP32 — 板级 GPIO + MPU6050 陀螺仪测试
 */

#include "board_gpio.h"
#include "board_i2c.h"
#include "board_pins.h"
#include "mpu6050.h"

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <inttypes.h>
#include <math.h>

static const char *TAG = "heart_lantern";

/** IMU 采样任务 */
static void mpu6050_task(void *arg)
{
    mpu6050_sample_t sample;
    float gyro_shake_threshold_dps = 120.0f;
    float accel_jolt_threshold_g = 0.35f;
    float last_gyro_mag = 0.0f;
    float last_accel_mag = 1.0f;

    (void)arg;

    while (true) {
        if (!mpu6050_is_ready()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (mpu6050_read(&sample) == ESP_OK) {
            bool shake = (sample.gyro_mag_dps > gyro_shake_threshold_dps) ||
                         (fabsf(sample.accel_mag_g - last_accel_mag) > accel_jolt_threshold_g);

            ESP_LOGI(TAG,
                     "IMU ax=%.2f ay=%.2f az=%.2f g | "
                     "gx=%.0f gy=%.0f gz=%.0f dps | "
                     "|a|=%.2f |g|=%.0f %s",
                     sample.ax_g, sample.ay_g, sample.az_g,
                     sample.gx_dps, sample.gy_dps, sample.gz_dps,
                     sample.accel_mag_g, sample.gyro_mag_dps,
                     shake ? "[SHAKE]" : "");

            last_gyro_mag = sample.gyro_mag_dps;
            last_accel_mag = sample.accel_mag_g;
        } else {
            ESP_LOGW(TAG, "MPU6050 read failed");
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

void app_main(void)
{
    esp_chip_info_t chip_info;
    uint32_t flash_size = 0;
    i2c_master_bus_handle_t i2c_bus = NULL;
    esp_err_t err;

    esp_chip_info(&chip_info);
    esp_flash_get_size(NULL, &flash_size);

    ESP_LOGI(TAG, "heart-lantern-esp32 boot");
    ESP_LOGI(TAG, "chip: %s rev=%d flash=%" PRIu32 "MB",
             CONFIG_IDF_TARGET, chip_info.revision,
             flash_size / (1024U * 1024U));

    ESP_ERROR_CHECK(board_gpio_init());
    board_gpio_log_status();

    ESP_LOGI(TAG, "pins: LRA=IO%d LED1=IO%d LED2=IO%d SCL=IO%d SDA=IO%d",
             (int)BOARD_PIN_LRA_EN, (int)BOARD_PIN_LED_DI_1,
             (int)BOARD_PIN_LED_DI_2, (int)BOARD_PIN_I2C_SCL,
             (int)BOARD_PIN_I2C_SDA);

    err = board_i2c_init(&i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C init failed: %s", esp_err_to_name(err));
    } else {
        err = mpu6050_init(i2c_bus);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "MPU6050 init failed: %s", esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "MPU6050 WHO_AM_I=0x%02X addr=0x%02X",
                     (unsigned)mpu6050_who_am_i(),
                     (unsigned)mpu6050_i2c_addr());
            xTaskCreate(mpu6050_task, "mpu6050", 4096, NULL, 5, NULL);
        }
    }

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}
