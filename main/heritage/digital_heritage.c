/*
 * Digital Heritage / 难陀心灯原型：
 * Wi-Fi STA + MQTT Hub + BLE 广播/扫描邻接 + 传灯状态机
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "digital_heritage.h"
#include "nantuo_lamp.h"
#include "nantuo_mqtt.h"
#include "nantuo_time.h"
#include "nantuo_types.h"

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_lvgl_port.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_wifi.h"

#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#endif

#if __has_include("digital_heritage_credentials.h")
#include "digital_heritage_credentials.h"
#endif

#ifndef DH_WIFI_SSID
#define DH_WIFI_SSID "ZTE-302"
#endif
#ifndef DH_WIFI_PASS
#define DH_WIFI_PASS ""
#endif
#ifndef DH_MQTT_BROKER_URI
#define DH_MQTT_BROKER_URI "mqtt://192.168.5.1:1883"
#endif
#define DH_BLE_NAME  "Nantuo"
/** 单次 BLE 扫描时长（ms）；扫描期间会暂停广播 */
#define DH_SCAN_DURATION_MS   800
#define DH_SCAN_DURATION_NEAR_MS  500
#define DH_SCAN_PERIOD_MS     3000

/** 单条 Wi-Fi 候选（含该网络对应的 MQTT Broker） */
typedef struct {
    const char *ssid;
    const char *password;
    const char *mqtt_broker_uri;
} dh_wifi_network_t;

#if defined(DH_WIFI_NETWORK_TABLE)
/** 凭据文件中的多网络列表 */
static const dh_wifi_network_t s_wifi_networks[] = {
    DH_WIFI_NETWORK_TABLE
};
#else
/** 单 SSID（来自凭据文件或下方默认值） */
static const dh_wifi_network_t s_wifi_networks[] = {
    { DH_WIFI_SSID, DH_WIFI_PASS, DH_MQTT_BROKER_URI },
};
#endif

#define DH_WIFI_NETWORK_COUNT (sizeof(s_wifi_networks) / sizeof(s_wifi_networks[0]))

#ifndef LVGL_LOCK_TIMEOUT_MS
#define LVGL_LOCK_TIMEOUT_MS 200
#endif

#define DH_STATUS_PERIOD_MS   5000

static const char *TAG = "digital_heritage";

static SemaphoreHandle_t s_status_mutex = NULL;
static digital_heritage_status_t s_status;
static char s_ip_short[16];
static char s_mqtt_topic_suffix[8];

static bool s_nvs_ready;
static bool s_netif_ready;
static bool s_wifi_ready;
static bool s_wifi_started;
static bool s_wifi_connecting;
static bool s_wifi_has_ip;
static size_t s_wifi_profile_idx; /**< 当前正在尝试的候选网络下标 */
static bool s_ble_ready;
static bool s_ble_host_started;
static bool s_ble_advertising;
static bool s_ble_scanning;
static bool s_dh_session_active;  /**< 处于 Digital Heritage 界面会话 */
static bool s_roaming_mode;       /**< 漫游：停 Wi-Fi 重试，持续 BLE 扫邻灯 */
static TaskHandle_t s_worker_task;
static TaskHandle_t s_scan_task;
static int64_t s_last_status_pub_ms;
static bool s_shake_key_lit;
static bool s_status_pub_after_pulse;
static bool s_sntp_started;

#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED
static uint8_t s_ble_addr_type;
static uint8_t s_ble_addr[6];
#endif

static void status_lock(void)
{
    if (s_status_mutex) {
        xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    }
}

static void status_unlock(void)
{
    if (s_status_mutex) {
        xSemaphoreGive(s_status_mutex);
    }
}

/**
 * 用芯片出厂 Wi-Fi MAC 生成本机唯一 ID（PC/MQTT 靠此区分不同设备）
 * - MQTT 主题: nantuo/lamp/{suffix}/status，suffix = MAC 后两字节，如 9e84
 * - MQTT client_id 同上
 * - BLE 广播 mac_tail 与协议文档一致（如 81:9e:84 的后缀展示为 9e:84）
 */
