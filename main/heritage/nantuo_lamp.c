/*
 * 难陀心灯 — 本地涌现引擎（三态 CA + 邻接表 + 传灯）
 */

#include "nantuo_lamp.h"

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <math.h>

static const char *TAG = "nantuo_lamp";

/** active 态保持时长（ms），之后进入 sleeping */
#define NANTUO_ACTIVE_HOLD_MS  30000

static uint8_t s_mac_tail[3];
static nantuo_lamp_state_t s_state = NANTUO_STATE_LISTENING;
static uint8_t s_brightness;
static uint8_t s_gratitude;
static int s_beat;
static bool s_is_seed;
static int64_t s_active_until_us;
static nantuo_neighbor_t s_neighbors[NANTUO_MAX_NEIGHBORS];
static nantuo_peer_rssi_t s_peer_rssi[NANTUO_MAX_NEIGHBORS];

/** 摇晃涟漪：MQTT 收到的延迟点亮 */
static struct {
    bool pending;
    int64_t due_ms;
    uint8_t brightness;
} s_shake_ripple;

/** 按键/涟漪触发的短时亮度脉冲（不进入 30s ACTIVE） */
static struct {
    bool active;
    int64_t start_ms;
    uint8_t peak;
    uint8_t return_bright;
} s_bright_pulse;

/** RSSI EMA：近距更大 alpha，响应更快 */
static float rssi_ema_alpha(int8_t rssi)
{
    if (rssi >= -55) {
        return 0.55f;
    }
    if (rssi >= -65) {
        return 0.38f;
    }
    return 0.25f;
}

/** 更新 EMA 平滑 RSSI */
static int8_t rssi_apply_ema(int8_t prev_smooth, int8_t raw, bool is_new)
{
    float alpha;
    float next;

    if (is_new) {
        return raw;
    }
    alpha = rssi_ema_alpha(raw);
    next = alpha * (float)raw + (1.0f - alpha) * (float)prev_smooth;
    return (int8_t)(next + (next >= 0 ? 0.5f : -0.5f));
}

static int peer_rssi_slot_for(const uint8_t mac_tail[3])
{
    int free_slot = -1;

    for (int i = 0; i < NANTUO_MAX_NEIGHBORS; i++) {
        if (memcmp(s_peer_rssi[i].mac_tail, mac_tail, 3) == 0) {
            return i;
        }
        if (free_slot < 0 && s_peer_rssi[i].last_seen_ms == 0) {
            free_slot = i;
        }
    }
    return free_slot;
}

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

/** 停止亮度脉冲（Hub 仪式 / 传灯接管时） */
static void bright_pulse_cancel(void)
{
    s_bright_pulse.active = false;
}

/** 摇晃闪回的目标亮度：仪式 ACT 保留底色，其余回到全灭 */
static uint8_t brightness_after_shake_pulse(void)
{
    if (s_bright_pulse.active) {
        return s_bright_pulse.return_bright;
    }
    if (s_state == NANTUO_STATE_ACTIVE && s_brightness > 40) {
        return s_brightness;
    }
    return 0;
}

/** 启动短时亮脉冲：峰值后快速回到触发前底色（不进入 30s ACTIVE） */
static void bright_pulse_start(uint8_t peak)
{
    s_bright_pulse.active = true;
    s_bright_pulse.start_ms = now_ms();
    s_bright_pulse.peak = peak;
    s_bright_pulse.return_bright = brightness_after_shake_pulse();
    s_brightness = peak;
    s_gratitude = 255;
}

