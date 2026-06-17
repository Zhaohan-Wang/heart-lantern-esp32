/*
 * 难陀心灯 — WS2812B 灯条（16 颗，数据脚 GPIO37）
 * 仅在难陀（Digital Heritage）模式下工作，亮度与 OLED 点阵共用 nantuo_lamp 引擎。
 *
 * 待机呼吸：仅四个锚点暗红呼吸，给按键串灯留出空间。
 */

#include "nantuo_ws2812.h"

#include "board_pins.h"
#include "digital_heritage.h"
#include "nantuo_lamp.h"
#include "nantuo_types.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "nvs.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "nantuo_ws2812";

#define WS2812_NVS_NAMESPACE "ws2812"
#define WS2812_NVS_BREATH_KEY "breath_cfg"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/** WS2812 数据线 1（LED_DI_1，IO40） */
#define NANTUO_WS2812_GPIO           BOARD_PIN_LED_DI_1
/** WS2812 数据线 2（LED_DI_2，IO39） */
#define NANTUO_WS2812_GPIO_2         BOARD_PIN_LED_DI_2
/** 灯条 LED 数量 */
#define NANTUO_WS2812_LED_COUNT      16
/** RMT 时钟分辨率（Hz） */
#define NANTUO_WS2812_RMT_RES_HZ     (10 * 1000 * 1000)
/** 未串联电阻时降低 GPIO 边沿强度，减轻 WS2812 数据线过冲/振铃 */
#define NANTUO_WS2812_DRIVE_CAP      GPIO_DRIVE_CAP_0
/** 默认环形呼吸周期（ms） */
#define WS2812_DEFAULT_PERIOD_MS     6400U
/** 常态刷新间隔（ms），降低数据线刷新次数，减少未串电阻时的绿闪机会 */
#define NANTUO_WS2812_FRAME_MS       35
/** 摇晃脉冲期间加快刷新（ms） */
#define NANTUO_WS2812_PULSE_FRAME_MS 25
/** 非难陀模式下的轮询间隔（ms） */
#define NANTUO_WS2812_IDLE_POLL_MS   200

/** 默认呼吸峰值强度：前端可运行时覆盖 */
#define WS2812_DEFAULT_INTENSITY     0.5f
/** 呼吸谷值相对峰值的比例：颜色可独立配置，强度仍控制整体亮度包络 */
#define WS2812_AMBIENT_MIN_RATIO     0.5f
/** 前端周期限制，避免误传 0 或过快刷新 */
#define WS2812_PERIOD_MIN_MS         1200U
#define WS2812_PERIOD_MAX_MS         30000U
/** 低于此亮度直接熄灭，非锚点保持真正关闭 */
#define WS2812_IGNITION_CUTOFF       0.006f
/** 屏幕 sRGB 到 WS2812 的轻量校准；后续按实物观感微调这几项即可 */
#define WS2812_COLOR_GAMMA           1.55f
#define WS2812_RED_GAIN              1.00f
#define WS2812_GREEN_GAIN            0.72f
#define WS2812_BLUE_GAIN             0.82f
/** 暖琥珀基准色 */
#define WS2812_WARM_R  255U
#define WS2812_WARM_G  88U
#define WS2812_WARM_B  10U

/** 单帧渲染参数 */
typedef struct {
    float pixel_scale[NANTUO_WS2812_LED_COUNT]; /**< 每颗灯 0~1 独立亮度 */
} ws2812_frame_t;

/**
 * 四个主呼吸点：1-based 的 4/8/12/16 号灯。
 */
static const uint8_t WS2812_FIELD_ANCHORS[4] = {3, 7, 11, 15};

