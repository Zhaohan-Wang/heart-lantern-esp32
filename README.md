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

当前编译 `main.c`，启动后初始化板级 GPIO、I2C 和 MPU6050，并通过串口输出 IMU 采样日志；当前阶段不初始化灯条 / Wi-Fi。

```bash
cd /path/to/heart-lantern-esp32
export IDF_PATH="$HOME/esp/v5.4.1/esp-idf"   # 如果 ESP-IDF 不在默认位置，请改这里
export ESPPORT=/dev/cu.usbserial-XXXX        # 按 ls /dev/cu.usbserial* 修改
zsh scripts/build_with_progress.zsh --flash
```

串口监视：

```bash
idf.py -p $ESPPORT monitor
```

预期日志：`heart-lantern-esp32 boot`、芯片信息、板级引脚状态，以及周期性的 `IMU ax=...` 采样日志。

## 阶段 1：接入完整大报恩寺功能（待做）

1. 在 `main/CMakeLists.txt` 取消 heritage 源文件注释
2. 恢复 `main/idf_component.yml`（`led_strip`）
3. 添加 `heritage_ui_stub.c`（无 OLED 时的 UI 空实现）
4. 改写 `main.c`：只调用 `digital_heritage_init()` + `nantuo_ws2812_start()`

Wi-Fi 凭据：默认开发环境配置在 `main/heritage/digital_heritage_credentials.h`。如需临时使用其他网络，可复制 `main/heritage/digital_heritage_credentials.h.example` 到本机独立分支或本地改动中再调整。

数字人研究院环境：灯控 Wi-Fi SSID 使用 `omelette`，MQTT / 控制端地址使用 `mqtt://192.168.31.12:1883`。该配置随开发固件提交，方便现场直接构建烧录。

## 已从原项目移除、本板不需要

- 键盘矩阵 / MIDI / Roco Helper
- OLED / LVGL
- 编码器 / 多模式切换