/** 按 elapsed 更新脉冲亮度与 gratitude 衰减 */
static void bright_pulse_tick(void)
{
    int64_t elapsed;
    float factor;
    int level;

    if (!s_bright_pulse.active) {
        return;
    }

    elapsed = now_ms() - s_bright_pulse.start_ms;
    if (elapsed < (int64_t)NANTUO_SHAKE_PULSE_PEAK_MS) {
        s_brightness = s_bright_pulse.peak;
        s_gratitude = 255;
        return;
    }

    if (elapsed < (int64_t)(NANTUO_SHAKE_PULSE_PEAK_MS + NANTUO_SHAKE_PULSE_DECAY_MS)) {
        elapsed -= (int64_t)NANTUO_SHAKE_PULSE_PEAK_MS;
        /* 线性渐暗：全程可见亮度下降，比平方曲线更易察觉 */
        factor = 1.0f - (float)elapsed / (float)NANTUO_SHAKE_PULSE_DECAY_MS;
        if (factor < 0.0f) {
            factor = 0.0f;
        }
        level = (int)((float)s_bright_pulse.return_bright +
                      ((float)s_bright_pulse.peak -
                       (float)s_bright_pulse.return_bright) * factor);
        if (level < 0) {
            level = 0;
        }
        if (level > 255) {
            level = 255;
        }
        s_brightness = (uint8_t)level;
        s_gratitude = (uint8_t)(255.0f * factor);
        return;
    }

    s_bright_pulse.active = false;
    s_brightness = s_bright_pulse.return_bright;
    s_gratitude = 0;
    ESP_LOGI(TAG, "shake pulse done -> bright=%u", (unsigned)s_brightness);
}

static void lamp_set_state(nantuo_lamp_state_t state, uint8_t brightness)
{
    bright_pulse_cancel();
    s_state = state;
    s_brightness = brightness;
    if (state == NANTUO_STATE_ACTIVE) {
        s_active_until_us = esp_timer_get_time() + (int64_t)NANTUO_ACTIVE_HOLD_MS * 1000;
    }
    ESP_LOGI(TAG, "state=%d bright=%u seed=%d beat=%d",
             (int)state, (unsigned)brightness, (int)s_is_seed, s_beat);
}

static void lamp_activate(uint8_t brightness)
{
    s_is_seed = false;
    lamp_set_state(NANTUO_STATE_ACTIVE, brightness);
}

static int neighbor_slot_for(const uint8_t mac_tail[3])
{
    int free_slot = -1;

    for (int i = 0; i < NANTUO_MAX_NEIGHBORS; i++) {
        if (memcmp(s_neighbors[i].mac_tail, mac_tail, 3) == 0) {
            return i;
        }
        if (free_slot < 0 && s_neighbors[i].last_seen_ms == 0) {
            free_slot = i;
        }
    }
    return free_slot;
}

static void try_relay_from_neighbor(const nantuo_neighbor_t *neighbor)
{
    if (neighbor->state != NANTUO_STATE_ACTIVE) {
        return;
    }
    if (s_state != NANTUO_STATE_LISTENING) {
        return;
    }
    if ((esp_random() % 100) >= NANTUO_RELAY_P_PERCENT) {
        return;
    }
    uint8_t inherited = neighbor->brightness;
    if (inherited < 80) {
        inherited = 80;
    }
    ESP_LOGI(TAG, "relay from %02x:%02x:%02x rssi=%d",
             neighbor->mac_tail[0], neighbor->mac_tail[1], neighbor->mac_tail[2],
             (int)neighbor->rssi);
    lamp_activate(inherited);
}

void nantuo_lamp_init(void)
{
    memset(s_mac_tail, 0, sizeof(s_mac_tail));
    memset(s_neighbors, 0, sizeof(s_neighbors));
    memset(s_peer_rssi, 0, sizeof(s_peer_rssi));
    s_gratitude = 0;
    s_beat = -1;
    s_is_seed = false;
    memset(&s_shake_ripple, 0, sizeof(s_shake_ripple));
    memset(&s_bright_pulse, 0, sizeof(s_bright_pulse));
    lamp_set_state(NANTUO_STATE_LISTENING, 0);
}

void nantuo_lamp_tick(void)
{
    int64_t now = now_ms();

    if (s_shake_ripple.pending && now >= s_shake_ripple.due_ms) {
        s_shake_ripple.pending = false;
        ESP_LOGI(TAG, "shake ripple fire bright=%u",
                 (unsigned)s_shake_ripple.brightness);
        bright_pulse_start(s_shake_ripple.brightness);
    }

    bright_pulse_tick();

    if (s_state == NANTUO_STATE_ACTIVE &&
        esp_timer_get_time() >= s_active_until_us) {
        lamp_set_state(NANTUO_STATE_SLEEPING, 20);
        s_gratitude = 0;
    }
}

