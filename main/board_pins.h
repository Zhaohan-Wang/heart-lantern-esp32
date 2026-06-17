/*
 * 心灯 ESP32 板级引脚定义（ESP32-S3-WROOM-1）
 */
#ifndef BOARD_PINS_H
#define BOARD_PINS_H

#include "driver/gpio.h"
#include "driver/i2c_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 波形发生芯片使能（LRA_EN） */
#define BOARD_PIN_LRA_EN     GPIO_NUM_41
/** WS2812 数据线 1（LED_DI_1） */
#define BOARD_PIN_LED_DI_1   GPIO_NUM_40
/** WS2812 数据线 2（LED_DI_2） */
#define BOARD_PIN_LED_DI_2   GPIO_NUM_39

/** I2C 时钟（SCL） */
#define BOARD_PIN_I2C_SCL    GPIO_NUM_35
/** I2C 数据（SDA） */
#define BOARD_PIN_I2C_SDA    GPIO_NUM_36
/** I2C 端口号 */
#define BOARD_I2C_PORT       I2C_NUM_0

#ifdef __cplusplus
}
#endif

#endif /* BOARD_PINS_H */
