#ifndef AUDIO_SERVICE_H
#define AUDIO_SERVICE_H

#include <memory>
#include <deque>
#include <condition_variable>
#include <chrono>
#include <mutex>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <esp_timer.h>
#include <model_path.h>
#include "esp_audio_enc.h"
#include "esp_opus_enc.h"
#include "esp_opus_dec.h"
#include "esp_ae_rate_cvt.h"
#include "esp_audio_types.h"

#include "audio_codec.h"
#include "audio_processor.h"
#include "processors/audio_debugger.h"
#include "wake_word.h"
#include "protocol.h"
#include "ogg_demuxer.h"

/**
 * @file audio_service.h
 * @brief 音频采集、唤醒、Opus 编解码、播放和队列调度服务。
 *
 * 上行链路：麦克风 -> 处理/唤醒 -> 编码队列 -> Opus 编码 -> 发送队列 -> 云端。
 * 下行链路：云端 -> 解码队列 -> Opus 解码 -> 播放队列 -> 扬声器。
 *
 * PCM 数据体积大，因此队列深度受到严格限制；压缩后的 Opus 队列可以缓存约
 * 2.4 秒数据。输入、输出和编解码分别运行在独立 FreeRTOS 任务中。
 */

#define OPUS_FRAME_DURATION_MS 60
#define MAX_ENCODE_TASKS_IN_QUEUE 2
#define MAX_PLAYBACK_TASKS_IN_QUEUE 2
#define MAX_DECODE_PACKETS_IN_QUEUE (2400 / OPUS_FRAME_DURATION_MS)
#define MAX_SEND_PACKETS_IN_QUEUE (2400 / OPUS_FRAME_DURATION_MS)
#define AUDIO_TESTING_MAX_DURATION_MS 10000

#define AUDIO_POWER_TIMEOUT_MS 15000
#define AUDIO_POWER_CHECK_INTERVAL_MS 1000

#define AS_EVENT_AUDIO_TESTING_RUNNING      (1 << 0)
#define AS_EVENT_WAKE_WORD_RUNNING          (1 << 1)
#define AS_EVENT_AUDIO_PROCESSOR_RUNNING    (1 << 2)
#define AS_EVENT_PLAYBACK_NOT_EMPTY         (1 << 3)

#define AS_OPUS_GET_FRAME_DRU_ENUM(duration_ms)                   \
    ((duration_ms) == 5 ? ESP_OPUS_ENC_FRAME_DURATION_5_MS :      \
     (duration_ms) == 10 ? ESP_OPUS_ENC_FRAME_DURATION_10_MS :    \
     (duration_ms) == 20 ? ESP_OPUS_ENC_FRAME_DURATION_20_MS :    \
     (duration_ms) == 40 ? ESP_OPUS_ENC_FRAME_DURATION_40_MS :    \
     (duration_ms) == 60 ? ESP_OPUS_ENC_FRAME_DURATION_60_MS :    \
     (duration_ms) == 80 ? ESP_OPUS_ENC_FRAME_DURATION_80_MS :    \
     (duration_ms) == 100 ? ESP_OPUS_ENC_FRAME_DURATION_100_MS :  \
     (duration_ms) == 120 ? ESP_OPUS_ENC_FRAME_DURATION_120_MS : -1)

#define AS_OPUS_ENC_CONFIG() {                                                                                    \
        .sample_rate        = ESP_AUDIO_SAMPLE_RATE_16K,                                                          \
        .channel            = ESP_AUDIO_MONO,                                                                     \
        .bits_per_sample    = ESP_AUDIO_BIT16,                                                                    \
        .bitrate            = ESP_OPUS_BITRATE_AUTO,                                                              \
        .frame_duration     = (esp_opus_enc_frame_duration_t)AS_OPUS_GET_FRAME_DRU_ENUM(OPUS_FRAME_DURATION_MS),  \
        .application_mode   = ESP_OPUS_ENC_APPLICATION_AUDIO,                                                     \
        .complexity         = 0,                                                                                  \
        .enable_fec         = false,                                                                              \
        .enable_dtx         = true,                                                                               \
        .enable_vbr         = true,                                                                               \
    }