void nantuo_lamp_set_mac_tail(const uint8_t mac_tail[3])
{
    if (mac_tail) {
        memcpy(s_mac_tail, mac_tail, 3);
    }
}

void nantuo_lamp_get_mac_suffix(char *buf, size_t buf_len)
{
    if (buf == NULL || buf_len == 0) {
        return;
    }
    snprintf(buf, buf_len, "%02x:%02x", s_mac_tail[1], s_mac_tail[2]);
}

nantuo_lamp_state_t nantuo_lamp_get_state(void)
{
    return s_state;
}

uint8_t nantuo_lamp_get_brightness(void)
{
    return s_brightness;
}

uint8_t nantuo_lamp_effective_brightness(uint8_t brightness, uint32_t tick_ms)
{
    float phase;
    float breath;
    int level;

    if (brightness == 0) {
        return 0;
    }
    /* 摇晃脉冲回落中不做呼吸，避免渐暗被干扰 */
    if (s_bright_pulse.active) {
        return brightness;
    }
    if (brightness >= 240) {
        return 255;
    }

    phase = (float)(tick_ms % NANTUO_BREATH_MS) /
            (float)NANTUO_BREATH_MS * 6.2831853f;
    breath = 0.42f + 0.58f * (sinf(phase) * 0.5f + 0.5f);
    level = (int)((float)brightness * breath);
    if (level < 0) {
        level = 0;
    }
    if (level > 255) {
        level = 255;
    }
    return (uint8_t)level;
}

int nantuo_lamp_get_beat(void)
{
    return s_beat;
}

bool nantuo_lamp_is_seed(void)
{
    return s_is_seed;
}

void nantuo_lamp_on_hub_beat(int beat)
{
    s_beat = beat;
    ESP_LOGI(TAG, "hub beat=%d", beat);

    switch (beat) {
    case 0:
        s_is_seed = false;
        lamp_set_state(NANTUO_STATE_LISTENING, 30);
        break;
    case 2:
        lamp_set_state(NANTUO_STATE_LISTENING, 60);
        break;
    case 4:
        lamp_set_state(NANTUO_STATE_ACTIVE, 160);
        break;
    case 6:
        /* 非种子灯等待邻接传灯；种子由 relay_seed 指令点亮 */
        if (!s_is_seed && s_state == NANTUO_STATE_LISTENING) {
            lamp_set_state(NANTUO_STATE_LISTENING, 40);
        }
        break;
    case 7:
        s_is_seed = false;
        lamp_set_state(NANTUO_STATE_SLEEPING, 25);
        break;
    default:
        break;
    }
}

void nantuo_lamp_on_relay_seed(const char *mac_suffix)
{
    char mine[8];

    if (mac_suffix == NULL) {
        return;
    }
    nantuo_lamp_get_mac_suffix(mine, sizeof(mine));
    if (strcasecmp(mac_suffix, mine) == 0) {
        s_is_seed = true;
        lamp_set_state(NANTUO_STATE_ACTIVE, 255);
        ESP_LOGI(TAG, "I am relay seed");
    }
}