static void setup_device_identity(void)
{
    uint8_t mac[6];

    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        ESP_LOGE(TAG, "read WiFi MAC failed");
        return;
    }

    nantuo_lamp_set_mac_tail(&mac[3]);
    snprintf(s_mqtt_topic_suffix, sizeof(s_mqtt_topic_suffix),
             "%02x%02x", mac[4], mac[5]);
    ESP_LOGI(TAG,
             "device id MAC=%02x:%02x:%02x:%02x:%02x:%02x mqtt=nantuo/lamp/%s",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
             s_mqtt_topic_suffix);
}

/** 根据灯态 / 连接状态刷新 OLED */
static void refresh_heritage_ui(void)
{
    digital_heritage_status_t copy;

    status_lock();
    s_status.wifi_connected = s_wifi_has_ip;
    s_status.wifi_connecting = s_wifi_connecting || (s_wifi_started && !s_wifi_has_ip);
    s_status.mqtt_connected = nantuo_mqtt_is_connected();
    s_status.roaming = s_roaming_mode;
    copy = s_status;
    status_unlock();

    if (lvgl_port_lock(LVGL_LOCK_TIMEOUT_MS)) {
        if (ui_is_in_digital_heritage_mode()) {
            digital_heritage_ui_set_status(&copy);
        }
        lvgl_port_unlock();
    }
}

void digital_heritage_get_status(digital_heritage_status_t *status)
{
    if (!status) {
        return;
    }
    status_lock();
    *status = s_status;
    status_unlock();
}

/** SNTP 首次同步完成 */
static void on_sntp_sync(struct timeval *tv)
{
    (void)tv;
    ESP_LOGI(TAG, "SNTP time synced");
}

void nantuo_time_start(void)
{
    if (s_sntp_started) {
        return;
    }
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "pool.ntp.org");
    esp_sntp_set_time_sync_notification_cb(on_sntp_sync);
    esp_sntp_init();
    s_sntp_started = true;
    ESP_LOGI(TAG, "SNTP started");
}

bool nantuo_time_is_synced(void)
{
    if (!s_sntp_started) {
        return false;
    }
    return esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED;
}

int64_t nantuo_time_wall_ms(void)
{
    struct timeval tv;

    if (!nantuo_time_is_synced()) {
        return -1;
    }
    if (gettimeofday(&tv, NULL) != 0) {
        return -1;
    }
    if (tv.tv_sec < 1600000000L) {
        return -1;
    }
    return (int64_t)tv.tv_sec * 1000LL + (int64_t)tv.tv_usec / 1000LL;
}

static void format_ip_short(const esp_ip4_addr_t *ip, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }
    snprintf(out, out_len, "." IPSTR, IP2STR(ip));
}

static void on_wifi_got_ip(const esp_ip4_addr_t *ip)
{
    const char *broker_uri = s_wifi_networks[s_wifi_profile_idx].mqtt_broker_uri;

    s_wifi_has_ip = true;
    format_ip_short(ip, s_ip_short, sizeof(s_ip_short));
    nantuo_time_start();
    refresh_heritage_ui();

    if (broker_uri == NULL || broker_uri[0] == '\0') {
        broker_uri = DH_MQTT_BROKER_URI;
    }

    if (s_mqtt_topic_suffix[0] != '\0') {
        nantuo_mqtt_start(s_mqtt_topic_suffix, broker_uri, refresh_heritage_ui);
    }
}

static esp_err_t ensure_nvs(void)
{
    if (s_nvs_ready) {
        return ESP_OK;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err == ESP_OK) {
        s_nvs_ready = true;
    }
    return err;
}

