/*
 * 难陀心灯 — MQTT 中枢客户端（Wi-Fi Hub 编排）
 */
#ifndef NANTUO_MQTT_H
#define NANTUO_MQTT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** MQTT 连接状态变化时刷新 UI */
typedef void (*nantuo_mqtt_ui_refresh_cb_t)(void);

/**
 * 启动 MQTT 客户端（需在 Wi-Fi 拿到 IP 后调用）
 * @param mac_topic_suffix 主题后缀，如 "ee76"（无冒号）
 * @param broker_uri Broker 地址，如 mqtt://192.168.31.76:1883
 */
void nantuo_mqtt_start(const char *mac_topic_suffix,
                       const char *broker_uri,
                       nantuo_mqtt_ui_refresh_cb_t refresh_cb);

/** 停止 MQTT */
void nantuo_mqtt_stop(void);

/** 是否已连接 Broker */
bool nantuo_mqtt_is_connected(void);

/** 周期性调用：发布状态心跳 */
void nantuo_mqtt_publish_status(void);

/** 发布邻灯明细（JSON，与 status 同周期） */
void nantuo_mqtt_publish_neighbors(void);

/** 限频发布 neighbors（扫描完成等高频场景） */
void nantuo_mqtt_publish_neighbors_throttled(void);

/** 发布摇晃涟漪计划（按键模拟陀螺仪摇晃） */
void nantuo_mqtt_publish_shake(void);

#ifdef __cplusplus
}
#endif

#endif /* NANTUO_MQTT_H */