void nantuo_lamp_on_adv_seen(const nantuo_adv_mfg_t *payload, int8_t rssi)
{
    int slot;
    nantuo_neighbor_t snapshot;
    bool is_new;

    if (payload == NULL || payload->magic != NANTUO_ADV_MAGIC) {
        return;
    }
    if (rssi < NANTUO_RSSI_NEAR_DBM) {
        ESP_LOGD(TAG, "peer rssi=%d below near threshold %d",
                 (int)rssi, (int)NANTUO_RSSI_NEAR_DBM);
        return;
    }
    if (memcmp(payload->mac_tail, s_mac_tail, 3) == 0) {
        return;
    }

    slot = neighbor_slot_for(payload->mac_tail);
    if (slot < 0) {
        ESP_LOGW(TAG, "neighbor table full");
        return;
    }

    is_new = (s_neighbors[slot].last_seen_ms == 0);
    memcpy(s_neighbors[slot].mac_tail, payload->mac_tail, 3);
    s_neighbors[slot].rssi_raw = rssi;
    s_neighbors[slot].rssi = rssi_apply_ema(s_neighbors[slot].rssi, rssi, is_new);
    s_neighbors[slot].state = payload->state;
    s_neighbors[slot].brightness = payload->brightness;
    s_neighbors[slot].last_seen_ms = (uint32_t)now_ms();

    if (is_new) {
        int8_t fused = nantuo_lamp_fused_rssi(payload->mac_tail, s_neighbors[slot].rssi);
        ESP_LOGI(TAG, "neighbor NEW %02x:%02x rssi=%d fused=%d dist~%.1fm",
                 payload->mac_tail[1], payload->mac_tail[2],
                 (int)s_neighbors[slot].rssi, (int)fused,
                 (double)nantuo_lamp_rssi_dist_m(fused));
    }

    snapshot = s_neighbors[slot];
    try_relay_from_neighbor(&snapshot);
}

void nantuo_lamp_prune_neighbors(void)
{
    uint32_t now = (uint32_t)now_ms();

    for (int i = 0; i < NANTUO_MAX_NEIGHBORS; i++) {
        if (s_neighbors[i].last_seen_ms == 0) {
            continue;
        }
        if ((now - s_neighbors[i].last_seen_ms) > NANTUO_NEIGHBOR_TIMEOUT_MS) {
            memset(&s_neighbors[i], 0, sizeof(s_neighbors[i]));
        }
    }
}

int nantuo_lamp_neighbor_count(void)
{
    int count = 0;

    for (int i = 0; i < NANTUO_MAX_NEIGHBORS; i++) {
        if (s_neighbors[i].last_seen_ms != 0) {
            count++;
        }
    }
    return count;
}

int nantuo_lamp_neighbor_snapshot(nantuo_neighbor_t *out, int max_count)
{
    int n = 0;

    if (out == NULL || max_count <= 0) {
        return 0;
    }
    for (int i = 0; i < NANTUO_MAX_NEIGHBORS && n < max_count; i++) {
        if (s_neighbors[i].last_seen_ms != 0) {
            out[n++] = s_neighbors[i];
        }
    }
    return n;
}

const char *nantuo_lamp_rssi_tier(int8_t rssi)
{
    if (rssi >= NANTUO_RSSI_NEAR_DBM) {
        return "near";
    }
    if (rssi >= NANTUO_RSSI_MID_DBM) {
        return "mid";
    }
    return "far";
}

float nantuo_lamp_rssi_dist_m(int8_t rssi)
{
    /* BLE 路径损耗：TxPower@1m=-59dBm，n=2 → d=10^((-59-RSSI)/20) */
    float exponent = ((float)(-59 - rssi)) / 20.0f;

    if (exponent > 6.0f) {
        return 99.9f;
    }
    if (exponent < -2.0f) {
        return 0.3f;
    }
    return powf(10.0f, exponent);
}

int8_t nantuo_lamp_best_neighbor_rssi(void)
{
    int8_t best = -127;

    for (int i = 0; i < NANTUO_MAX_NEIGHBORS; i++) {
        if (s_neighbors[i].last_seen_ms == 0) {
            continue;
        }
        if (s_neighbors[i].rssi > best) {
            best = s_neighbors[i].rssi;
        }
    }
    return best;
}

float nantuo_lamp_shake_dist_m(const uint8_t peer_mac_tail[3], int8_t local_rssi)
{
    int8_t fused;
    int8_t peer_rssi;
    float dist_local;
    float dist_peer;

    fused = nantuo_lamp_fused_rssi(peer_mac_tail, local_rssi);
    dist_local = nantuo_lamp_rssi_dist_m(fused);
    peer_rssi = nantuo_lamp_get_peer_rssi(peer_mac_tail);
    if (peer_rssi <= -127) {
        return dist_local;
    }
    dist_peer = nantuo_lamp_rssi_dist_m(peer_rssi);
    /* 较弱信号对应更远距离，双向取 max 使两灯算出的等待更一致 */
    if (dist_peer > dist_local) {
        return dist_peer;
    }
    return dist_local;
}

