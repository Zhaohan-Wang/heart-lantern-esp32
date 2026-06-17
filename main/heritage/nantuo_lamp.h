/*
 * 难陀心灯 — 本地涌现引擎（三态 CA + 邻接表 + 传灯）
 */
#ifndef NANTUO_LAMP_H
#define NANTUO_LAMP_H

#include "nantuo_types.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化灯态引擎 */
void nantuo_lamp_init(void);

/** 周期调用：处理超时、自动回落 */
void nantuo_lamp_tick(void);

/** 设置本机 BLE MAC 后三字节（用于广播与 MQTT 主题） */
void nantuo_lamp_set_mac_tail(const uint8_t mac_tail[3]);

/** 获取 MAC 后缀字符串，如 "ee:76" */
void nantuo_lamp_get_mac_suffix(char *buf, size_t buf_len);

/** 当前状态 / 亮度 / 仪式拍 / 是否种子灯 */
nantuo_lamp_state_t nantuo_lamp_get_state(void);
uint8_t nantuo_lamp_get_brightness(void);

/**
 * 呼吸曲线后的有效亮度（0–255）
 * @param brightness 引擎原始亮度
 * @param tick_ms 动画时间戳（ms）
 */
uint8_t nantuo_lamp_effective_brightness(uint8_t brightness, uint32_t tick_ms);

int nantuo_lamp_get_beat(void);
bool nantuo_lamp_is_seed(void);

/** Hub 仪式拍（0–7） */
void nantuo_lamp_on_hub_beat(int beat);

/** Hub 指定传灯种子（MAC 后缀，如 "ee:76"） */
void nantuo_lamp_on_relay_seed(const char *mac_suffix);

/** 扫描到邻灯广播时调用 */
void nantuo_lamp_on_adv_seen(const nantuo_adv_mfg_t *payload, int8_t rssi);

/** 扫描窗口结束，剔除过期邻居 */
void nantuo_lamp_prune_neighbors(void);

/** 邻接数量 */
int nantuo_lamp_neighbor_count(void);

/** 复制当前邻接表快照，返回有效条目数 */
int nantuo_lamp_neighbor_snapshot(nantuo_neighbor_t *out, int max_count);

/** RSSI 距离分档标签：near / mid / far */
const char *nantuo_lamp_rssi_tier(int8_t rssi);

/** 基于 RSSI 的粗略距离估算（米，仅供调试展示） */
float nantuo_lamp_rssi_dist_m(int8_t rssi);

/** 摇晃用距离：融合双向 RSSI，取较远一侧，避免 A→B 与 B→A 等待不对称 */
float nantuo_lamp_shake_dist_m(const uint8_t peer_mac_tail[3], int8_t local_rssi);

/** 邻接表最强 RSSI；无邻灯返回 -127 */
int8_t nantuo_lamp_best_neighbor_rssi(void);

/** 自适应 BLE 扫描周期（ms） */
uint32_t nantuo_lamp_scan_period_ms(void);

/** 自适应 neighbors MQTT 上报间隔（ms） */
uint32_t nantuo_lamp_neighbors_publish_interval_ms(void);

/** 融合本机与对端 MQTT 上报的 RSSI；无对端数据时返回 local */
int8_t nantuo_lamp_fused_rssi(const uint8_t peer_mac_tail[3], int8_t local_rssi);

/** 是否已有对端 RSSI 可用于融合 */
bool nantuo_lamp_has_peer_rssi(const uint8_t peer_mac_tail[3]);

/** 获取对端上报的 RSSI；无数据返回 -128 */
int8_t nantuo_lamp_get_peer_rssi(const uint8_t peer_mac_tail[3]);

/** 处理其它灯发布的 neighbors JSON（提取它对本机的 RSSI） */
void nantuo_lamp_on_peer_neighbors_json(const uint8_t reporter_mac_tail[3],
                                        const char *json);

/** 填充 OLED 三行文案（Hub / 灯态 / 邻接与 BLE 调试） */
void nantuo_lamp_format_ui(char *line1, size_t l1,
                           char *line2, size_t l2,
                           char *line3, size_t l3,
                           bool mqtt_connected,
                           bool wifi_connected,
                           bool wifi_connecting,
                           bool ble_advertising,
                           bool ble_scanning,
                           const char *ip_short);

/** 构建待写入 BLE 广播的 Manufacturer Data */
void nantuo_lamp_fill_adv_payload(nantuo_adv_mfg_t *payload);

/** 距离 → 邻灯涟漪延迟（ms） */
uint32_t nantuo_lamp_shake_delay_ms(float dist_m);

/** 距离 → 邻灯目标亮度（0–255） */
uint8_t nantuo_lamp_shake_brightness(float dist_m);

/**
 * 计算摇晃涟漪计划（基于当前邻接表 + 融合 RSSI）
 * @return 有效 peer 数量
 */
int nantuo_lamp_build_shake_plan(nantuo_shake_peer_plan_t *out, int max_count);

/** 本机触发摇晃：立即点亮并生成邻灯计划 */
int nantuo_lamp_on_shake(void);

/** 收到 MQTT 摇晃涟漪：按剩余延迟调度点亮 */
void nantuo_lamp_on_shake_incoming(uint32_t delay_ms, uint8_t brightness);

/** 亮度脉冲是否进行中（摇晃/邻灯涟漪） */
bool nantuo_lamp_bright_pulse_active(void);

/** 是否有待触发的邻灯涟漪 */
bool nantuo_lamp_shake_ripple_pending(void);

/** 摇晃相关效果是否未结束（脉冲或待触发涟漪） */
bool nantuo_lamp_shake_effects_active(void);

#ifdef __cplusplus
}
#endif

#endif /* NANTUO_LAMP_H */