/**
 * @brief AudioService 向应用层报告异步事件的回调集合。
 */
struct AudioServiceCallbacks {
    /**
     * 发送队列从空变为非空，通知主任务尽快取包上传。
     */
    std::function<void(void)> on_send_queue_available;
    std::function<void(const std::string&)> on_wake_word_detected;
    std::function<void(bool)> on_vad_change;
    std::function<void(void)> on_audio_testing_queue_full;
};


/**
 * @brief 编解码工作队列中的任务类型。
 */
enum AudioTaskType {
    kAudioTaskTypeEncodeToSendQueue,
    kAudioTaskTypeEncodeToTestingQueue,
    kAudioTaskTypeDecodeToPlaybackQueue,
};

/**
 * @brief 一项待编码或待播放的 PCM 工作。
 */
struct AudioTask {
    AudioTaskType type;
    std::vector<int16_t> pcm;
    uint32_t timestamp = 0;
};

/**
 * @brief 音频流水线累计帧数，用于定位丢帧或队列堵塞。
 */
struct DebugStatistics {
    uint32_t input_count = 0;
    uint32_t decode_count = 0;
    uint32_t encode_count = 0;
    uint32_t playback_count = 0;
};

/**
 * @brief 管理设备完整音频流水线及其后台任务。
 */
class AudioService {
public:
    /**
     * @brief 创建同步对象和定时器，尚未绑定硬件。
     */
    AudioService();
    /**
     * @brief 停止任务并释放 Opus、重采样器和同步资源。
     */
    ~AudioService();

    /**
     * @brief 绑定板级 Codec 并创建唤醒词、处理器和编解码器。
     * @param codec 非空且生命周期覆盖本服务。
     */
    void Initialize(AudioCodec* codec);
    /**
     * @brief 创建输入、输出、Opus 三个后台任务并开始处理。
     */
    void Start();
    /**
     * @brief 请求后台任务停止并唤醒所有正在等待的队列。
     */
    void Stop();
    /**
     * @return 最近一次识别到的唤醒词；尚未识别时为空字符串。
     */
    const std::string& GetLastWakeWord() const;
    /**
     * @brief 查询当前 VAD 判断结果。
     */
    bool IsVoiceDetected() const { return voice_detected_; }
    /**
     * @brief 所有编解码与播放队列为空且无处理任务运行时返回 true。
     */
    bool IsIdle();
    /**
     * @brief 阻塞等待所有待播放 PCM 消耗完，用于重启或切换音频状态。
     */
    void WaitForPlaybackQueueEmpty();
    bool IsWakeWordRunning() const { return xEventGroupGetBits(event_group_) & AS_EVENT_WAKE_WORD_RUNNING; }
    bool IsAudioProcessorRunning() const { return xEventGroupGetBits(event_group_) & AS_EVENT_AUDIO_PROCESSOR_RUNNING; }

    /**
     * @brief 启停本地 WakeNet 检测。
     * @param enable true 开启，false 停止并清缓冲。
     */
    void EnableWakeWordDetection(bool enable);
    /**
     * @brief 启停上传前的语音处理/VAD 流水线。
     */
    void EnableVoiceProcessing(bool enable);
    /**
     * @brief 启停十秒回环录音测试模式。
     */
    void EnableAudioTesting(bool enable);

    /**
     * @brief 设置应用层回调。
     * @param callbacks 回调会被复制保存。
     */
    void SetCallbacks(AudioServiceCallbacks& callbacks);

    /**
     * @brief 将云端 Opus 包加入解码队列。
     * @param packet 数据包所有权。
     * @param wait 队列满时是否等待空间；网络回调通常传 false，避免阻塞收包线程。
     */
    bool PushPacketToDecodeQueue(std::unique_ptr<AudioStreamPacket> packet, bool wait = false);

    /**
     * @brief 取出最早的上行 Opus 包。
     * @return 队列为空时返回 nullptr。
     */
    std::unique_ptr<AudioStreamPacket> PopPacketFromSendQueue();
    /**
     * @brief 解封装并播放内嵌 OGG/Opus 提示音。
     * @param sound 完整 OGG 文件数据。
     */
    void PlaySound(const std::string_view& sound);