uint32_t nantuo_lamp_shake_delay_ms(float dist_m)
{
    uint32_t delay;

    if (dist_m < 0.3f) {
        dist_m = 0.3f;
    }
    delay = NANTUO_SHAKE_DELAY_BASE_MS +
            (uint32_t)(dist_m * (float)NANTUO_SHAKE_DELAY_PER_M_MS);
    if (delay > NANTUO_SHAKE_MAX_DELAY_MS) {
        delay = NANTUO_SHAKE_MAX_DELAY_MS;
    }
    return delay;
}

uint8_t nantuo_lamp_shake_brightness(float dist_m)
{
    float factor;
    int level;

    if (dist_m < 0.3f) {
        dist_m = 0.3f;
    }
    /* 指数衰减：距离越远峰值越低（非渐灭时长） */
    factor = expf(-dist_m / NANTUO_SHAKE_DIST_SCALE_M);
    level = NANTUO_SHAKE_MIN_BRIGHT +
            (int)(factor * (float)(NANTUO_SHAKE_SOURCE_BRIGHT - NANTUO_SHAKE_MIN_BRIGHT));
    if (level > (int)NANTUO_SHAKE_SOURCE_BRIGHT) {
        level = (int)NANTUO_SHAKE_SOURCE_BRIGHT;
    }
    if (level < (int)NANTUO_SHAKE_MIN_BRIGHT) {
        level = (int)NANTUO_SHAKE_MIN_BRIGHT;
    }
    return (uint8_t)level;
}

int nantuo_lamp_build_shake_plan(nantuo_shake_peer_plan_t *out, int max_count)
{
    nantuo_neighbor_t peers[NANTUO_MAX_NEIGHBORS];
    int count;
    int n = 0;

    if (out == NULL || max_count <= 0) {
        return 0;
    }

    count = nantuo_lamp_neighbor_snapshot(peers, NANTUO_MAX_NEIGHBORS);
    for (int i = 0; i < count && n < max_count; i++) {
        float dist_m;

        dist_m = nantuo_lamp_shake_dist_m(peers[i].mac_tail, peers[i].rssi);
        snprintf(out[n].mac, sizeof(out[n].mac), "%02x:%02x",
                 peers[i].mac_tail[1], peers[i].mac_tail[2]);
        out[n].dist_m = dist_m;
        out[n].delay_ms = nantuo_lamp_shake_delay_ms(dist_m);
        out[n].bright = nantuo_lamp_shake_brightness(dist_m);
        ESP_LOGI(TAG, "shake plan peer %s dist=%.1fm delay=%ums bright=%u",
                 out[n].mac, (double)dist_m,
                 (unsigned)out[n].delay_ms, (unsigned)out[n].bright);
        n++;
    }
    return n;
}

int nantuo_lamp_on_shake(void)
{
    nantuo_shake_peer_plan_t plans[NANTUO_MAX_NEIGHBORS];
    int peer_count;

    bright_pulse_start(NANTUO_SHAKE_SOURCE_BRIGHT);
    peer_count = nantuo_lamp_build_shake_plan(plans, NANTUO_MAX_NEIGHBORS);
    ESP_LOGI(TAG, "shake source ON, peers=%d", peer_count);
    return peer_count;
}

void nantuo_lamp_on_shake_incoming(uint32_t delay_ms, uint8_t brightness)
{
    if (brightness < NANTUO_SHAKE_MIN_BRIGHT) {
        brightness = NANTUO_SHAKE_MIN_BRIGHT;
    }
    s_shake_ripple.pending = true;
    s_shake_ripple.due_ms = now_ms() + (int64_t)delay_ms;
    s_shake_ripple.brightness = brightness;
    ESP_LOGI(TAG, "shake incoming delay=%ums bright=%u",
             (unsigned)delay_ms, (unsigned)brightness);
}

bool nantuo_lamp_bright_pulse_active(void)
{
    return s_bright_pulse.active;
}

bool nantuo_lamp_shake_ripple_pending(void)
{
    return s_shake_ripple.pending;
}

