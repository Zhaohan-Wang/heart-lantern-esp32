/*
 * MPU6050 六轴 IMU 驱动（I2C）
 */

#include "mpu6050.h"

#include "esp_check.h"
#include "esp_log.h"

#include <math.h>

static const char *TAG = "mpu6050";

/** WHO_AM_I 寄存器 */
#define MPU6050_REG_WHO_AM_I     0x75U
/** 电源管理 1 */
#define MPU6050_REG_PWR_MGMT_1     0x6BU
/** 加速度输出起始寄存器 */
#define MPU6050_REG_ACCEL_XOUT_H   0x3BU
/** 期望 WHO_AM_I 值 */
#define MPU6050_WHO_AM_I_VALUE     0x68U

/** 默认量程换算：±2g / ±250°/s */
#define MPU6050_ACCEL_LSB_PER_G    16384.0f
#define MPU6050_GYRO_LSB_PER_DPS   131.0f

static i2c_master_dev_handle_t s_dev;
static bool s_ready;
static uint8_t s_addr;

static esp_err_t mpu6050_reg_read(i2c_master_dev_handle_t dev,
                                    uint8_t reg,
                                    uint8_t *data,
                                    size_t len)
{
    return i2c_master_transmit_receive(dev, &reg, 1, data, len, 1000);
}

static esp_err_t mpu6050_write_u8(uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = {reg, value};

    return i2c_master_transmit(s_dev, buf, sizeof(buf), 1000);
}

static esp_err_t mpu6050_probe_addr(i2c_master_bus_handle_t bus, uint8_t addr)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 100000,
    };
    i2c_master_dev_handle_t dev = NULL;
    uint8_t who = 0;
    esp_err_t err;

    err = i2c_master_bus_add_device(bus, &dev_cfg, &dev);
    if (err != ESP_OK) {
        return err;
    }

    err = mpu6050_reg_read(dev, MPU6050_REG_WHO_AM_I, &who, 1);
    if (err == ESP_OK && who == MPU6050_WHO_AM_I_VALUE) {
        s_dev = dev;
        s_addr = addr;
        return ESP_OK;
    }

    i2c_master_bus_rm_device(dev);
    return ESP_ERR_NOT_FOUND;
}

static int16_t mpu6050_combine_msb_lsb(uint8_t msb, uint8_t lsb)
{
    return (int16_t)((uint16_t)msb << 8 | lsb);
}

esp_err_t mpu6050_init(i2c_master_bus_handle_t bus)
{
    static const uint8_t candidates[] = {0x68U, 0x69U};
    esp_err_t err = ESP_ERR_NOT_FOUND;

    if (s_ready) {
        return ESP_OK;
    }

    for (size_t i = 0; i < sizeof(candidates); i++) {
        err = mpu6050_probe_addr(bus, candidates[i]);
        if (err == ESP_OK) {
            break;
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MPU6050 not found on I2C (tried 0x68/0x69)");
        return ESP_ERR_NOT_FOUND;
    }

    /* 退出睡眠，使用内部时钟 */
    ESP_RETURN_ON_ERROR(mpu6050_write_u8(MPU6050_REG_PWR_MGMT_1, 0x00), TAG, "wake");

    s_ready = true;
    ESP_LOGI(TAG, "MPU6050 ready, addr=0x%02X WHO_AM_I=0x%02X",
             (unsigned)s_addr, (unsigned)MPU6050_WHO_AM_I_VALUE);
    return ESP_OK;
}

bool mpu6050_is_ready(void)
{
    return s_ready;
}

uint8_t mpu6050_who_am_i(void)
{
    uint8_t who = 0;

    if (!s_ready) {
        return 0;
    }
    if (mpu6050_reg_read(s_dev, MPU6050_REG_WHO_AM_I, &who, 1) != ESP_OK) {
        return 0;
    }
    return who;
}

uint8_t mpu6050_i2c_addr(void)
{
    return s_addr;
}

esp_err_t mpu6050_read(mpu6050_sample_t *out)
{
    uint8_t raw[14];
    esp_err_t err;

    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    err = mpu6050_reg_read(s_dev, MPU6050_REG_ACCEL_XOUT_H, raw, sizeof(raw));
    if (err != ESP_OK) {
        return err;
    }

    out->ax_raw = mpu6050_combine_msb_lsb(raw[0], raw[1]);
    out->ay_raw = mpu6050_combine_msb_lsb(raw[2], raw[3]);
    out->az_raw = mpu6050_combine_msb_lsb(raw[4], raw[5]);
    out->gx_raw = mpu6050_combine_msb_lsb(raw[8], raw[9]);
    out->gy_raw = mpu6050_combine_msb_lsb(raw[10], raw[11]);
    out->gz_raw = mpu6050_combine_msb_lsb(raw[12], raw[13]);

    out->ax_g = (float)out->ax_raw / MPU6050_ACCEL_LSB_PER_G;
    out->ay_g = (float)out->ay_raw / MPU6050_ACCEL_LSB_PER_G;
    out->az_g = (float)out->az_raw / MPU6050_ACCEL_LSB_PER_G;
    out->gx_dps = (float)out->gx_raw / MPU6050_GYRO_LSB_PER_DPS;
    out->gy_dps = (float)out->gy_raw / MPU6050_GYRO_LSB_PER_DPS;
    out->gz_dps = (float)out->gz_raw / MPU6050_GYRO_LSB_PER_DPS;

    out->accel_mag_g = sqrtf(out->ax_g * out->ax_g +
                             out->ay_g * out->ay_g +
                             out->az_g * out->az_g);
    out->gyro_mag_dps = sqrtf(out->gx_dps * out->gx_dps +
                               out->gy_dps * out->gy_dps +
                               out->gz_dps * out->gz_dps);
    return ESP_OK;
}
