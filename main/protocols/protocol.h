#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <cJSON.h>
#include <string>
#include <functional>
#include <chrono>
#include <vector>

/**
 * @file protocol.h
 * @brief 云端语音协议的公共数据结构和抽象接口。
 */

/**
 * @brief 一帧待发送或刚收到的压缩音频。
 */
struct AudioStreamPacket {
    /**
     * 音频采样率，单位 Hz。
     */
    int sample_rate = 0;
    int frame_duration = 0;
    uint32_t timestamp = 0;
    std::vector<uint8_t> payload;
};

/**
 * @brief 二进制协议版本 2 的网络帧头，所有字段按紧凑布局传输。
 */
struct BinaryProtocol2 {
    uint16_t version;
    uint16_t type;          ///< 消息类型：0 表示 Opus，1 表示 JSON。
    uint32_t reserved;      ///< 保留字段，发送时置零。
    uint32_t timestamp;     ///< 毫秒时间戳，供云端回声消除对齐使用。
    uint32_t payload_size;  ///< payload 的字节数。
    uint8_t payload[];      ///< 紧跟帧头的变长负载。
} __attribute__((packed));

/**
 * @brief 二进制协议版本 3 的精简网络帧头。
 */
struct BinaryProtocol3 {
    uint8_t type;
    uint8_t reserved;
    uint16_t payload_size;
    uint8_t payload[];
} __attribute__((packed));

/**
 * @brief 终止云端播报的原因。
 */
enum AbortReason {
    kAbortReasonNone,
    kAbortReasonWakeWordDetected
};

/**
 * @brief 一轮语音输入的结束策略。
 */
enum ListeningMode {
    kListeningModeAutoStop,
    kListeningModeManualStop,
    kListeningModeRealtime // 需要 AEC 支持
};

/**
 * @brief WebSocket 与 MQTT/UDP 协议共同实现的云端会话接口。
 *
 * 子类负责连接和传输，本类负责统一 JSON 控制消息、回调注册、超时与错误状态。
 */
class Protocol {
public:
    virtual ~Protocol() = default;

    /**
     * @brief 获取服务端下发音频的采样率，单位 Hz。
     */
    inline int server_sample_rate() const {
        return server_sample_rate_;
    }
    /**
     * @brief 获取服务端音频帧时长，单位 ms。
     */
    inline int server_frame_duration() const {
        return server_frame_duration_;
    }
    /**
     * @brief 获取当前会话编号；仅在协议对象存活期间有效。
     */
    inline const std::string& session_id() const {
        return session_id_;
    }

    /**
     * @brief 注册音频接收回调。
     * @param callback 接收音频包所有权的回调。
     */
    void OnIncomingAudio(std::function<void(std::unique_ptr<AudioStreamPacket> packet)> callback);
    /**
     * @brief 注册 JSON 控制消息回调。
     * @param callback root 仅在回调执行期间有效。
     */
    void OnIncomingJson(std::function<void(const cJSON* root)> callback);
    /**
     * @brief 注册音频通道建立完成回调。
     */
    void OnAudioChannelOpened(std::function<void()> callback);
    /**
     * @brief 注册音频通道关闭回调。
     */
    void OnAudioChannelClosed(std::function<void()> callback);
    /**
     * @brief 注册不可恢复网络错误回调。
     * @param callback 参数为可显示的错误信息。
     */
    void OnNetworkError(std::function<void(const std::string& message)> callback);
    /**
     * @brief 注册协议控制连接建立回调。
     */
    void OnConnected(std::function<void()> callback);
    /**
     * @brief 注册协议控制连接断开回调。
     */
    void OnDisconnected(std::function<void()> callback);

    /**
     * @brief 启动协议客户端及其控制连接。
     * @return 启动成功返回 true。
     */
    virtual bool Start() = 0;
    /**
     * @brief 创建一轮云端音频会话。
     * @return 握手完成返回 true。
     */
    virtual bool OpenAudioChannel() = 0;
    /**
     * @brief 关闭音频会话。
     * @param send_goodbye 是否先发送正常结束消息。
     */
    virtual void CloseAudioChannel(bool send_goodbye = true) = 0;
    /**
     * @brief 查询音频通道当前是否可发送数据。
     */
    virtual bool IsAudioChannelOpened() const = 0;
    /**
     * @brief 发送一帧 Opus 音频。
     * @param packet 待发送数据包的所有权。
     */
    virtual bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) = 0;
    /**
     * @brief 通知云端开始聆听。
     * @param mode 本轮输入的停止策略。
     */
    virtual void SendStartListening(ListeningMode mode);
    /**
     * @brief 通知云端用户输入结束。
     */
    virtual void SendStopListening();
    /**
     * @brief 请求云端中断当前 TTS。
     * @param reason 中断原因。
     */
    virtual void SendAbortSpeaking(AbortReason reason);
    /**
     * @brief 发送 MCP JSON-RPC 消息。
     * @param message 完整 JSON 文本。
     */
    virtual void SendMcpMessage(const std::string& message);

protected:
    std::function<void(const cJSON* root)> on_incoming_json_;
    std::function<void(std::unique_ptr<AudioStreamPacket> packet)> on_incoming_audio_;
    std::function<void()> on_audio_channel_opened_;
    std::function<void()> on_audio_channel_closed_;
    std::function<void(const std::string& message)> on_network_error_;
    std::function<void()> on_connected_;
    std::function<void()> on_disconnected_;

    int server_sample_rate_ = 24000;
    int server_frame_duration_ = 60;
    bool error_occurred_ = false;
    std::string session_id_;
    std::chrono::time_point<std::chrono::steady_clock> last_incoming_time_;

    /**
     * @brief 通过子类控制通道发送 UTF-8 文本。
     */
    virtual bool SendText(const std::string& text) = 0;
    /**
     * @brief 记录首次错误并触发网络错误回调。
     * @param message 错误说明。
     */
    virtual void SetError(const std::string& message);
    /**
     * @brief 判断距离最后一次收到服务器数据是否已经超时。
     */
    virtual bool IsTimeout() const;
};

#endif // PROTOCOL_H
