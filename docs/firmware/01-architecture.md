# ESP32-C5 语音助手 - 固件架构

- 日期：2026-07-11
- 目标：基于现成硬件从零实现方案 A 云端半双工语音助手
- 平台：ESP-IDF 6.0.2，C++17，FreeRTOS

## 设计原则

1. BSP 只描述硬件，不包含业务状态。
2. 音频采集、播放和网络传输通过有界队列隔离。
3. 所有业务状态切换只在主事件循环执行。
4. 半双工强约束：录音上传与扬声器播放不能同时运行。
5. DMA缓冲放内部RAM，大块UI资源、图片和非DMA队列优先放PSRAM。
6. 云端协议使用抽象接口，避免绑定单一服务商。

## 目录建议

```text
main/
  app/                 主事件循环、状态机、错误恢复
  bsp/                 GPIO、I2C、I2S、LCD、背光、按键、LED
  audio/               Codec、采集、播放、Opus、提示音
  wake_word/           WakeNet9s适配
  display/             LVGL界面、表情、字幕、状态栏
  network/             Wi-Fi配网、TLS、时间同步、重连
  protocol/            云端协议抽象及具体实现
  storage/             NVS、资源分区、配置
  ota/                 双分区HTTPS OTA
  diagnostics/         日志、堆监控、任务水位和故障记录
```

## 核心接口

```cpp
class AudioCodec {
public:
    virtual bool Start() = 0;
    virtual bool Read(int16_t* pcm, size_t samples) = 0;
    virtual bool Write(const int16_t* pcm, size_t samples) = 0;
    virtual void SetVolume(uint8_t percent) = 0;
};

class AssistantProtocol {
public:
    virtual bool Connect() = 0;
    virtual bool OpenSession() = 0;
    virtual bool SendAudio(const uint8_t* opus, size_t size) = 0;
    virtual bool EndAudio() = 0;
    virtual void CloseSession() = 0;
};
```

## FreeRTOS任务

| 任务 | 初始优先级 | 职责 | 关键约束 |
|---|---:|---|---|
| app_main_loop | 8 | 状态机和事件分发 | 唯一业务状态写入者 |
| audio_capture | 10 | I2S录音、唤醒词或编码输入 | 使用内部DMA缓冲，不阻塞网络 |
| audio_playback | 9 | 解码PCM写入I2S | 播放期间关闭采集上传 |
| audio_codec | 7 | Opus编解码 | 使用有界队列 |
| protocol_rx | 7 | 接收控制消息和TTS音频 | 不直接操作UI和状态机 |
| ui | 5 | LVGL刷新 | 所有LVGL调用串行化 |
| housekeeping | 2 | 堆监控、状态栏、健康检查 | 不影响音频实时性 |

优先级只是首版起点，必须通过运行时统计调整。

## 半双工状态机

```text
BOOT
  -> PROVISIONING       无Wi-Fi配置
  -> CONNECTING         有Wi-Fi配置

CONNECTING
  -> IDLE               网络和协议就绪
  -> ERROR              连接失败

IDLE
  -> LISTENING          唤醒词或按键触发

LISTENING
  -> THINKING           VAD结束或用户手动结束
  -> IDLE               取消/超时

THINKING
  -> SPEAKING           收到首个TTS音频包
  -> ERROR              云端失败

SPEAKING
  -> IDLE               播放完成
  -> IDLE               按键中断

任意可恢复状态
  -> OTA                 固件升级
  -> ERROR               不可继续的模块错误
```

## 音频参数基线

- 麦克风和唤醒词内部流：16 kHz、单声道、16 bit PCM。
- Opus帧长：首版20 ms或40 ms，联调后按网络延迟调整。
- TTS播放：优先要求云端输出设备原生采样率；否则在设备端重采样。
- 待命：只运行唤醒词检测，不上传音频。
- 聆听：停止唤醒词检测，启动Opus上传。
- 播放：停止采集上传，清空残留录音帧后再使能功放。

## 内存策略

| 数据 | 内存区域 |
|---|---|
| I2S DMA描述符和DMA缓冲 | 内部RAM |
| LCD传输DMA缓冲 | 内部RAM |
| WakeNet工作区 | 按ESP-SR要求分配 |
| LVGL图片、字体和资源缓存 | PSRAM |
| TTS压缩包队列 | PSRAM优先 |
| PCM实时帧和短队列 | 内部RAM优先 |
| TLS大块临时缓冲 | PSRAM，按组件能力配置 |

## 云端协议最小契约

控制消息使用JSON，音频使用二进制Opus帧。至少支持：

- `hello/auth`：设备标识、固件版本和能力。
- `session.start`：开始一次对话。
- `audio.frame`：设备上传Opus帧。
- `audio.end`：用户输入结束。
- `stt.partial/final`：识别字幕。
- `llm.text`：助手文本。
- `tts.start/frame/end`：流式播放。
- `error`：可显示、可重试的错误。
- `session.close`：结束会话并回到待命。

## 故障恢复

- Wi-Fi断开：关闭会话、停止录音和播放，回到CONNECTING。
- 云端超时：停止录音，显示错误，延时后回到IDLE。
- 音频队列溢出：丢弃最旧的压缩包，不允许无限增长。
- LCD异常：语音功能继续运行，显示降级为日志和LED。
- PSRAM初始化失败：禁止进入完整UI，输出明确错误，不静默继续。
