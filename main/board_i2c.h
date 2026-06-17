/*
 * 心灯板 I2C 总线初始化
 */
#ifndef BOARD_I2C_H
#define BOARD_I2C_H

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化 I2C 主机总线，返回 bus 句柄 */
esp_err_t board_i2c_init(i2c_master_bus_handle_t *out_bus);

#ifdef __cplusplus
}
#endif

#endif /* BOARD_I2C_H */