/** 将指定下标的 Wi-Fi 候选写入 STA 配置 */
static esp_err_t wifi_apply_profile(size_t index)
{
    if (index >= DH_WIFI_NETWORK_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    const dh_wifi_network_t *net = &s_wifi_networks[index];
    wifi_config_t wifi_config = {0};

    strlcpy((char *)wifi_config.sta.ssid, net->ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, net->password, sizeof(wifi_config.sta.password));
    if (net->password[0] == '\0') {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    } else {
        /* 阈值取 WPA2：兼容仅 WPA2 的路由器（阈值过高会报 reason=211） */
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    }

    ESP_LOGI(TAG, "try Wi-Fi [%u/%u] ssid=%s",
             (unsigned)(index + 1), (unsigned)DH_WIFI_NETWORK_COUNT, net->ssid);
    return esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
}

/** 连接失败时扫描并打印周围可见 AP，便于排查 SSID / 频段问题 */
static void wifi_log_visible_aps(void)
{
    wifi_scan_config_t scan_cfg = {
        .show_hidden = true,
    };
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi scan failed: %s", esp_err_to_name(err));
        return;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count == 0) {
        ESP_LOGW(TAG, "wifi scan: no AP visible (check antenna / 2.4GHz only)");
        return;
    }

    wifi_ap_record_t *records = calloc(ap_count, sizeof(wifi_ap_record_t));
    if (records == NULL) {
        ESP_LOGW(TAG, "wifi scan: OOM for %u AP records", (unsigned)ap_count);
        return;
    }

    esp_wifi_scan_get_ap_records(&ap_count, records);
    ESP_LOGW(TAG, "wifi scan: %u AP(s) visible:", (unsigned)ap_count);
    for (uint16_t i = 0; i < ap_count; i++) {
        ESP_LOGW(TAG, "  ssid=\"%s\" rssi=%d ch=%u auth=%u",
                 (const char *)records[i].ssid,
                 (int)records[i].rssi,
                 (unsigned)records[i].primary,
                 (unsigned)records[i].authmode);
    }
    free(records);
}

/** 进入漫游：不再自动连 Wi-Fi，持续 BLE 扫描邻灯 */
static void enter_roaming_mode(void)
{
    if (s_roaming_mode) {
        return;
    }

    s_roaming_mode = true;
    s_wifi_connecting = false;
    if (s_wifi_started) {
        esp_wifi_disconnect();
    }
    ESP_LOGI(TAG, "roaming mode ON: BLE neighbor scan without Wi-Fi");
    refresh_heritage_ui();
}

/** 退出漫游（例如连上 Wi-Fi 或用户按键重试） */
static void exit_roaming_mode(void)
{
    if (!s_roaming_mode) {
        return;
    }

    s_roaming_mode = false;
    ESP_LOGI(TAG, "roaming mode OFF");
    refresh_heritage_ui();
}

/** 当前是否允许触发 BLE 邻灯扫描 */
static bool heritage_ble_scan_allowed(void)
{
    return s_ble_ready;
}

/** 按 MAC 错开扫描相位，避免多灯同时停广播导致互扫不到 */
static uint32_t ble_scan_phase_offset_ms(void)
{
    uint8_t mac[6];

    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        return 0;
    }
    return ((uint32_t)mac[5] * 131u + (uint32_t)mac[4] * 17u) % DH_SCAN_PERIOD_MS;
}

