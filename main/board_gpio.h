/*
 * 心灯板 GPIO 安全初始化与控制
 */
#ifndef BOARD_GPIO_H
#define BOARD_GPIO_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 上电安全态：
 * - LRA_EN 低电平（波形芯片关闭）
 * - 两路 LED 数据线低电平（WS2812 空闲）
 */
esp_err_t board_gpio_init(void);

/** 波形芯片使能（true=允许输出，false=关闭） */
esp_err_t board_lra_set_enable(bool enable);

/** 两路 LED 数据线拉低（熄灭/空闲） */
esp_err_t board_led_data_idle(void);

/** 打印当前关键引脚电平（调试用） */
void board_gpio_log_status(void);

#ifdef __cplusplus
}
#endif

#endif /* BOARD_GPIO_H */
