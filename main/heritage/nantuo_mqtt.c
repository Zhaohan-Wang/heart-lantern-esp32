/*
 * 难陀心灯 — MQTT 中枢客户端
 *
 * 订阅:
 *   nantuo/hub/beat        载荷: 数字 0-7
 *   nantuo/hub/relay_seed   载荷: MAC 后缀 "ee:76"
 *   nantuo/lamp/{suffix}/cmd 载荷: beat=N / seed /
 *       led=color=#ff2200,intensity=0.08,period=6400 /
 *       led=bright_color=#ff2200,dark_color=#120020,intensity=0.08,dark_intensity=0.02,period=6400 /
 *       led_reset
 *   nantuo/lamp/+/neighbors 其它灯邻接上报（双向 RSSI 融合）
 *
 * 发布:
 *   nantuo/lamp/{suffix}/status
 *   nantuo/lamp/{suffix}/config/request
 *   nantuo/lamp/{suffix}/neighbors  JSON 邻灯明细（调试）
 */

#include "nantuo_mqtt.h"

#include "nantuo_lamp.h"
#include "nantuo_time.h"
#include "nantuo_ws2812.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "mqtt_client.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#if __has_include("digital_heritage_credentials.h")
#include "digital_heritage_credentials.h"
#endif

#ifndef DH_MQTT_BROKER_URI
#define DH_MQTT_BROKER_URI "mqtt://192.168.5.1:1883"
#endif

#define TOPIC_NEIGHBORS_WILDCARD  "nantuo/lamp/+/neighbors"
#define TOPIC_SHAKE_WILDCARD      "nantuo/lamp/+/shake"

static const char *TAG = "nantuo_mqtt";

static esp_mqtt_client_handle_t s_client;
static bool s_connected;
static char s_suffix[8];
static char s_broker_uri[64];
static char s_topic_status[48];
static char s_topic_neighbors[48];
static char s_topic_cmd[48];
static char s_topic_config_request[64];
static nantuo_mqtt_ui_refresh_cb_t s_refresh_cb;
static int64_t s_last_neighbors_pub_ms;

/** 从 neighbors 主题提取 suffix，如 nantuo/lamp/ee74/neighbors → ee74 */
static bool extract_neighbors_suffix(const char *topic, char *suffix, size_t suffix_len)
{
    const char *prefix = "nantuo/lamp/";
    const char *tail = "/neighbors";

    if (topic == NULL || suffix == NULL || suffix_len == 0) {
        return false;
    }
    if (strncmp(topic, prefix, strlen(prefix)) != 0) {
        return false;
    }
    if (!strstr(topic, tail)) {
        return false;
    }

    const char *start = topic + strlen(prefix);
    const char *end = strstr(start, tail);
    if (end == NULL || end == start) {
        return false;
    }

    size_t len = (size_t)(end - start);
    if (len >= suffix_len) {
        return false;
    }
    memcpy(suffix, start, len);
    suffix[len] = '\0';
    return true;
}

/** suffix（4 hex）→ mac_tail 后两字节；首字节填 0 */
static void suffix_to_mac_tail(const char *suffix, uint8_t mac_tail[3])
{
    unsigned b1 = 0;
    unsigned b2 = 0;

    mac_tail[0] = 0;
    mac_tail[1] = 0;
    mac_tail[2] = 0;
    if (suffix == NULL) {
        return;
    }
    if (sscanf(suffix, "%2x%2x", &b1, &b2) == 2) {
        mac_tail[1] = (uint8_t)b1;
        mac_tail[2] = (uint8_t)b2;
    }
}

static void handle_peer_neighbors(const char *topic, const char *data, int data_len)
{
    char reporter_suffix[8];
    char payload[640];
    uint8_t reporter_tail[3];

    if (!extract_neighbors_suffix(topic, reporter_suffix, sizeof(reporter_suffix))) {
        return;
    }
    if (strcmp(reporter_suffix, s_suffix) == 0) {
        return;
    }
    if (data == NULL || data_len <= 0) {
        return;
    }
    if (data_len >= (int)sizeof(payload)) {
        data_len = (int)sizeof(payload) - 1;
    }
    memcpy(payload, data, (size_t)data_len);
    payload[data_len] = '\0';

    suffix_to_mac_tail(reporter_suffix, reporter_tail);
    nantuo_lamp_on_peer_neighbors_json(reporter_tail, payload);
    nantuo_mqtt_publish_neighbors_throttled();
    if (s_refresh_cb) {
        s_refresh_cb();
    }
}

