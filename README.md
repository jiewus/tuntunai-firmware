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

## 囤囤管家平台接入

固件内置 MCP 工具 `self.tuntun.bind_device`。用户唤醒小智并明确提出“绑定囤囤管家平台”
后，设备会向 `CONFIG_TUNTUN_API_URL` 申请一次性绑定码，并在圆屏上持续显示。用户在囤囤管家
网页端的设备页面输入该绑定码后，设备会自动完成绑定并保存设备访问凭据。

再次要求绑定时，已保存完整设备凭据的设备会直接回答“设备已绑定”，不会重复生成绑定码。用户
要求解绑时，`self.tuntun.unbind_device` 只会提示登录囤囤管家后台完成解绑，固件不会执行语音解绑
或清除本地凭据。

设备绑定后可以通过 `self.tuntun.set_weather_location` 响应“把天气城市设置为上海市松江区”等
语音指令，也可以通过 `self.tuntun.set_weather_ip_auto` 响应“天气改为 IP 自动定位”。固定模式下
设备只提交城市或区县名称，行政区编码由后端通过高德接口解析并保存。

绑定完成后，设备会在屏保可见期间通过设备访问凭据同步天气。首次进入屏保时立即检查天气，
成功数据在运行内存中缓存 30 分钟；网络临时断开或接口失败时继续显示最近一次成功天气。
天气不会写入 NVS，避免周期刷新增加 Flash 擦写。备忘录目前仍显示本地占位内容。

默认平台地址为 `https://api.tuntun.life`，可通过 `idf.py menuconfig` 中的
`TuntunLife API URL` 修改。完整协议和状态处理见
[`docs/后端API接入说明.md`](docs/后端API接入说明.md)。