    /**
     * @brief 从 Codec 读取并按需重采样到指定参数。
     * @param data 输出 PCM。
     * @param sample_rate 目标采样率 Hz。
     * @param samples 目标单声道样本数。
     */
    bool ReadAudioData(std::vector<int16_t>& data, int sample_rate, int samples);
    /**
     * @brief 销毁并按当前服务端参数重新创建 Opus 解码器。
     */
    void ResetDecoder();
    /**
     * @brief 设置资源分区加载出的 ESP-SR 模型列表；列表所有权仍归资源管理器。
     */
    void SetModelsList(srmodel_list_t* models_list);

private:
    AudioCodec* codec_ = nullptr;
    AudioServiceCallbacks callbacks_;
    std::unique_ptr<AudioProcessor> audio_processor_;
    std::unique_ptr<WakeWord> wake_word_;
    std::unique_ptr<AudioDebugger> audio_debugger_;
    void* opus_encoder_ = nullptr;
    void* opus_decoder_ = nullptr;
    std::mutex decoder_mutex_;
    std::mutex input_resampler_mutex_;
    esp_ae_rate_cvt_handle_t input_resampler_ = nullptr;
    esp_ae_rate_cvt_handle_t output_resampler_ = nullptr;
    
    // 编解码器的当前协商参数和派生缓冲区尺寸。
    int encoder_sample_rate_ = 16000;
    int encoder_duration_ms_ = OPUS_FRAME_DURATION_MS;
    int encoder_frame_size_ = 0;
    int encoder_outbuf_size_ = 0;
    int decoder_sample_rate_ = 0;
    int decoder_duration_ms_ = OPUS_FRAME_DURATION_MS;
    int decoder_frame_size_ = 0;
    DebugStatistics debug_statistics_;
    srmodel_list_t* models_list_ = nullptr;

    EventGroupHandle_t event_group_;

    // 三个后台任务以及它们共享的有界队列。
    TaskHandle_t audio_input_task_handle_ = nullptr;
    TaskHandle_t audio_output_task_handle_ = nullptr;
    TaskHandle_t opus_codec_task_handle_ = nullptr;
    std::mutex audio_queue_mutex_;
    std::condition_variable audio_queue_cv_;
    std::deque<std::unique_ptr<AudioStreamPacket>> audio_decode_queue_;
    std::deque<std::unique_ptr<AudioStreamPacket>> audio_send_queue_;
    std::deque<std::unique_ptr<AudioStreamPacket>> audio_testing_queue_;
    std::deque<std::unique_ptr<AudioTask>> audio_encode_queue_;
    std::deque<std::unique_ptr<AudioTask>> audio_playback_queue_;
    bool wake_word_initialized_ = false;
    bool audio_processor_initialized_ = false;
    bool voice_detected_ = false;
    bool service_stopped_ = true;
    bool audio_input_need_warmup_ = false;

    esp_timer_handle_t audio_power_timer_ = nullptr;
    std::chrono::steady_clock::time_point last_input_time_;
    std::chrono::steady_clock::time_point last_output_time_;

    /**
     * @brief 循环读取麦克风，将数据分发给唤醒词、处理器或测试队列。
     */
    void AudioInputTask();
    /**
     * @brief 循环从播放队列取 PCM 并写入 Codec。
     */
    void AudioOutputTask();
    /**
     * @brief 在同一任务中串行执行 Opus 编码和解码，降低并发内存峰值。
     */
    void OpusCodecTask();
    /**
     * @brief 有界地加入 PCM 编码任务，满时丢弃最旧实时帧避免延迟累积。
     */
    void PushTaskToEncodeQueue(AudioTaskType type, std::vector<int16_t>&& pcm);
    /**
     * @brief 更新下行参数并重建解码器/输出重采样器。
     */
    void SetDecodeSampleRate(int sample_rate, int frame_duration);
    /**
     * @brief 根据最近输入输出时间自动关闭或唤醒 Codec 电源。
     */
    void CheckAndUpdateAudioPowerState();
};

#endif