/** 从 shake 主题提取 suffix */
static bool extract_shake_suffix(const char *topic, char *suffix, size_t suffix_len)
{
    const char *prefix = "nantuo/lamp/";
    const char *tail = "/shake";

    if (topic == NULL || suffix == NULL || suffix_len == 0) {
        return false;
    }
    if (strncmp(topic, prefix, strlen(prefix)) != 0) {
        return false;
    }
    if (!strstr(topic, tail)) {
        return false;
    }

    const char *start = topic + strlen(prefix);
    const char *end = strstr(start, tail);
    if (end == NULL || end == start) {
        return false;
    }

    size_t len = (size_t)(end - start);
    if (len >= suffix_len) {
        return false;
    }
    memcpy(suffix, start, len);
    suffix[len] = '\0';
    return true;
}

/** 处理其它灯发布的摇晃涟漪 */
static void handle_peer_shake(const char *topic, const char *data, int data_len)
{
    char reporter_suffix[8];
    char payload[640];
    char my_mac[12];
    char pattern[24];
    const char *found;
    int delay_ms = 0;
    int bright = 0;
    float dist_m = 0.0f;
    int64_t fire_at_ms = 0;
    int64_t now_wall_ms;
    int32_t remain_ms;

    if (!extract_shake_suffix(topic, reporter_suffix, sizeof(reporter_suffix))) {
        return;
    }
    if (strcmp(reporter_suffix, s_suffix) == 0) {
        return;
    }
    if (data == NULL || data_len <= 0) {
        return;
    }
    if (data_len >= (int)sizeof(payload)) {
        data_len = (int)sizeof(payload) - 1;
    }
    memcpy(payload, data, (size_t)data_len);
    payload[data_len] = '\0';

    nantuo_lamp_get_mac_suffix(my_mac, sizeof(my_mac));
    snprintf(pattern, sizeof(pattern), "\"mac\":\"%s\"", my_mac);
    found = strstr(payload, pattern);
    if (found == NULL) {
        return;
    }

    if (sscanf(found, "\"mac\":\"%*[^\"]\",\"dist_m\":%f,\"delay_ms\":%d,\"bright\":%d",
                     &dist_m, &delay_ms, &bright) != 3) {
        if (sscanf(found, "\"dist_m\":%f,\"delay_ms\":%d,\"bright\":%d",
                         &dist_m, &delay_ms, &bright) != 3) {
            return;
        }
    }

    if (delay_ms < 0) {
        delay_ms = 0;
    }

    {
        const char *fire_key = strstr(found, "\"fire_at_ms\":");
        if (fire_key != NULL) {
            sscanf(fire_key, "\"fire_at_ms\":%lld", (long long *)&fire_at_ms);
        }
    }

    remain_ms = delay_ms;
    if (fire_at_ms > 0 && nantuo_time_is_synced()) {
        now_wall_ms = nantuo_time_wall_ms();
        if (now_wall_ms >= 0) {
            remain_ms = (int32_t)(fire_at_ms - now_wall_ms);
            if (remain_ms < 0) {
                remain_ms = 0;
            }
        }
    }

    ESP_LOGI(TAG,
             "shake from %s dist=%.1fm delay=%dms remain=%dms bright=%d sync=%d",
             reporter_suffix, (double)dist_m, delay_ms, (int)remain_ms, bright,
             (int)nantuo_time_is_synced());
    nantuo_lamp_on_shake_incoming((uint32_t)remain_ms, (uint8_t)bright);
    if (s_refresh_cb) {
        s_refresh_cb();
    }
}

