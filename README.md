# Movecall Moji2 ESP32-C5 语音助手

本项目基于 `xiaozhi-esp32` v2.2.6 裁剪，仅支持
`movecall-moji2-esp32c5` 开发板。

## 硬件

- ESP32-C5 N16R8
- ES8311 音频 Codec
- ST77916 360x360 QSPI LCD
- 单麦克风、扬声器、WS2812 状态灯

## 构建

要求 ESP-IDF 5.5.2 或更高版本。

```bash
idf.py set-target esp32c5
python scripts/release.py movecall-moji2-esp32c5
```

也可以通过 `idf.py menuconfig` 配置显示样式、资源和调试选项，然后执行：

```bash
idf.py build
idf.py flash monitor
```

硬件和固件设计说明位于 `docs/hardware` 与 `docs/firmware`。
