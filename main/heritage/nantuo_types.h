/*
 * 难陀心灯 — 共用类型与 BLE 广播载荷定义
 */
#ifndef NANTUO_TYPES_H
#define NANTUO_TYPES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 灯态（对应提案 listening / active / sleeping） */
typedef enum {
    NANTUO_STATE_LISTENING = 0,
    NANTUO_STATE_ACTIVE    = 1,
    NANTUO_STATE_SLEEPING  = 2,
} nantuo_lamp_state_t;

/** BLE Manufacturer Data 公司 ID（测试用 0xFFFF） */
#define NANTUO_COMPANY_ID  0xFFFF
/** 载荷魔数，用于扫描时识别心灯广播 */
#define NANTUO_ADV_MAGIC   0x4E  /* 'N' */

#define NANTUO_MAX_NEIGHBORS  8
/** 邻接判定 RSSI 阈值（约 2m 内，低于此值不入邻接表） */
#define NANTUO_RSSI_NEAR_DBM  (-65)
/** RSSI 中档阈值（约 2–5m，仅用于距离分档展示） */
#define NANTUO_RSSI_MID_DBM   (-75)
/** 传灯概率 p（提案约 0.1） */
#define NANTUO_RELAY_P_PERCENT  10
/** 邻居超时（ms），超时则从邻接表移除 */
#define NANTUO_NEIGHBOR_TIMEOUT_MS  8000
/** 对端 MQTT 上报的「它看我」RSSI 有效期 */
#define NANTUO_PEER_RSSI_TIMEOUT_MS  12000

/** 近距扫描周期（ms） */
#define NANTUO_SCAN_PERIOD_NEAR_MS   600
/** 中距扫描周期（ms） */
#define NANTUO_SCAN_PERIOD_MID_MS    1200
/** 默认扫描周期（ms） */
#define NANTUO_SCAN_PERIOD_FAR_MS    3000

/** 近距 neighbors MQTT 上报间隔（ms） */
#define NANTUO_NEIGHBOR_PUB_NEAR_MS  800
/** 默认 neighbors MQTT 上报间隔（ms） */
#define NANTUO_NEIGHBOR_PUB_FAR_MS   5000

/** 模拟摇晃触发键：第一排第二个（README 编号 2） */
#define NANTUO_SHAKE_KEY_NUM         2
/** 摇晃时本机立即点亮的亮度 */
#define NANTUO_SHAKE_SOURCE_BRIGHT   255
/** 邻灯延迟：基础 + 距离(m) × 系数（约 1m → 1s） */
#define NANTUO_SHAKE_DELAY_BASE_MS   300
#define NANTUO_SHAKE_DELAY_PER_M_MS  700
#define NANTUO_SHAKE_MAX_DELAY_MS    5000
/** 邻灯峰值亮度下限（再远也保留微弱反馈） */
#define NANTUO_SHAKE_MIN_BRIGHT      22
/** 距离 → 邻灯峰值亮度衰减尺度（越小远灯越暗） */
#define NANTUO_SHAKE_DIST_SCALE_M    0.65f
/** 摇晃本机脉冲：峰值维持（ms） */
#define NANTUO_SHAKE_PULSE_PEAK_MS   180
/** 摇晃本机/邻灯脉冲：渐灭时长（ms），合计约 1s 熄灭 */
#define NANTUO_SHAKE_PULSE_DECAY_MS  820

/** 难陀模式下待机呼吸底色（0–255，极低亮度） */
#define NANTUO_IDLE_AMBIENT_BRIGHT   10
/** 呼吸动画周期（ms），OLED 点阵与 WS2812 共用 */
#define NANTUO_BREATH_MS             2600

/** 摇晃涟漪：对某一邻灯的计划 */
typedef struct {
    char mac[12];
    float dist_m;
    uint32_t delay_ms;
    uint8_t bright;
} nantuo_shake_peer_plan_t;

/**
 * BLE 广播 Manufacturer Data（含 company_id，共 9 字节）
 * 布局: [company_id LE][magic][state][brightness][gratitude][mac_tail x3]
 */
typedef struct __attribute__((packed)) {
    uint16_t company_id;
    uint8_t magic;
    uint8_t state;
    uint8_t brightness;
    uint8_t gratitude;
    uint8_t mac_tail[3];
} nantuo_adv_mfg_t;

/** 邻接表项：附近心灯的最近一次扫描结果 */
typedef struct {
    uint8_t mac_tail[3];
    int8_t rssi;           /**< EMA 平滑 RSSI（用于距离/融合） */
    int8_t rssi_raw;       /**< 最近一次原始 RSSI */
    uint8_t state;
    uint8_t brightness;
    uint32_t last_seen_ms;
} nantuo_neighbor_t;

/** 对端经 MQTT 上报的「它看我」RSSI 缓存 */
typedef struct {
    uint8_t mac_tail[3];
    int8_t rssi;
    uint32_t last_seen_ms;
} nantuo_peer_rssi_t;

#ifdef __cplusplus
}
#endif

#endif /* NANTUO_TYPES_H */