/** 当前候选连接失败时，切换到下一个候选并重新连接 */
static void wifi_advance_profile_and_connect(void)
{
    const size_t prev_idx = s_wifi_profile_idx;

    s_wifi_profile_idx = (s_wifi_profile_idx + 1) % DH_WIFI_NETWORK_COUNT;
    if (s_wifi_profile_idx == 0 && prev_idx != 0) {
        ESP_LOGW(TAG, "all Wi-Fi profiles failed, enter roaming mode");
        enter_roaming_mode();
        return;
    }

    if (wifi_apply_profile(s_wifi_profile_idx) != ESP_OK) {
        ESP_LOGE(TAG, "apply Wi-Fi profile failed");
        refresh_heritage_ui();
        return;
    }

    s_wifi_connecting = true;
    refresh_heritage_ui();
    esp_wifi_connect();
}

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        s_wifi_started = true;
        s_wifi_has_ip = false;
        refresh_heritage_ui();
        if (wifi_apply_profile(s_wifi_profile_idx) != ESP_OK) {
            ESP_LOGE(TAG, "apply Wi-Fi profile on start failed");
            return;
        }
        s_wifi_connecting = true;
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
        const bool was_connected = s_wifi_has_ip;

        s_wifi_connecting = false;
        s_wifi_has_ip = false;
        s_ip_short[0] = '\0';
        refresh_heritage_ui();

        ESP_LOGW(TAG, "Wi-Fi disconnected reason=%d ssid=%s",
                 disc->reason, s_wifi_networks[s_wifi_profile_idx].ssid);

        if (s_roaming_mode) {
            /* 漫游中不自动重连 Wi-Fi */
            return;
        }

        if (disc->reason == WIFI_REASON_NO_AP_FOUND) {
            ESP_LOGW(TAG, "AP not found — ESP32 仅支持 2.4GHz，请确认 SSID 不是 5G 专用");
        }

        if (was_connected) {
            /* 曾经连上过：优先重连当前网络 */
            s_wifi_connecting = true;
            esp_wifi_connect();
        } else {
            /* 从未拿到 IP：尝试下一个候选（扫描放到切换之后，避免阻塞事件循环） */
            wifi_advance_profile_and_connect();
            if (disc->reason == WIFI_REASON_NO_AP_FOUND) {
                wifi_log_visible_aps();
            }
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        s_wifi_connecting = false;
        exit_roaming_mode();
        ESP_LOGI(TAG, "Wi-Fi connected ssid=%s",
                 s_wifi_networks[s_wifi_profile_idx].ssid);
        on_wifi_got_ip(&event->ip_info.ip);
    }
}

static esp_err_t ensure_wifi(void)
{
    if (s_wifi_ready) {
        return ESP_OK;
    }

    esp_err_t err = ensure_nvs();
    if (err != ESP_OK) {
        return err;
    }

    if (!s_netif_ready) {
        err = esp_netif_init();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            return err;
        }
        err = esp_event_loop_create_default();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            return err;
        }
        esp_netif_create_default_wifi_sta();
        s_netif_ready = true;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK && err != ESP_ERR_WIFI_INIT_STATE) {
        return err;
    }

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    s_wifi_ready = true;
    refresh_heritage_ui();
    return ESP_OK;
}

/** 是否至少配置了一个有效 SSID */
static bool wifi_has_any_profile(void)
{
    for (size_t i = 0; i < DH_WIFI_NETWORK_COUNT; i++) {
        if (s_wifi_networks[i].ssid[0] != '\0') {
            return true;
        }
    }
    return false;
}

static void start_wifi_test(void)
{
    if (!wifi_has_any_profile()) {
        ESP_LOGW(TAG, "no Wi-Fi profile; set main/digital_heritage_credentials.h");
        refresh_heritage_ui();
        return;
    }

    esp_err_t err = ensure_wifi();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi init failed: %s", esp_err_to_name(err));
        refresh_heritage_ui();
        return;
    }

    /* 手动重试时从第一个候选重新开始 */
    s_wifi_profile_idx = 0;

    if (!s_wifi_started) {
        ESP_ERROR_CHECK(esp_wifi_start());
    } else if (!s_wifi_connecting) {
        if (wifi_apply_profile(s_wifi_profile_idx) != ESP_OK) {
            ESP_LOGE(TAG, "apply Wi-Fi profile on retry failed");
            refresh_heritage_ui();
            return;
        }
        s_wifi_connecting = true;
        refresh_heritage_ui();
        esp_wifi_connect();
    }
}

