/*
 * 难陀心灯 — Wi-Fi SNTP 墙钟（多灯摇晃调度对齐）
 */
#ifndef NANTUO_TIME_H
#define NANTUO_TIME_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 拿到 IP 后启动 SNTP（可重复调用，仅初始化一次） */
void nantuo_time_start(void);

/** SNTP 是否已完成至少一次同步 */
bool nantuo_time_is_synced(void);

/**
 * 墙钟毫秒时间戳（Unix epoch ms）
 * 未同步时返回 -1
 */
int64_t nantuo_time_wall_ms(void);

#ifdef __cplusplus
}
#endif

#endif /* NANTUO_TIME_H */