bool nantuo_lamp_shake_effects_active(void)
{
    return s_bright_pulse.active || s_shake_ripple.pending;
}

uint32_t nantuo_lamp_scan_period_ms(void)
{
    int8_t best = nantuo_lamp_best_neighbor_rssi();

    if (best >= -55) {
        return NANTUO_SCAN_PERIOD_NEAR_MS;
    }
    if (best >= -62) {
        return NANTUO_SCAN_PERIOD_MID_MS;
    }
    if (best >= NANTUO_RSSI_NEAR_DBM) {
        return NANTUO_SCAN_PERIOD_MID_MS;
    }
    return NANTUO_SCAN_PERIOD_FAR_MS;
}

uint32_t nantuo_lamp_neighbors_publish_interval_ms(void)
{
    int8_t best = nantuo_lamp_best_neighbor_rssi();

    if (best >= -62) {
        return NANTUO_NEIGHBOR_PUB_NEAR_MS;
    }
    if (best >= NANTUO_RSSI_NEAR_DBM) {
        return 2000;
    }
    return NANTUO_NEIGHBOR_PUB_FAR_MS;
}

bool nantuo_lamp_has_peer_rssi(const uint8_t peer_mac_tail[3])
{
    uint32_t now = (uint32_t)now_ms();

    for (int i = 0; i < NANTUO_MAX_NEIGHBORS; i++) {
        if (s_peer_rssi[i].last_seen_ms == 0) {
            continue;
        }
        if (memcmp(s_peer_rssi[i].mac_tail, peer_mac_tail, 3) != 0) {
            continue;
        }
        return (now - s_peer_rssi[i].last_seen_ms) <= NANTUO_PEER_RSSI_TIMEOUT_MS;
    }
    return false;
}

int8_t nantuo_lamp_fused_rssi(const uint8_t peer_mac_tail[3], int8_t local_rssi)
{
    uint32_t now = (uint32_t)now_ms();

    for (int i = 0; i < NANTUO_MAX_NEIGHBORS; i++) {
        if (s_peer_rssi[i].last_seen_ms == 0) {
            continue;
        }
        if (memcmp(s_peer_rssi[i].mac_tail, peer_mac_tail, 3) != 0) {
            continue;
        }
        if ((now - s_peer_rssi[i].last_seen_ms) > NANTUO_PEER_RSSI_TIMEOUT_MS) {
            return local_rssi;
        }
        return (int8_t)(((int)local_rssi + (int)s_peer_rssi[i].rssi + 1) / 2);
    }
    return local_rssi;
}

int8_t nantuo_lamp_get_peer_rssi(const uint8_t peer_mac_tail[3])
{
    uint32_t now = (uint32_t)now_ms();

    for (int i = 0; i < NANTUO_MAX_NEIGHBORS; i++) {
        if (s_peer_rssi[i].last_seen_ms == 0) {
            continue;
        }
        if (memcmp(s_peer_rssi[i].mac_tail, peer_mac_tail, 3) != 0) {
            continue;
        }
        if ((now - s_peer_rssi[i].last_seen_ms) > NANTUO_PEER_RSSI_TIMEOUT_MS) {
            return -128;
        }
        return s_peer_rssi[i].rssi;
    }
    return -128;
}

void nantuo_lamp_on_peer_neighbors_json(const uint8_t reporter_mac_tail[3],
                                          const char *json)
{
    char my_mac[12];
    char pattern[20];
    const char *found;
    int rssi;
    int slot;

    if (json == NULL || reporter_mac_tail == NULL) {
        return;
    }
    if (memcmp(reporter_mac_tail, s_mac_tail, 3) == 0) {
        return;
    }

    nantuo_lamp_get_mac_suffix(my_mac, sizeof(my_mac));
    snprintf(pattern, sizeof(pattern), "\"mac\":\"%s\"", my_mac);
    found = strstr(json, pattern);
    if (found == NULL) {
        return;
    }

    found = strstr(found, "\"rssi_local\":");
    if (found == NULL) {
        found = strstr(json, pattern);
        if (found != NULL) {
            found = strstr(found, "\"rssi\":");
        }
    }
    if (found == NULL) {
        return;
    }
    if (sscanf(found, "\"rssi_local\":%d", &rssi) != 1 &&
        sscanf(found, "\"rssi\":%d", &rssi) != 1) {
        return;
    }

    slot = peer_rssi_slot_for(reporter_mac_tail);
    if (slot < 0) {
        return;
    }

    memcpy(s_peer_rssi[slot].mac_tail, reporter_mac_tail, 3);
    s_peer_rssi[slot].rssi = (int8_t)rssi;
    s_peer_rssi[slot].last_seen_ms = (uint32_t)now_ms();
}