#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED
static void ble_update_mfg_and_adv_fields(struct ble_hs_adv_fields *fields);

static int ble_disc_event(struct ble_gap_event *event, void *arg);
static void ble_start_advertising(void);
static void ble_trigger_scan(void);

static void ble_update_mfg_and_adv_fields(struct ble_hs_adv_fields *fields)
{
    static uint8_t mfg_buf[sizeof(nantuo_adv_mfg_t)];
    nantuo_adv_mfg_t *mfg = (nantuo_adv_mfg_t *)mfg_buf;

    memset(fields, 0, sizeof(*fields));
    fields->flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    /* 广播包上限 31 字节：仅放 flags + mfg，不放设备名（否则 mfg 会被截断） */

    nantuo_lamp_fill_adv_payload(mfg);
    fields->mfg_data = mfg_buf;
    fields->mfg_data_len = sizeof(nantuo_adv_mfg_t);
}

static void ble_parse_disc_report(const struct ble_gap_disc_desc *disc)
{
    struct ble_hs_adv_fields fields;
    nantuo_adv_mfg_t *mfg;

    if (ble_hs_adv_parse_fields(&fields, disc->data, disc->length_data) != 0) {
        return;
    }
    if (fields.mfg_data == NULL || fields.mfg_data_len < sizeof(nantuo_adv_mfg_t)) {
        return;
    }
    mfg = (nantuo_adv_mfg_t *)fields.mfg_data;
    if (mfg->company_id != NANTUO_COMPANY_ID || mfg->magic != NANTUO_ADV_MAGIC) {
        return;
    }

    ESP_LOGD(TAG, "BLE peer adv mac=%02x:%02x rssi=%d state=%u",
             mfg->mac_tail[1], mfg->mac_tail[2], (int)disc->rssi,
             (unsigned)mfg->state);
    nantuo_lamp_on_adv_seen(mfg, disc->rssi);
}

static int ble_disc_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        ble_parse_disc_report(&event->disc);
        return 0;
    case BLE_GAP_EVENT_DISC_COMPLETE:
        s_ble_scanning = false;
        nantuo_lamp_prune_neighbors();
        ESP_LOGI(TAG, "BLE scan done nb=%d", nantuo_lamp_neighbor_count());
        nantuo_mqtt_publish_neighbors_throttled();
        ble_start_advertising();
        refresh_heritage_ui();
        return 0;
    default:
        return 0;
    }
}

static int ble_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (!s_ble_scanning) {
            s_ble_advertising = false;
            ble_start_advertising();
        }
        break;
    default:
        break;
    }
    return 0;
}

static void ble_start_advertising(void)
{
    struct ble_hs_adv_fields fields;
    struct ble_gap_adv_params adv_params = {0};
    int rc;

    ble_update_mfg_and_adv_fields(&fields);
    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        s_ble_advertising = false;
        ESP_LOGE(TAG, "adv set_fields failed: %d (check 31B adv limit)", rc);
        refresh_heritage_ui();
        return;
    }

    adv_params.conn_mode = BLE_GAP_CONN_MODE_NON;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(s_ble_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, ble_gap_event, NULL);
    if (rc == 0 || rc == BLE_HS_EALREADY) {
        s_ble_advertising = true;
    } else {
        s_ble_advertising = false;
        ESP_LOGE(TAG, "adv start failed: %d", rc);
    }
    refresh_heritage_ui();
}