static led_strip_handle_t s_strip;
static bool s_started;
static portMUX_TYPE s_config_lock = portMUX_INITIALIZER_UNLOCKED;
static nantuo_ws2812_breath_config_t s_breath_config = {
    .red = WS2812_WARM_R,
    .green = WS2812_WARM_G,
    .blue = WS2812_WARM_B,
    .dark_red = WS2812_WARM_R,
    .dark_green = WS2812_WARM_G,
    .dark_blue = WS2812_WARM_B,
    .intensity = WS2812_DEFAULT_INTENSITY,
    .dark_intensity = WS2812_DEFAULT_INTENSITY * WS2812_AMBIENT_MIN_RATIO,
    .period_ms = WS2812_DEFAULT_PERIOD_MS,
};
static bool s_has_last_rgb_frame;
static uint8_t s_last_red[NANTUO_WS2812_LED_COUNT];
static uint8_t s_last_green[NANTUO_WS2812_LED_COUNT];
static uint8_t s_last_blue[NANTUO_WS2812_LED_COUNT];

/** 初始化 RMT + WS2812 灯条 */
static esp_err_t ws2812_init_strip(void)
{
    esp_err_t err;
    led_strip_config_t strip_config = {
        .strip_gpio_num = NANTUO_WS2812_GPIO,
        .max_leds = NANTUO_WS2812_LED_COUNT,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };

    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = NANTUO_WS2812_RMT_RES_HZ,
        .mem_block_symbols = 0,
        .flags.with_dma = false,
    };

    err = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip);
    if (err != ESP_OK) {
        return err;
    }

    /*
     * GPIO37 直连 WS2812 DIN 且没有串 330Ω 时，强驱动边沿容易振铃。
     * 降低 drive capability 不能替代电阻/电平转换，但能减少偶发 green byte 误读。
     */
    err = gpio_set_drive_capability(NANTUO_WS2812_GPIO, NANTUO_WS2812_DRIVE_CAP);
    if (err != ESP_OK) {
        return err;
    }
    return ESP_OK;
}

/** 限幅 */
static float ws2812_clampf(float value, float lo, float hi)
{
    if (value < lo) {
        return lo;
    }
    if (value > hi) {
        return hi;
    }
    return value;
}

static uint32_t ws2812_clamp_period_ms(uint32_t period_ms)
{
    if (period_ms < WS2812_PERIOD_MIN_MS) {
        return WS2812_PERIOD_MIN_MS;
    }
    if (period_ms > WS2812_PERIOD_MAX_MS) {
        return WS2812_PERIOD_MAX_MS;
    }
    return period_ms;
}

static void ws2812_copy_config(nantuo_ws2812_breath_config_t *out)
{
    taskENTER_CRITICAL(&s_config_lock);
    *out = s_breath_config;
    taskEXIT_CRITICAL(&s_config_lock);
}

static void ws2812_load_config_from_nvs(void)
{
    nvs_handle_t handle;
    nantuo_ws2812_breath_config_t stored;
    size_t size = sizeof(stored);
    esp_err_t err;

    err = nvs_open(WS2812_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return;
    }

    err = nvs_get_blob(handle, WS2812_NVS_BREATH_KEY, &stored, &size);
    nvs_close(handle);
    if (err != ESP_OK || size != sizeof(stored)) {
        return;
    }

    stored.intensity = ws2812_clampf(stored.intensity, 0.0f, 1.0f);
    stored.dark_intensity = ws2812_clampf(stored.dark_intensity,
                                          0.0f,
                                          stored.intensity);
    stored.period_ms = ws2812_clamp_period_ms(stored.period_ms);

    taskENTER_CRITICAL(&s_config_lock);
    s_breath_config = stored;
    taskEXIT_CRITICAL(&s_config_lock);

    ESP_LOGI(TAG,
             "loaded breath config from NVS bright=#%02x%02x%02x dark=#%02x%02x%02x intensity=%.3f dark_intensity=%.3f period=%ums",
             (unsigned)stored.red,
             (unsigned)stored.green,
             (unsigned)stored.blue,
             (unsigned)stored.dark_red,
             (unsigned)stored.dark_green,
             (unsigned)stored.dark_blue,
             (double)stored.intensity,
             (double)stored.dark_intensity,
             (unsigned)stored.period_ms);
}