static const char *state_abbr(nantuo_lamp_state_t state)
{
    switch (state) {
    case NANTUO_STATE_ACTIVE:
        return "ACT";
    case NANTUO_STATE_SLEEPING:
        return "SLP";
    default:
        return "LIS";
    }
}

void nantuo_lamp_format_ui(char *line1, size_t l1,
                           char *line2, size_t l2,
                           char *line3, size_t l3,
                           bool mqtt_connected,
                           bool wifi_connected,
                           bool wifi_connecting,
                           bool ble_advertising,
                           bool ble_scanning,
                           const char *ip_short)
{
    char mac_suffix[8];
    const char *mq = mqtt_connected ? "MQ:ok" : "MQ:--";
    const char *wf;
    const char *ip;

    nantuo_lamp_get_mac_suffix(mac_suffix, sizeof(mac_suffix));

    if (wifi_connected && ip_short && ip_short[0]) {
        wf = "";
        ip = ip_short;
    } else if (wifi_connecting) {
        wf = "WF:..";
        ip = "";
    } else {
        wf = "WF:--";
        ip = "";
    }

    if (line1 && l1 > 0) {
        if (s_beat >= 0 && ip[0]) {
            snprintf(line1, l1, "%s %s%s B%d", mq, wf, ip, s_beat);
        } else if (ip[0]) {
            snprintf(line1, l1, "%s %s%s", mq, wf, ip);
        } else if (wf[0]) {
            snprintf(line1, l1, "%s %s", mq, wf);
        } else {
            snprintf(line1, l1, "%s no-IP", mq);
        }
    }
    if (line2 && l2 > 0) {
        snprintf(line2, l2, "%s %s b:%u%s",
                 state_abbr(s_state),
                 mac_suffix,
                 (unsigned)s_brightness,
                 s_is_seed ? "*" : "");
    }
    if (line3 && l3 > 0) {
        int nb = nantuo_lamp_neighbor_count();
        int best_rssi = -127;
        char peer[8] = "--";
        const char *adv = ble_advertising ? "A+" : "A-";
        const char *scan = ble_scanning ? "S+" : "S-";

        for (int i = 0; i < NANTUO_MAX_NEIGHBORS; i++) {
            if (s_neighbors[i].last_seen_ms == 0) {
                continue;
            }
            {
                int8_t fused = nantuo_lamp_fused_rssi(s_neighbors[i].mac_tail,
                                                      s_neighbors[i].rssi);
                if (fused > best_rssi) {
                    best_rssi = fused;
                    snprintf(peer, sizeof(peer), "%02x%02x",
                             s_neighbors[i].mac_tail[1],
                             s_neighbors[i].mac_tail[2]);
                }
            }
        }
        if (nb > 0) {
            float dist_m = nantuo_lamp_rssi_dist_m((int8_t)best_rssi);
            snprintf(line3, l3, "Nb:%d %s%s %.1fm@%d",
                     nb, adv, scan, (double)dist_m, (int)best_rssi);
        } else {
            snprintf(line3, l3, "Nb:0 %s%s scan", adv, scan);
        }
    }
}

void nantuo_lamp_fill_adv_payload(nantuo_adv_mfg_t *payload)
{
    if (payload == NULL) {
        return;
    }
    payload->company_id = NANTUO_COMPANY_ID;
    payload->magic = NANTUO_ADV_MAGIC;
    payload->state = (uint8_t)s_state;
    payload->brightness = s_brightness;
    payload->gratitude = s_gratitude;
    memcpy(payload->mac_tail, s_mac_tail, 3);
}
