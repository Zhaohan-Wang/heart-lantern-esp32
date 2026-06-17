/*
 * MPU6050 六轴 IMU（I2C）
 */
#ifndef MPU6050_H
#define MPU6050_H

#include "driver/i2c_master.h"
#include "esp_err.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 单次采样（原始值 + 物理量） */
typedef struct {
    int16_t ax_raw;
    int16_t ay_raw;
    int16_t az_raw;
    int16_t gx_raw;
    int16_t gy_raw;
    int16_t gz_raw;
    float ax_g;
    float ay_g;
    float az_g;
    float gx_dps;
    float gy_dps;
    float gz_dps;
    /** 合成加速度幅值（g） */
    float accel_mag_g;
    /** 角速度幅值（°/s） */
    float gyro_mag_dps;
} mpu6050_sample_t;

/**
 * 在已有 I2C 总线上探测并初始化 MPU6050。
 * 自动尝试 0x68 / 0x69 两个地址。
 */
esp_err_t mpu6050_init(i2c_master_bus_handle_t bus);

/** 是否已成功初始化 */
bool mpu6050_is_ready(void);

/** 读取 WHO_AM_I（未初始化返回 0） */
uint8_t mpu6050_who_am_i(void);

/** 当前设备 7-bit 地址 */
uint8_t mpu6050_i2c_addr(void);

/** 读取加速度 + 陀螺仪 */
esp_err_t mpu6050_read(mpu6050_sample_t *out);

#ifdef __cplusplus
}
#endif

#endif /* MPU6050_H */
