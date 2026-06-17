/*
 * Digital Heritage mode: Wi-Fi + BLE + 灯效模拟 OLED
 */

#ifndef DIGITAL_HERITAGE_H
#define DIGITAL_HERITAGE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Digital Heritage OLED 状态（供顶部图标 + 灯效区渲染） */
typedef struct {
    bool wifi_connected;
    bool wifi_connecting;
    bool mqtt_connected;
    bool roaming;             /**< 无 Wi-Fi 漫游：仅 BLE 邻灯检测 */
} digital_heritage_status_t;

void digital_heritage_init(void);
void digital_heritage_start_test(void);
/** 模拟陀螺仪摇晃（按键 2 触发） */
void digital_heritage_simulate_shake(void);
/** 摇晃模拟键抬起（仅 UI） */
void digital_heritage_shake_key_set(bool pressed);
void digital_heritage_get_status(digital_heritage_status_t *status);

bool app_mode_is_digital_heritage(void);
void app_mode_toggle_digital_heritage(void);

/** 是否处于无 Wi-Fi 漫游（博物馆逛馆等场景） */
bool digital_heritage_is_roaming(void);

/* UI primitives implemented in lvgl_demo_ui.c. Caller holds LVGL lock. */
void digital_heritage_ui_show(const digital_heritage_status_t *status);
void digital_heritage_ui_hide(void);
void digital_heritage_ui_set_status(const digital_heritage_status_t *status);
/** 难陀模式下高亮摇晃模拟键（README 编号 2） */
void digital_heritage_ui_set_shake_key(bool pressed);
bool ui_is_in_digital_heritage_mode(void);

#ifdef __cplusplus
}
#endif

#endif /* DIGITAL_HERITAGE_H */
