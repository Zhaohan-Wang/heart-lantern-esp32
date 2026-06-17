/*
 * 心灯板 GPIO 安全初始化与控制
 */

#include "board_gpio.h"
#include "board_pins.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "board_gpio";

/** 板级输出引脚掩码 */
#define BOARD_OUTPUT_PIN_MASK \
    ((1ULL << BOARD_PIN_LRA_EN) | (1ULL << BOARD_PIN_LED_DI_1) | (1ULL << BOARD_PIN_LED_DI_2))

/** LRA 使能有效电平：高电平开启（按常见 EN 脚命名；上电默认拉低） */
#define BOARD_LRA_ENABLE_LEVEL   1
#define BOARD_LRA_DISABLE_LEVEL  0

esp_err_t board_gpio_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = BOARD_OUTPUT_PIN_MASK,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "gpio_config failed");

    /* 波形芯片默认关闭，避免上电误触发 */
    gpio_set_level(BOARD_PIN_LRA_EN, BOARD_LRA_DISABLE_LEVEL);

    /* WS2812 空闲态：数据线保持低电平 */
    gpio_set_level(BOARD_PIN_LED_DI_1, 0);
    gpio_set_level(BOARD_PIN_LED_DI_2, 0);

    /*
     * 数据线驱动能力降到最低，减轻未串限流电阻时的过冲/振铃。
     * 后续 led_strip 接管引脚后仍建议保留该设置。
     */
    gpio_set_drive_capability(BOARD_PIN_LED_DI_1, GPIO_DRIVE_CAP_0);
    gpio_set_drive_capability(BOARD_PIN_LED_DI_2, GPIO_DRIVE_CAP_0);

    ESP_LOGI(TAG, "safe defaults: LRA_EN=LOW, LED_DI_1/2=LOW");
    return ESP_OK;
}

esp_err_t board_lra_set_enable(bool enable)
{
    int level = enable ? BOARD_LRA_ENABLE_LEVEL : BOARD_LRA_DISABLE_LEVEL;

    gpio_set_level(BOARD_PIN_LRA_EN, level);
    ESP_LOGI(TAG, "LRA_EN -> %s", enable ? "ON" : "OFF");
    return ESP_OK;
}

esp_err_t board_led_data_idle(void)
{
    gpio_set_level(BOARD_PIN_LED_DI_1, 0);
    gpio_set_level(BOARD_PIN_LED_DI_2, 0);
    return ESP_OK;
}

void board_gpio_log_status(void)
{
    ESP_LOGI(TAG, "GPIO status: LRA_EN(IO%d)=%d LED_DI_1(IO%d)=%d LED_DI_2(IO%d)=%d",
             (int)BOARD_PIN_LRA_EN, gpio_get_level(BOARD_PIN_LRA_EN),
             (int)BOARD_PIN_LED_DI_1, gpio_get_level(BOARD_PIN_LED_DI_1),
             (int)BOARD_PIN_LED_DI_2, gpio_get_level(BOARD_PIN_LED_DI_2));
}