static void handle_hub_message(const char *topic, const char *data, int data_len)
{
    char buf[192];

    if (strstr(topic, "/neighbors") != NULL) {
        handle_peer_neighbors(topic, data, data_len);
        return;
    }

    if (strstr(topic, "/shake") != NULL) {
        handle_peer_shake(topic, data, data_len);
        return;
    }

    if (data == NULL || data_len <= 0) {
        return;
    }
    if (data_len >= (int)sizeof(buf)) {
        data_len = (int)sizeof(buf) - 1;
    }
    memcpy(buf, data, (size_t)data_len);
    buf[data_len] = '\0';

    if (strstr(topic, "nantuo/hub/beat") != NULL) {
        int beat = -1;
        if (sscanf(buf, "%d", &beat) == 1) {
            nantuo_lamp_on_hub_beat(beat);
            if (s_refresh_cb) {
                s_refresh_cb();
            }
        }
        return;
    }

    if (strstr(topic, "nantuo/hub/relay_seed") != NULL) {
        nantuo_lamp_on_relay_seed(buf);
        if (s_refresh_cb) {
            s_refresh_cb();
        }
        return;
    }

    if (strstr(topic, "nantuo/lamp/") != NULL && strstr(topic, "/cmd") != NULL) {
        int beat = -1;
        if (sscanf(buf, "beat=%d", &beat) == 1) {
            nantuo_lamp_on_hub_beat(beat);
        } else if (strncmp(buf, "seed", 4) == 0) {
            char mac[8];
            nantuo_lamp_get_mac_suffix(mac, sizeof(mac));
            nantuo_lamp_on_relay_seed(mac);
        } else if (nantuo_ws2812_handle_command(buf)) {
            nantuo_mqtt_publish_status();
        }
        if (s_refresh_cb) {
            s_refresh_cb();
        }
    }
}

static void publish_config_request(void)
{
    char payload[64];
    char mac[8];

    if (!s_connected || s_client == NULL) {
        return;
    }

    nantuo_lamp_get_mac_suffix(mac, sizeof(mac));
    snprintf(payload, sizeof(payload), "type=led_config_request,mac=%s", mac);
    esp_mqtt_client_publish(s_client, s_topic_config_request, payload, 0, 1, 0);
}

static void mqtt_event_handler(void *handler_args,
                               esp_event_base_t base,
                               int32_t event_id,
                               void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    (void)handler_args;
    (void)base;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        ESP_LOGI(TAG, "connected to broker");
        esp_mqtt_client_subscribe(s_client, "nantuo/hub/beat", 1);
        esp_mqtt_client_subscribe(s_client, "nantuo/hub/relay_seed", 1);
        esp_mqtt_client_subscribe(s_client, s_topic_cmd, 1);
        esp_mqtt_client_subscribe(s_client, TOPIC_NEIGHBORS_WILDCARD, 0);
        esp_mqtt_client_subscribe(s_client, TOPIC_SHAKE_WILDCARD, 0);
        nantuo_mqtt_publish_status();
        publish_config_request();
        nantuo_mqtt_publish_neighbors();
        if (s_refresh_cb) {
            s_refresh_cb();
        }
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(TAG, "disconnected");
        if (s_refresh_cb) {
            s_refresh_cb();
        }
        break;
    case MQTT_EVENT_DATA:
        if (event->topic_len > 0 && event->data_len > 0) {
            char topic[64];
            int tlen = event->topic_len;
            if (tlen >= (int)sizeof(topic)) {
                tlen = (int)sizeof(topic) - 1;
            }
            memcpy(topic, event->topic, (size_t)tlen);
            topic[tlen] = '\0';
            handle_hub_message(topic, event->data, event->data_len);
        }
        break;
    default:
        break;
    }
}

void nantuo_mqtt_start(const char *mac_topic_suffix,
                       const char *broker_uri,
                       nantuo_mqtt_ui_refresh_cb_t refresh_cb)
{
    const char *uri = broker_uri;

    if (mac_topic_suffix == NULL || mac_topic_suffix[0] == '\0') {
        ESP_LOGE(TAG, "invalid mac topic suffix");
        return;
    }
    if (uri == NULL || uri[0] == '\0') {
        uri = DH_MQTT_BROKER_URI;
    }

    /* 已连接同一 Broker 时无需重建客户端 */
    if (s_client != NULL && strcmp(s_broker_uri, uri) == 0) {
        return;
    }

    if (s_client != NULL) {
        nantuo_mqtt_stop();
    }

    s_refresh_cb = refresh_cb;
    s_last_neighbors_pub_ms = 0;
    strlcpy(s_suffix, mac_topic_suffix, sizeof(s_suffix));
    strlcpy(s_broker_uri, uri, sizeof(s_broker_uri));
    snprintf(s_topic_status, sizeof(s_topic_status), "nantuo/lamp/%s/status", s_suffix);
    snprintf(s_topic_neighbors, sizeof(s_topic_neighbors), "nantuo/lamp/%s/neighbors", s_suffix);
    snprintf(s_topic_cmd, sizeof(s_topic_cmd), "nantuo/lamp/%s/cmd", s_suffix);
    snprintf(s_topic_config_request, sizeof(s_topic_config_request),
             "nantuo/lamp/%s/config/request", s_suffix);

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = s_broker_uri,
        .credentials.client_id = s_suffix,
    };

    s_client = esp_mqtt_client_init(&cfg);
    if (s_client == NULL) {
        ESP_LOGE(TAG, "mqtt client init failed");
        return;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_client);
    ESP_LOGI(TAG, "connecting %s as %s", s_broker_uri, s_suffix);
}

