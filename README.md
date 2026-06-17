# heart-lantern-esp32

大报恩寺（难陀心灯）专用 ESP32 固件，从 `hardware-oled-keyboard` 拆分。

## 目录

```
heart-lantern-esp32/
├── main/
│   ├── main.c              # 应用入口（当前为冒烟测试）
│   └── heritage/           # 从大键盘项目批量复制的灯效 / 联网源码（暂未链接）
│       ├── digital_heritage.c
│       ├── nantuo_lamp.c
│       ├── nantuo_mqtt.c
│       ├── nantuo_ws2812.c
│       └── ...
├── scripts/build_with_progress.zsh
└── sdkconfig.defaults
```

## 阶段 0：验证 ESP32 可烧录（当前）

仅编译 `main.c`，每 5 秒打印 `alive`，不初始化 GPIO / 灯条 / Wi-Fi。

```bash
cd /Users/wangzhaohan/Documents/GitHub/heart-lantern-esp32
export ESP_ROM_ELF_DIR="/Users/wangzhaohan/.espressif/tools/esp-rom-elfs/20241011/"
export ESPPORT=/dev/cu.usbserial-XXXX   # 按 ls /dev/cu.usbserial* 修改
zsh scripts/build_with_progress.zsh --flash
```

串口监视：

```bash
idf.py -p $ESPPORT monitor
```

预期日志：`heart-lantern-esp32 smoke test` + 周期性 `alive`。

## 阶段 1：接入完整大报恩寺功能（待做）

1. 在 `main/CMakeLists.txt` 取消 heritage 源文件注释
2. 恢复 `main/idf_component.yml`（`led_strip`）
3. 添加 `heritage_ui_stub.c`（无 OLED 时的 UI 空实现）
4. 改写 `main.c`：只调用 `digital_heritage_init()` + `nantuo_ws2812_start()`

Wi-Fi 凭据：复制 `main/heritage/digital_heritage_credentials.h.example` 为 `digital_heritage_credentials.h` 并填写。

## 已从原项目移除、本板不需要

- 键盘矩阵 / MIDI / Roco Helper
- OLED / LVGL
- 编码器 / 多模式切换