static void ble_trigger_scan(void)
{
    struct ble_gap_disc_params disc_params = {0};
    int rc;

    if (s_ble_scanning || !s_ble_ready) {
        return;
    }

    ble_gap_adv_stop();
    s_ble_advertising = false;
    s_ble_scanning = true;
    refresh_heritage_ui();

    disc_params.passive = 0;
    disc_params.filter_duplicates = 0;
    {
        uint32_t duration = DH_SCAN_DURATION_MS;
        if (nantuo_lamp_best_neighbor_rssi() >= -55) {
            duration = DH_SCAN_DURATION_NEAR_MS;
        }
        rc = ble_gap_disc(s_ble_addr_type, duration, &disc_params,
                          ble_disc_event, NULL);
    }
    if (rc != 0) {
        ESP_LOGW(TAG, "BLE scan start failed: %d", rc);
        s_ble_scanning = false;
        ble_start_advertising();
    } else {
        ESP_LOGD(TAG, "BLE scan started %ums", (unsigned)DH_SCAN_DURATION_MS);
    }
}

static void ble_on_sync(void)
{
    int rc = ble_hs_id_infer_auto(0, &s_ble_addr_type);

    if (rc != 0) {
        ESP_LOGE(TAG, "BLE addr infer failed: %d", rc);
        refresh_heritage_ui();
        return;
    }

    rc = ble_hs_id_copy_addr(s_ble_addr_type, s_ble_addr, NULL);
    if (rc == 0) {
        ESP_LOGI(TAG, "BLE addr=%02x:%02x:%02x:%02x:%02x:%02x",
                 s_ble_addr[5], s_ble_addr[4], s_ble_addr[3],
                 s_ble_addr[2], s_ble_addr[1], s_ble_addr[0]);
    }

    ble_start_advertising();
    refresh_heritage_ui();
}

static void ble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}
#endif

static esp_err_t ensure_ble(void)
{
#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED
    if (s_ble_ready) {
        return ESP_OK;
    }

    esp_err_t err = ensure_nvs();
    if (err != ESP_OK) {
        return err;
    }

    int rc = nimble_port_init();
    if (rc != 0) {
        return ESP_FAIL;
    }

    ble_svc_gap_device_name_set(DH_BLE_NAME);
    ble_hs_cfg.sync_cb = ble_on_sync;

    if (!s_ble_host_started) {
        nimble_port_freertos_init(ble_host_task);
        s_ble_host_started = true;
    }

    s_ble_ready = true;
    refresh_heritage_ui();
    return ESP_OK;
#else
    refresh_heritage_ui();
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static void start_ble_test(void)
{
    esp_err_t err = ensure_ble();
    if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGE(TAG, "BLE init failed: %s", esp_err_to_name(err));
    }
}

