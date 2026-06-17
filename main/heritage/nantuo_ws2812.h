/*
 * 难陀心灯 — WS2812B 灯条驱动（RMT）
 */
#ifndef NANTUO_WS2812_H
#define NANTUO_WS2812_H

#include "esp_err.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /** 呼吸最亮时的颜色 */
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    /** 呼吸最暗时的颜色 */
    uint8_t dark_red;
    uint8_t dark_green;
    uint8_t dark_blue;
    float intensity;      /**< 默认呼吸峰值强度，0.0~1.0 */
    float dark_intensity; /**< 默认呼吸谷值强度，0.0~1.0 */
    uint32_t period_ms;   /**< 默认呼吸周期，毫秒 */
} nantuo_ws2812_breath_config_t;

/** 启动灯条硬件与渲染任务（仅难陀模式下亮灯）；可重复调用（仅初始化一次） */
esp_err_t nantuo_ws2812_start(void);

void nantuo_ws2812_get_breath_config(nantuo_ws2812_breath_config_t *config);
void nantuo_ws2812_set_breath_config(const nantuo_ws2812_breath_config_t *config);
void nantuo_ws2812_reset_breath_config(void);

/**
 * 处理前端/MQTT 灯效命令。
 * 支持：
 *   led_reset
 *   led=color=#ff2200,intensity=0.08,period=6400
 *   led=bright_color=#ff2200,dark_color=#120020,intensity=0.08,dark_intensity=0.02,period=6400
 *   {"led":{"color":"#ff2200","intensity":0.08,"period":6400}}
 *   {"led":{"bright_color":"#ff2200","dark_color":"#120020","intensity":0.08,"dark_intensity":0.02,"period":6400}}
 */
bool nantuo_ws2812_handle_command(const char *command);

#ifdef __cplusplus
}
#endif

#endif /* NANTUO_WS2812_H */