static void ws2812_save_config_to_nvs(const nantuo_ws2812_breath_config_t *config)
{
    nvs_handle_t handle;
    esp_err_t err;

    if (config == NULL) {
        return;
    }

    err = nvs_open(WS2812_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "open NVS failed: %s", esp_err_to_name(err));
        return;
    }

    err = nvs_set_blob(handle, WS2812_NVS_BREATH_KEY, config, sizeof(*config));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "save breath config failed: %s", esp_err_to_name(err));
    }
}

static bool ws2812_parse_hex_color(const char *command,
                                   uint8_t *red,
                                   uint8_t *green,
                                   uint8_t *blue)
{
    const char *hash = strchr(command, '#');
    char hex[7];
    unsigned long value;
    char *end = NULL;

    if (hash == NULL || strlen(hash + 1) < 6) {
        return false;
    }
    memcpy(hex, hash + 1, 6);
    hex[6] = '\0';
    value = strtoul(hex, &end, 16);
    if (end == NULL || *end != '\0') {
        return false;
    }
    *red = (uint8_t)((value >> 16) & 0xFFU);
    *green = (uint8_t)((value >> 8) & 0xFFU);
    *blue = (uint8_t)(value & 0xFFU);
    return true;
}

static const char *ws2812_skip_value_separators(const char *pos)
{
    while (*pos == ' ' || *pos == '\t' || *pos == '"' || *pos == '\'' ||
           *pos == ':' || *pos == '=') {
        pos++;
    }
    return pos;
}

static bool ws2812_parse_hex_color_after_key(const char *command,
                                             const char *key,
                                             uint8_t *red,
                                             uint8_t *green,
                                             uint8_t *blue)
{
    const char *pos = strstr(command, key);
    char hex[7];
    unsigned long value;
    char *end = NULL;

    if (pos == NULL) {
        return false;
    }
    pos += strlen(key);
    pos = ws2812_skip_value_separators(pos);
    if (*pos != '#' || strlen(pos + 1) < 6) {
        return false;
    }
    memcpy(hex, pos + 1, 6);
    hex[6] = '\0';
    value = strtoul(hex, &end, 16);
    if (end == NULL || *end != '\0') {
        return false;
    }
    *red = (uint8_t)((value >> 16) & 0xFFU);
    *green = (uint8_t)((value >> 8) & 0xFFU);
    *blue = (uint8_t)(value & 0xFFU);
    return true;
}

static bool ws2812_parse_float_after_key(const char *command,
                                         const char *key,
                                         float *value)
{
    const char *pos = strstr(command, key);
    char *end = NULL;
    float parsed;

    if (pos == NULL) {
        return false;
    }
    pos += strlen(key);
    pos = ws2812_skip_value_separators(pos);
    parsed = strtof(pos, &end);
    if (end == pos) {
        return false;
    }
    *value = parsed;
    return true;
}

static bool ws2812_parse_uint_after_key(const char *command,
                                        const char *key,
                                        uint32_t *value)
{
    const char *pos = strstr(command, key);
    char *end = NULL;
    unsigned long parsed;

    if (pos == NULL) {
        return false;
    }
    pos += strlen(key);
    pos = ws2812_skip_value_separators(pos);
    parsed = strtoul(pos, &end, 10);
    if (end == pos) {
        return false;
    }
    *value = (uint32_t)parsed;
    return true;
}

/** 线性插值 */
static float ws2812_lerp(float a, float b, float t)
{
    return a + (b - a) * ws2812_clampf(t, 0.0f, 1.0f);
}