static void worker_task(void *arg)
{
    int64_t last_neighbors_pub_ms = 0;

    (void)arg;

    while (true) {
        bool pulse_was_active = nantuo_lamp_bright_pulse_active();

        nantuo_lamp_tick();

        if (pulse_was_active && !nantuo_lamp_bright_pulse_active()) {
            s_status_pub_after_pulse = true;
        }

        /* 脉冲结束后自动熄灭按键指示（不必等抬起） */
        if (s_shake_key_lit &&
            !nantuo_lamp_shake_effects_active()) {
            s_shake_key_lit = false;
            if (lvgl_port_lock(LVGL_LOCK_TIMEOUT_MS)) {
                digital_heritage_ui_set_shake_key(false);
                lvgl_port_unlock();
            }
        }

        int64_t now = esp_timer_get_time() / 1000;
        if ((now - s_last_status_pub_ms) >= DH_STATUS_PERIOD_MS) {
            nantuo_mqtt_publish_status();
            nantuo_mqtt_publish_neighbors();
            s_last_status_pub_ms = now;
            last_neighbors_pub_ms = now;
            s_status_pub_after_pulse = false;
        } else if (s_status_pub_after_pulse) {
            nantuo_mqtt_publish_status();
            s_status_pub_after_pulse = false;
        } else if ((now - last_neighbors_pub_ms) >=
                   (int64_t)nantuo_lamp_neighbors_publish_interval_ms()) {
            nantuo_mqtt_publish_neighbors();
            last_neighbors_pub_ms = now;
        }

        refresh_heritage_ui();

        if (nantuo_lamp_shake_effects_active()) {
            vTaskDelay(pdMS_TO_TICKS(25));
        } else {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
}

static void scan_task(void *arg)
{
    uint32_t phase_ms;

    (void)arg;

    phase_ms = ble_scan_phase_offset_ms();
    ESP_LOGI(TAG, "BLE scan task phase=%ums", (unsigned)phase_ms);
    vTaskDelay(pdMS_TO_TICKS(2000 + phase_ms));
    while (true) {
#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED
        if (heritage_ble_scan_allowed()) {
            ble_trigger_scan();
        }
#endif
        vTaskDelay(pdMS_TO_TICKS(nantuo_lamp_scan_period_ms()));
    }
}

static void ensure_background_tasks(void)
{
    if (s_worker_task == NULL) {
        xTaskCreate(worker_task, "dh_worker", 4096, NULL, 4, &s_worker_task);
    }
    if (s_scan_task == NULL) {
        xTaskCreate(scan_task, "dh_scan", 3072, NULL, 3, &s_scan_task);
    }
}

void digital_heritage_init(void)
{
    if (s_status_mutex == NULL) {
        s_status_mutex = xSemaphoreCreateMutex();
    }
    if (s_status_mutex == NULL) {
        ESP_LOGE(TAG, "status mutex create failed");
        return;
    }

    nantuo_lamp_init();
    setup_device_identity();
    memset(&s_status, 0, sizeof(s_status));
    ensure_background_tasks();
    /* 上电即启 BLE 广播/扫描，邻灯检测不依赖旋钮切 UI */
    start_ble_test();
}

void digital_heritage_start_test(void)
{
    setup_device_identity();
    exit_roaming_mode();
    s_wifi_profile_idx = 0;
    start_wifi_test();
    start_ble_test();
    refresh_heritage_ui();
}

void digital_heritage_simulate_shake(void)
{
    int peer_count;

    s_shake_key_lit = true;
    if (lvgl_port_lock(LVGL_LOCK_TIMEOUT_MS)) {
        digital_heritage_ui_set_shake_key(true);
        lvgl_port_unlock();
    }

    peer_count = nantuo_lamp_on_shake();
    nantuo_mqtt_publish_shake();
    nantuo_mqtt_publish_status();
    ESP_LOGI(TAG, "simulate shake, peers=%d", peer_count);
    refresh_heritage_ui();
}

void digital_heritage_shake_key_set(bool pressed)
{
    if (pressed) {
        s_shake_key_lit = true;
    } else if (!nantuo_lamp_shake_effects_active()) {
        s_shake_key_lit = false;
    }
    if (lvgl_port_lock(LVGL_LOCK_TIMEOUT_MS)) {
        digital_heritage_ui_set_shake_key(pressed);
        lvgl_port_unlock();
    }
}

bool digital_heritage_is_roaming(void)
{
    return s_roaming_mode;
}

bool app_mode_is_digital_heritage(void)
{
    return ui_is_in_digital_heritage_mode();
}

void app_mode_toggle_digital_heritage(void)
{
    bool in_mode;

    if (lvgl_port_lock(LVGL_LOCK_TIMEOUT_MS)) {
        in_mode = ui_is_in_digital_heritage_mode();
        lvgl_port_unlock();
    } else {
        ESP_LOGW(TAG, "lvgl lock timeout");
        return;
    }

    if (in_mode) {
        s_dh_session_active = false;
        exit_roaming_mode();
        if (lvgl_port_lock(LVGL_LOCK_TIMEOUT_MS)) {
            digital_heritage_ui_hide();
            lvgl_port_unlock();
        }
    } else {
        s_dh_session_active = true;
        digital_heritage_status_t status;
        digital_heritage_get_status(&status);
        if (lvgl_port_lock(LVGL_LOCK_TIMEOUT_MS)) {
            digital_heritage_ui_show(&status);
            lvgl_port_unlock();
        }
        digital_heritage_start_test();
    }
}
