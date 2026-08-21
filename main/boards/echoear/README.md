# EchoEar ESP32-S3 板型

本目录是 TuntunLife 固件对 EchoEar 的板级适配，目标硬件为
`ESP32-S3-WROOM-1-N32R16`。适配代码以 TuntunLife 的公共业务、网络、语音和 OTA
实现为准，只复用原 EchoEar 工程中的硬件初始化与驱动参数。

板型标识为 `echoear`，构建产物与其他板型隔离存放，不需要执行
`idf.py set-target`。

## 已适配硬件

| 模块 | 配置 |
| --- | --- |
| 主控 | ESP32-S3，双核 240 MHz |
| Flash | 32 MB，QIO，80 MHz |
| PSRAM | 16 MB Octal PSRAM，80 MHz |
| 显示屏 | ST77916，360 x 360，QSPI |
| 触摸 | CST816S，I2C |
| 音频输出 | ES8311 |
| 音频输入 | ES7210，四麦克风 TDM 输入 |
| 电池检测 | I2C 电量计，地址 `0x55` |
| 网络 | 2.4 GHz Wi-Fi |
| 本地唤醒 | ESP-SR WakeNet，“你好小智” |

固件启动时会通过 I2C 自动识别 PCB V1.0 和 V1.2，并选择对应的音频输入、功放和
LCD 复位引脚。无法识别版本时按 V1.0 引脚配置运行。

## 主要功能

- Wi-Fi 配网和自动重连。
- 360 x 360 圆屏界面、表盘、状态栏和触摸交互。
- 本地唤醒、语音采集、语音播放和按住说话模式。
- TuntunLife 设备绑定、天气、自定义提醒、主动通知和动态 MCP 工具。
- TuntunLife 自有 OTA 检查；检查失败时跳过，不阻塞设备启动。
- PCB V1.0/V1.2 自动适配。

## 目录说明

```text
main/boards/echoear/
├── CMakeLists.txt           板型组件、源码和依赖注册
├── config.json              芯片目标、板型名称和资源配置
├── config.h                 GPIO、采样率和屏幕参数
├── echoear.cc               板级初始化和交互逻辑
├── echoear_audio_codec.*    ES8311 + ES7210 音频实现
├── echoear_lcd_init.h       ST77916 初始化命令
├── idf_component.yml        板型专用组件依赖
├── sdkconfig.defaults       Flash、PSRAM、LVGL 和 WakeNet 配置
└── assets/                  该板型的资源分区输入文件
```

## 开发环境

项目要求使用 **ESP-IDF 6.0.2**。ESP-IDF 5.5.x 与当前组件和结构体不兼容。

进入固件项目根目录并加载 ESP-IDF 环境：

```bash
cd ESP32-C5.Firmware
source ~/.espressif/v6.0.2/esp-idf/export.sh
idf.py --version
```

`idf.py --version` 应输出 `ESP-IDF v6.0.2`。Windows 请使用安装器创建的
“ESP-IDF 6.0.2 PowerShell”；其他平台的环境加载方式参见
[编译和烧录](../../../docs/编译和烧录.md)。

## 编译

从固件项目根目录执行：

```bash
python3 scripts/build.py echoear build
```

调试构建目录为：

```text
build/echoear/debug/
```

常用配置命令：

```bash
python3 scripts/build.py echoear reconfigure
python3 scripts/build.py echoear menuconfig
python3 scripts/build.py echoear fullclean
```

修改 `sdkconfig.defaults` 后，如果已有构建目录仍保留旧配置，先执行 `fullclean`，再重新
`build`。EchoEar 的系统事件任务栈必须保持为 4096；使用 ESP-IDF 默认的 2304 会在
Wi-Fi 扫描完成后触发 `sys_evt` 栈溢出并循环重启。

## 查找串口

macOS：

```bash
ls /dev/cu.usbmodem*
```

Linux：

```bash
ls /dev/ttyACM* /dev/ttyUSB*
```

Windows 可在设备管理器中查看对应的 `COM` 端口。

## 烧录和监听

仅烧录：

```bash
python3 scripts/build.py echoear flash -p /dev/cu.usbmodemXXXX
```

烧录后立即监听：

```bash
python3 scripts/build.py echoear flash-monitor -p /dev/cu.usbmodemXXXX
```

Linux 和 Windows 分别将端口替换为 `/dev/ttyACM0`、`/dev/ttyUSB0` 或 `COM4`。
按 `Ctrl+]` 退出串口监听。

标准烧录会写入以下内容：

- Bootloader
- 分区表
- OTA 初始数据
- 应用固件
- EchoEar 资源分区

默认烧录参数不擦除 NVS，因此通常会保留 Wi-Fi 和设备设置。

## 生成完整发布固件

生成未启用 Secure Boot、Flash Encryption 和防降级的发布固件：

```bash
python3 scripts/release.py echoear
```

完整合并镜像输出到：

```text
releases/EchoEar/tuntun-binary.bin
```

该镜像从 Flash 地址 `0x0` 写入：

```bash
esptool --chip esp32s3 --port /dev/cu.usbmodemXXXX --baud 460800 \
  write-flash 0x0 releases/EchoEar/tuntun-binary.bin
```

发布脚本生成的是未安全加固版本，不会写入 eFuse，可正常重复烧录。

## 首次启动

1. 上电后等待设备进入 Wi-Fi 配网模式。
2. 完成 Wi-Fi 配置，设备会自动连接保存的网络。
3. Wi-Fi 连接后，固件请求 TuntunLife OTA/激活接口并获取 MQTT 或 WebSocket 配置。
4. 云端配置成功后，可通过语音指令绑定设备并使用平台功能。

## 常见问题

### Wi-Fi 连接时循环重启

串口出现以下信息表示系统事件任务栈配置过小：

```text
***ERROR*** A stack overflow in task sys_evt has been detected.
```

确认最终配置包含：

```text
CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE=4096
```

然后执行 `fullclean`、重新构建并烧录。

### 串口提示 busy 或无法独占

同一串口只能由一个程序打开。先退出其他 `idf.py monitor`、串口工具或 IDE
监视器，再重新烧录。

### Wi-Fi 已连接，但唤醒后提示连接错误

检查串口中的 OTA/激活请求状态。若接口返回错误，或日志提示“未配置小智 MQTT 服务地址”，
说明 OTA 服务没有为该设备下发 MQTT/WebSocket 配置，需要检查
`https://api.tenclass.net/xiaozhi/ota/` 的响应和设备信息。

### 屏幕正常但没有唤醒响应

确认烧录命令包含资源分区，并检查日志中是否出现：

```text
MODEL_LOADER: Successfully load srmodels
EspWakeWord: Wake word(wn9s_nihaoxiaozhi)
```

只写入应用分区而未写入 `generated_assets.bin` 会导致 WakeNet 模型不可用。

## 相关文档

- [板型移植契约](../README.md)
- [项目编译和烧录](../../../docs/编译和烧录.md)
- [固件架构](../../../docs/firmware/01-architecture.md)