void nantuo_mqtt_stop(void)
{
    if (s_client) {
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
    }
    s_connected = false;
    s_broker_uri[0] = '\0';
}

bool nantuo_mqtt_is_connected(void)
{
    return s_connected;
}

void nantuo_mqtt_publish_status(void)
{
    char payload[256];
    char mac[8];
    nantuo_ws2812_breath_config_t led_cfg;

    if (!s_connected || s_client == NULL) {
        return;
    }

    nantuo_lamp_get_mac_suffix(mac, sizeof(mac));
    nantuo_ws2812_get_breath_config(&led_cfg);
    snprintf(payload, sizeof(payload),
             "state=%d,bright=%u,beat=%d,seed=%d,nb=%d,mac=%s,led_color=#%02x%02x%02x,led_dark_color=#%02x%02x%02x,led_intensity=%.3f,led_dark_intensity=%.3f,led_period=%u",
             (int)nantuo_lamp_get_state(),
             (unsigned)nantuo_lamp_get_brightness(),
             nantuo_lamp_get_beat(),
             (int)nantuo_lamp_is_seed(),
             nantuo_lamp_neighbor_count(),
             mac,
             (unsigned)led_cfg.red,
             (unsigned)led_cfg.green,
             (unsigned)led_cfg.blue,
             (unsigned)led_cfg.dark_red,
             (unsigned)led_cfg.dark_green,
             (unsigned)led_cfg.dark_blue,
             (double)led_cfg.intensity,
             (double)led_cfg.dark_intensity,
             (unsigned)led_cfg.period_ms);

    esp_mqtt_client_publish(s_client, s_topic_status, payload, 0, 0, 0);
}

void nantuo_mqtt_publish_neighbors(void)
{
    nantuo_neighbor_t peers[NANTUO_MAX_NEIGHBORS];
    char payload[768];
    int count;
    int pos = 0;

    if (!s_connected || s_client == NULL) {
        return;
    }

    count = nantuo_lamp_neighbor_snapshot(peers, NANTUO_MAX_NEIGHBORS);
    pos = snprintf(payload, sizeof(payload), "{\"nb\":%d,\"peers\":[", count);
    if (pos < 0 || pos >= (int)sizeof(payload)) {
        return;
    }

    for (int i = 0; i < count; i++) {
        char mac[12];
        const char *tier;
        int8_t local_rssi;
        int8_t fused_rssi;
        int8_t peer_rssi;
        bool has_peer;
        float dist_m;
        int written;

        snprintf(mac, sizeof(mac), "%02x:%02x",
                 peers[i].mac_tail[1], peers[i].mac_tail[2]);
        local_rssi = peers[i].rssi;
        has_peer = nantuo_lamp_has_peer_rssi(peers[i].mac_tail);
        fused_rssi = nantuo_lamp_fused_rssi(peers[i].mac_tail, local_rssi);
        peer_rssi = nantuo_lamp_get_peer_rssi(peers[i].mac_tail);
        tier = nantuo_lamp_rssi_tier(fused_rssi);
        dist_m = nantuo_lamp_rssi_dist_m(fused_rssi);

        if (i > 0) {
            written = snprintf(payload + pos, sizeof(payload) - (size_t)pos, ",");
            if (written <= 0) {
                return;
            }
            pos += written;
        }

        if (has_peer) {
            written = snprintf(payload + pos, sizeof(payload) - (size_t)pos,
                               "{\"mac\":\"%s\",\"rssi\":%d,\"rssi_local\":%d,"
                               "\"rssi_peer\":%d,\"state\":%u,"
                               "\"bright\":%u,\"tier\":\"%s\",\"dist_m\":%.1f,"
                               "\"fused\":1}",
                               mac,
                               (int)fused_rssi,
                               (int)local_rssi,
                               (int)peer_rssi,
                               (unsigned)peers[i].state,
                               (unsigned)peers[i].brightness,
                               tier,
                               (double)dist_m);
        } else {
            written = snprintf(payload + pos, sizeof(payload) - (size_t)pos,
                               "{\"mac\":\"%s\",\"rssi\":%d,\"rssi_local\":%d,"
                               "\"state\":%u,\"bright\":%u,\"tier\":\"%s\","
                               "\"dist_m\":%.1f,\"fused\":0}",
                               mac,
                               (int)local_rssi,
                               (int)local_rssi,
                               (unsigned)peers[i].state,
                               (unsigned)peers[i].brightness,
                               tier,
                               (double)dist_m);
        }
        if (written <= 0 || pos + written >= (int)sizeof(payload)) {
            return;
        }
        pos += written;
    }

    {
        int written = snprintf(payload + pos, sizeof(payload) - (size_t)pos, "]}");
        if (written <= 0) {
            return;
        }
    }

    esp_mqtt_client_publish(s_client, s_topic_neighbors, payload, 0, 0, 0);
    s_last_neighbors_pub_ms = esp_timer_get_time() / 1000;
}

