/*
 * 心灯板 I2C 总线初始化
 */

#include "board_i2c.h"
#include "board_pins.h"

#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "board_i2c";

static i2c_master_bus_handle_t s_bus;
static bool s_ready;

esp_err_t board_i2c_init(i2c_master_bus_handle_t *out_bus)
{
    i2c_master_bus_config_t bus_cfg;

    if (s_ready) {
        if (out_bus != NULL) {
            *out_bus = s_bus;
        }
        return ESP_OK;
    }

    bus_cfg = (i2c_master_bus_config_t){
        .i2c_port = BOARD_I2C_PORT,
        .sda_io_num = BOARD_PIN_I2C_SDA,
        .scl_io_num = BOARD_PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = {
            .enable_internal_pullup = 1,
        },
    };

    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_bus), TAG, "i2c_new_master_bus");
    s_ready = true;

    ESP_LOGI(TAG, "I2C ready: SCL=IO%d SDA=IO%d port=%d",
             (int)BOARD_PIN_I2C_SCL, (int)BOARD_PIN_I2C_SDA, (int)BOARD_I2C_PORT);

    if (out_bus != NULL) {
        *out_bus = s_bus;
    }
    return ESP_OK;
}
