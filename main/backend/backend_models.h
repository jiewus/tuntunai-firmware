#ifndef TUNTUN_BACKEND_MODELS_H
#define TUNTUN_BACKEND_MODELS_H

#include <cstdint>
#include <ctime>
#include <string>

/**
 * @file backend_models.h
 * @brief 固件与吞吞生活后端通信时使用的有界业务模型。
 *
 * 本文件只保留设备执行提醒所需字段，不直接复刻后端全部 DTO。这样可以限制
 * ESP32-C5 的常驻 SRAM 占用，并避免将无关管理字段长期保存在设备内存中。
 */

/**
 * @brief 设备事件的稳定协议类型。
 */
enum class BackendEventType : uint8_t {
    Unknown = 0,
    ScreenNotification,
    DataRefresh,
    VoiceReminder,
    ReminderScheduleChanged,
    DeviceConfigChanged,
    ToolManifestChanged,
    WorkflowResult
};

/**
 * @brief MQTT 回调传给后端 Worker 的轻量事件提示。
 *
 * UUID 和类型使用固定数组，确保 FreeRTOS 队列可以按字节复制该结构，且回调
 * 不需要创建长期堆对象。通知不包含提醒正文，正文必须通过认证 HTTPS 获取。
 */
struct BackendEventHint {
    /**
     * @brief 后端设备事件 UUID，包含结尾零字符的最大长度为 37 字节。
     */
    char event_id[37]{};

    /**
     * @brief 事件类型，用于 Worker 决定需要刷新哪类权威数据。
     */
    BackendEventType type = BackendEventType::Unknown;

    /**
     * @brief 对应业务资源的单调版本号，用于诊断重复或乱序通知。
     */
    uint64_t revision = 0;
};

/**
 * @brief 后端下发给设备的独立业务 MQTT 连接配置。
 */
struct BackendMqttConfig {
    /**
     * @brief 后端总开关；false 时设备销毁业务 MQTT，但保留 HTTPS 功能。
     */
    bool enabled = false;
    /**
     * @brief 是否必须使用 TLS；当前固件只接受 true。
     */
    bool tls = true;
    /**
     * @brief Broker DNS 主机名，不包含协议和端口。
     */
    std::string host;
    /**
     * @brief Broker TCP 端口；当前网络抽象使用 8883 识别 TLS。
     */
    int port = 0;
    /**
     * @brief Broker 会话使用的设备唯一客户端标识。
     */
    std::string client_id;
    /**
     * @brief Broker HTTP 认证映射的设备用户名。
     */
    std::string username;
    /**
     * @brief 后端临时签发的 Broker 密码，只保存在 RAM 中。
     */
    std::string password;
    /**
     * @brief 当前设备唯一允许订阅的业务事件主题。
     */
    std::string event_topic;
    /**
     * @brief 订阅服务质量等级，固件限制在 0 至 1。
     */
    int qos = 1;
};

/**
 * @brief 固件本地保存的一条未来提醒计划。
 */
struct BackendReminderPlan {
    /**
     * @brief 提醒 UUID；确认、音频授权和版本匹配均使用该标识。
     */
    std::string reminder_id;

    /**
     * @brief 提醒权威版本；旧版本绝不能覆盖或确认新版本。
     */
    uint64_t version = 0;

    /**
     * @brief 设备开始提醒的 UTC Unix 秒时间戳。
     */
    std::time_t trigger_at_utc = 0;

    /**
     * @brief 超过该 UTC Unix 秒时间戳后必须放弃提醒。
     */
    std::time_t expires_at_utc = 0;

    /**
     * @brief 屏幕展示文本；固件解析时限制 UTF-8 字节数，防止异常响应耗尽内存。
     */
    std::string display_text;

    /**
     * @brief 数字优先级，值越大越优先处理同一触发时刻的提醒。
     */
    uint8_t priority = 0;

    /**
     * @brief 后端音频状态；2 表示 Ogg/Opus 资源已就绪。
     */
    uint8_t audio_status = 0;

    /**
     * @brief 已就绪的音频资源 UUID；未生成语音时为空。
     */
    std::string audio_asset_id;
};

/**
 * @brief 当前设备生效的提醒开关和播放策略。
 */
struct BackendReminderSetting {
    /**
     * @brief true 表示设备允许执行任何主动提醒。
     */
    bool reminders_enabled = false;
    /**
     * @brief true 表示提醒到期时允许播放动态语音或固定提示音。
     */
    bool voice_enabled = false;
    /**
     * @brief true 表示提醒到期时允许在屏保或通知区域显示正文。
     */
    bool screen_enabled = false;
    /**
     * @brief 用户未指定延后时间时使用的默认分钟数。
     */
    uint16_t default_snooze_minutes = 10;
};

/**
 * @brief 提醒音频下载前由后端返回的受信元数据。
 */
struct BackendReminderAudioMetadata {
    /**
     * @brief 后端授权给当前提醒版本的音频资源 UUID。
     */
    std::string audio_asset_id;
    /**
     * @brief 只能拼接到固定 API 根地址的站内认证下载相对路径。
     */
    std::string download_path;
    /**
     * @brief 完整 Ogg 文件的 64 位小写十六进制 SHA-256。
     */
    std::string sha256;
    /**
     * @brief HTTP 媒体类型，第一阶段必须为 audio/ogg。
     */
    std::string content_type;
    /**
     * @brief OpusHead 声明采样率，第一阶段固定为 24000 Hz。
     */
    uint32_t sample_rate = 0;
    /**
     * @brief 音频有效播放总时长，单位为毫秒。
     */
    uint32_t duration_ms = 0;
    /**
     * @brief 完整 Ogg 文件字节数，用于限制读取和结束校验。
     */
    uint32_t size_bytes = 0;
    /**
     * @brief 单个 Opus 帧目标时长，第一阶段固定为 60 ms。
     */
    uint16_t frame_duration_ms = 0;
    /**
     * @brief 声道数，第一阶段固定为单声道 1。
     */
    uint8_t channels = 0;
    /**
     * @brief AudioCodecEnum 数字值，第一阶段 Opus 为 1。
     */
    uint8_t codec = 0;
};

#endif  // TUNTUN_BACKEND_MODELS_H