void nantuo_mqtt_publish_shake(void)
{
    nantuo_shake_peer_plan_t plans[NANTUO_MAX_NEIGHBORS];
    char payload[768];
    char from_mac[12];
    int count;
    int pos;
    int64_t origin_ms;
    bool time_synced;

    if (!s_connected || s_client == NULL) {
        return;
    }

    count = nantuo_lamp_build_shake_plan(plans, NANTUO_MAX_NEIGHBORS);
    nantuo_lamp_get_mac_suffix(from_mac, sizeof(from_mac));
    time_synced = nantuo_time_is_synced();
    origin_ms = nantuo_time_wall_ms();

    if (time_synced && origin_ms >= 0) {
        pos = snprintf(payload, sizeof(payload),
                       "{\"from\":\"%s\",\"synced\":1,\"origin_ms\":%lld,\"peers\":[",
                       from_mac, (long long)origin_ms);
    } else {
        pos = snprintf(payload, sizeof(payload),
                       "{\"from\":\"%s\",\"synced\":0,\"peers\":[", from_mac);
    }
    if (pos < 0 || pos >= (int)sizeof(payload)) {
        return;
    }

    for (int i = 0; i < count; i++) {
        int written;

        if (i > 0) {
            written = snprintf(payload + pos, sizeof(payload) - (size_t)pos, ",");
            if (written <= 0) {
                return;
            }
            pos += written;
        }
        if (time_synced && origin_ms >= 0) {
            int64_t fire_at = origin_ms + (int64_t)plans[i].delay_ms;
            written = snprintf(payload + pos, sizeof(payload) - (size_t)pos,
                               "{\"mac\":\"%s\",\"dist_m\":%.1f,"
                               "\"delay_ms\":%u,\"bright\":%u,"
                               "\"fire_at_ms\":%lld}",
                               plans[i].mac,
                               (double)plans[i].dist_m,
                               (unsigned)plans[i].delay_ms,
                               (unsigned)plans[i].bright,
                               (long long)fire_at);
        } else {
            written = snprintf(payload + pos, sizeof(payload) - (size_t)pos,
                               "{\"mac\":\"%s\",\"dist_m\":%.1f,"
                               "\"delay_ms\":%u,\"bright\":%u}",
                               plans[i].mac,
                               (double)plans[i].dist_m,
                               (unsigned)plans[i].delay_ms,
                               (unsigned)plans[i].bright);
        }
        if (written <= 0 || pos + written >= (int)sizeof(payload)) {
            return;
        }
        pos += written;
    }

    {
        int written = snprintf(payload + pos, sizeof(payload) - (size_t)pos, "]}");
        if (written <= 0) {
            return;
        }
    }

    {
        char topic[48];
        snprintf(topic, sizeof(topic), "nantuo/lamp/%s/shake", s_suffix);
        esp_mqtt_client_publish(s_client, topic, payload, 0, 1, 0);
        ESP_LOGI(TAG, "shake pub %s", payload);
    }
}

void nantuo_mqtt_publish_neighbors_throttled(void)
{
    int64_t now = esp_timer_get_time() / 1000;
    uint32_t interval = nantuo_lamp_neighbors_publish_interval_ms();

    if ((now - s_last_neighbors_pub_ms) < (int64_t)interval) {
        return;
    }
    nantuo_mqtt_publish_neighbors();
}
