# 板级驱动 + MPU6050 测试；heritage 源码已同步，下一阶段再链接
idf_component_register(
    SRCS "main.c"
         "board_gpio.c"
         "board_i2c.c"
         "mpu6050.c"
    INCLUDE_DIRS "." "heritage"
    REQUIRES driver spi_flash esp_driver_i2c
)