/** 更柔和的 S 曲线，接近 Shader 里常见的 smootherstep */
static float ws2812_smootherstep(float t)
{
    t = ws2812_clampf(t, 0.0f, 1.0f);
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

/** 感知亮度校正：默认环境光很低，保持近线性才能保留 1~8 档细节 */
static float ws2812_gamma(float scale)
{
    scale = ws2812_clampf(scale, 0.0f, 1.0f);
    return powf(scale, 1.05f);
}

static uint8_t ws2812_calibrated_channel(float color,
                                         float brightness,
                                         float gain)
{
    float normalized = ws2812_clampf(color / 255.0f, 0.0f, 1.0f);
    float corrected = powf(normalized, WS2812_COLOR_GAMMA) * brightness * gain;

    corrected = ws2812_clampf(corrected, 0.0f, 1.0f);
    return (uint8_t)(corrected * 255.0f + 0.5f);
}

/** 0~1 亮度系数 → 暗色/亮色插值 RGB */
static void ws2812_scale_to_rgb(float scale,
                                const nantuo_ws2812_breath_config_t *config,
                                uint8_t *red,
                                uint8_t *green,
                                uint8_t *blue)
{
    float corrected;
    float t;
    float peak;
    float floor;
    float denom;
    float color_red;
    float color_green;
    float color_blue;

    if (scale <= WS2812_IGNITION_CUTOFF) {
        *red = 0;
        *green = 0;
        *blue = 0;
        return;
    }
    peak = config->intensity;
    floor = config->dark_intensity;
    denom = peak - floor;
    if (denom <= 0.001f) {
        t = 1.0f;
    } else {
        t = ws2812_clampf((scale - floor) / denom, 0.0f, 1.0f);
    }
    corrected = ws2812_gamma(scale);
    color_red = ws2812_lerp((float)config->dark_red, (float)config->red, t);
    color_green = ws2812_lerp((float)config->dark_green, (float)config->green, t);
    color_blue = ws2812_lerp((float)config->dark_blue, (float)config->blue, t);

    *red = ws2812_calibrated_channel(color_red, corrected, WS2812_RED_GAIN);
    *green = ws2812_calibrated_channel(color_green, corrected, WS2812_GREEN_GAIN);
    *blue = ws2812_calibrated_channel(color_blue, corrected, WS2812_BLUE_GAIN);
}

/**
 * 四点呼吸：
 * - 借鉴 FastLED beatsin 的连续正弦亮度；
 * - 只保留 4 个锚点，其他灯完全关闭；
 * - 锚点在暗色/亮色之间平滑过渡，色相完全由配置决定。
 */
static ws2812_frame_t ws2812_ring_breath_frame(uint32_t tick_ms)
{
    ws2812_frame_t frame;
    nantuo_ws2812_breath_config_t config;
    float min_scale;
    float max_scale;
    float cycle;
    float breath;
    float shaped;
    float energy;

    ws2812_copy_config(&config);
    min_scale = config.dark_intensity;
    max_scale = config.intensity;
    cycle = (float)(tick_ms % config.period_ms) / (float)config.period_ms;
    breath = 0.5f - 0.5f * cosf(cycle * 2.0f * (float)M_PI);
    shaped = ws2812_smootherstep(breath);
    energy = ws2812_lerp(min_scale,
                         max_scale,
                         shaped);

    for (int i = 0; i < NANTUO_WS2812_LED_COUNT; i++) {
        frame.pixel_scale[i] = 0.0f;
    }

    for (int a = 0; a < 4; a++) {
        uint8_t index = WS2812_FIELD_ANCHORS[a];
        float phase_offset = 0.018f * (float)a;
        float local_cycle = cycle + phase_offset;
        float local_breath = 0.5f - 0.5f * cosf(local_cycle * 2.0f * (float)M_PI);
        float local = ws2812_lerp(min_scale,
                                  max_scale,
                                  ws2812_smootherstep(local_breath));
        frame.pixel_scale[index] = ws2812_clampf((energy * 0.72f + local * 0.28f),
                                                 min_scale,
                                                 max_scale);
    }

    return frame;
}

/**
 * 计算当前帧：脉冲/高亮走全条直驱；待机/低亮走分布呼吸。
 */
static ws2812_frame_t ws2812_compute_frame(uint32_t tick_ms)
{
    ws2812_frame_t frame;
    uint8_t raw;

    raw = nantuo_lamp_get_brightness();

    /* 摇晃脉冲：16 灯全亮，直接跟引擎亮度 */
    if (nantuo_lamp_bright_pulse_active()) {
        for (int i = 0; i < NANTUO_WS2812_LED_COUNT; i++) {
            frame.pixel_scale[i] = (float)raw / 255.0f;
        }
        return frame;
    }

    /* 仪式/传灯等高亮：全条常亮 */
    if (raw >= 240) {
        for (int i = 0; i < NANTUO_WS2812_LED_COUNT; i++) {
            frame.pixel_scale[i] = 1.0f;
        }
        return frame;
    }

    /* 默认：4 个锚点固定低亮呼吸，不随 Hub 状态放大。 */
    return ws2812_ring_breath_frame(tick_ms);
}

/** 熄灭灯条 */
static esp_err_t ws2812_turn_off(void)
{
    esp_err_t err = led_strip_clear(s_strip);
    if (err != ESP_OK) {
        return err;
    }
    err = led_strip_refresh(s_strip);
    if (err != ESP_OK) {
        return err;
    }
    for (int i = 0; i < NANTUO_WS2812_LED_COUNT; i++) {
        s_last_red[i] = 0;
        s_last_green[i] = 0;
        s_last_blue[i] = 0;
    }
    s_has_last_rgb_frame = true;

    /*
     * WS2812 的 GRB 数据中 green byte 最先到达。点火低亮阶段若偶发
     * 信号误码，会表现为一颗灯闪绿；紧跟一次同帧刷新可快速覆盖错帧。
     */
    return led_strip_refresh(s_strip);
}

/** 按帧参数渲染：每帧显式写满 16 颗，避免残留像素或错色闪烁 */
static esp_err_t ws2812_show_frame(const ws2812_frame_t *frame)
{
    uint8_t red[NANTUO_WS2812_LED_COUNT];
    uint8_t green[NANTUO_WS2812_LED_COUNT];
    uint8_t blue[NANTUO_WS2812_LED_COUNT];
    nantuo_ws2812_breath_config_t config;
    esp_err_t err;
    bool changed = !s_has_last_rgb_frame;

    ws2812_copy_config(&config);
    for (int i = 0; i < NANTUO_WS2812_LED_COUNT; i++) {
        ws2812_scale_to_rgb(frame->pixel_scale[i], &config, &red[i], &green[i], &blue[i]);
        if (red[i] != s_last_red[i] ||
            green[i] != s_last_green[i] ||
            blue[i] != s_last_blue[i]) {
            changed = true;
        }
    }

    if (!changed) {
        return ESP_OK;
    }

    for (int i = 0; i < NANTUO_WS2812_LED_COUNT; i++) {
        err = led_strip_set_pixel(s_strip, i, red[i], green[i], blue[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    err = led_strip_refresh(s_strip);
    if (err != ESP_OK) {
        return err;
    }

    for (int i = 0; i < NANTUO_WS2812_LED_COUNT; i++) {
        s_last_red[i] = red[i];
        s_last_green[i] = green[i];
        s_last_blue[i] = blue[i];
    }
    s_has_last_rgb_frame = true;
    return ESP_OK;
}

void nantuo_ws2812_get_breath_config(nantuo_ws2812_breath_config_t *config)
{
    if (config == NULL) {
        return;
    }
    ws2812_copy_config(config);
}

void nantuo_ws2812_set_breath_config(const nantuo_ws2812_breath_config_t *config)
{
    nantuo_ws2812_breath_config_t next;

    if (config == NULL) {
        return;
    }

    next = *config;
    next.intensity = ws2812_clampf(next.intensity, 0.0f, 1.0f);
    next.dark_intensity = ws2812_clampf(next.dark_intensity, 0.0f, next.intensity);
    next.period_ms = ws2812_clamp_period_ms(next.period_ms);

    taskENTER_CRITICAL(&s_config_lock);
    s_breath_config = next;
    taskEXIT_CRITICAL(&s_config_lock);
    ws2812_save_config_to_nvs(&next);

    s_has_last_rgb_frame = false;
    ESP_LOGI(TAG,
             "breath config bright=#%02x%02x%02x dark=#%02x%02x%02x intensity=%.3f dark_intensity=%.3f period=%ums",
             (unsigned)next.red,
             (unsigned)next.green,
             (unsigned)next.blue,
             (unsigned)next.dark_red,
             (unsigned)next.dark_green,
             (unsigned)next.dark_blue,
             (double)next.intensity,
             (double)next.dark_intensity,
             (unsigned)next.period_ms);
}

void nantuo_ws2812_reset_breath_config(void)
{
    nantuo_ws2812_breath_config_t defaults = {
        .red = WS2812_WARM_R,
        .green = WS2812_WARM_G,
        .blue = WS2812_WARM_B,
        .dark_red = WS2812_WARM_R,
        .dark_green = WS2812_WARM_G,
        .dark_blue = WS2812_WARM_B,
        .intensity = WS2812_DEFAULT_INTENSITY,
        .dark_intensity = WS2812_DEFAULT_INTENSITY * WS2812_AMBIENT_MIN_RATIO,
        .period_ms = WS2812_DEFAULT_PERIOD_MS,
    };

    nantuo_ws2812_set_breath_config(&defaults);
}

bool nantuo_ws2812_handle_command(const char *command)
{
    nantuo_ws2812_breath_config_t config;
    float intensity;
    float dark_intensity;
    uint32_t period_ms;
    bool bright_color_set;
    bool dark_color_set;
    uint8_t red;
    uint8_t green;
    uint8_t blue;

    if (command == NULL) {
        return false;
    }

    if (strcmp(command, "led_reset") == 0 ||
        strcmp(command, "led=reset") == 0 ||
        strcmp(command, "{\"led\":\"reset\"}") == 0) {
        nantuo_ws2812_reset_breath_config();
        return true;
    }

    if (strstr(command, "led") == NULL &&
        strstr(command, "color") == NULL &&
        strstr(command, "bright_color") == NULL &&
        strstr(command, "dark_color") == NULL &&
        strstr(command, "intensity") == NULL &&
        strstr(command, "period") == NULL) {
        return false;
    }

    ws2812_copy_config(&config);
    bright_color_set =
        ws2812_parse_hex_color_after_key(command, "bright_color",
                                         &config.red,
                                         &config.green,
                                         &config.blue) ||
        ws2812_parse_hex_color_after_key(command, "color_bright",
                                         &config.red,
                                         &config.green,
                                         &config.blue) ||
        ws2812_parse_hex_color_after_key(command, "high_color",
                                         &config.red,
                                         &config.green,
                                         &config.blue);

    dark_color_set =
        ws2812_parse_hex_color_after_key(command, "dark_color",
                                         &config.dark_red,
                                         &config.dark_green,
                                         &config.dark_blue) ||
        ws2812_parse_hex_color_after_key(command, "color_dark",
                                         &config.dark_red,
                                         &config.dark_green,
                                         &config.dark_blue) ||
        ws2812_parse_hex_color_after_key(command, "low_color",
                                         &config.dark_red,
                                         &config.dark_green,
                                         &config.dark_blue) ||
        ws2812_parse_hex_color_after_key(command, "ambient_color",
                                         &config.dark_red,
                                         &config.dark_green,
                                         &config.dark_blue);

    if (!bright_color_set && !dark_color_set &&
        ws2812_parse_hex_color(command, &red, &green, &blue)) {
        config.red = red;
        config.green = green;
        config.blue = blue;
        config.dark_red = red;
        config.dark_green = green;
        config.dark_blue = blue;
    }

    if (ws2812_parse_float_after_key(command, "intensity", &intensity) ||
        ws2812_parse_float_after_key(command, "brightness", &intensity) ||
        ws2812_parse_float_after_key(command, "strength", &intensity)) {
        if (intensity > 1.0f) {
            if (intensity <= 100.0f) {
                intensity /= 100.0f;
            } else {
                intensity /= 255.0f;
            }
        }
        config.intensity = intensity;
        if (!ws2812_parse_float_after_key(command, "dark_intensity", &dark_intensity) &&
            !ws2812_parse_float_after_key(command, "low_intensity", &dark_intensity) &&
            !ws2812_parse_float_after_key(command, "ambient_intensity", &dark_intensity)) {
            config.dark_intensity = intensity * WS2812_AMBIENT_MIN_RATIO;
        }
    }

    if (ws2812_parse_float_after_key(command, "dark_intensity", &dark_intensity) ||
        ws2812_parse_float_after_key(command, "low_intensity", &dark_intensity) ||
        ws2812_parse_float_after_key(command, "ambient_intensity", &dark_intensity)) {
        if (dark_intensity > 1.0f) {
            if (dark_intensity <= 100.0f) {
                dark_intensity /= 100.0f;
            } else {
                dark_intensity /= 255.0f;
            }
        }
        config.dark_intensity = dark_intensity;
    }

    if (ws2812_parse_uint_after_key(command, "period_ms", &period_ms) ||
        ws2812_parse_uint_after_key(command, "period", &period_ms) ||
        ws2812_parse_uint_after_key(command, "interval", &period_ms)) {
        config.period_ms = period_ms;
    }

    nantuo_ws2812_set_breath_config(&config);
    return true;
}

/** 灯条渲染任务：仅在难陀模式下点亮 */
static void ws2812_lamp_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "lamp task started, gpio=%d leds=%d distributed breath",
             (int)NANTUO_WS2812_GPIO, NANTUO_WS2812_LED_COUNT);

    while (true) {
        uint32_t delay_ms;
        uint32_t tick_ms;
        ws2812_frame_t frame;
        esp_err_t err;

        if (!app_mode_is_digital_heritage()) {
            err = ws2812_turn_off();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "turn off failed: %s", esp_err_to_name(err));
            }
            vTaskDelay(pdMS_TO_TICKS(NANTUO_WS2812_IDLE_POLL_MS));
            continue;
        }

        tick_ms = (uint32_t)(esp_timer_get_time() / 1000);
        frame = ws2812_compute_frame(tick_ms);
        err = ws2812_show_frame(&frame);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "render failed: %s", esp_err_to_name(err));
        }

        delay_ms = nantuo_lamp_shake_effects_active() ?
                   NANTUO_WS2812_PULSE_FRAME_MS : NANTUO_WS2812_FRAME_MS;
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

esp_err_t nantuo_ws2812_start(void)
{
    esp_err_t err;

    if (s_started) {
        return ESP_OK;
    }

    ws2812_load_config_from_nvs();

    err = ws2812_init_strip();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WS2812 init failed on GPIO%d: %s",
                 (int)NANTUO_WS2812_GPIO, esp_err_to_name(err));
        return err;
    }

    err = ws2812_turn_off();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "clear failed: %s", esp_err_to_name(err));
        return err;
    }

    if (xTaskCreate(ws2812_lamp_task,
                    "ws2812_lamp",
                    4096,
                    NULL,
                    5,
                    NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create lamp task");
        return ESP_ERR_NO_MEM;
    }

    s_started = true;
    ESP_LOGI(TAG, "WS2812B ready: GPIO%d x %d (distributed ring breath)",
             (int)NANTUO_WS2812_GPIO, NANTUO_WS2812_LED_COUNT);
    return ESP_OK;
}
