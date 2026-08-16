#ifndef XIAOZHI_CLIENT_H
#define XIAOZHI_CLIENT_H

#include <cJSON.h>

#include <functional>
#include <memory>
#include <string>

#include "xiaozhi/protocol/protocol.h"

/**
 * @file xiaozhi_client.h
 * @brief 小智云端会话、消息分类和传输协议统一入口。
 */

/**
 * @brief 小智客户端向应用层报告的会话事件。
 */
struct XiaozhiClientCallbacks {
    std::function<void()> on_connected;
    std::function<void(const std::string& message)> on_network_error;
    std::function<void(std::unique_ptr<AudioStreamPacket> packet)> on_incoming_audio;
    std::function<void()> on_audio_channel_opened;
    std::function<void()> on_audio_channel_closed;
    std::function<void()> on_tts_started;
    std::function<void()> on_tts_stopped;
    std::function<void(const std::string& text)> on_tts_sentence;
    std::function<void(const std::string& text)> on_stt_text;
    std::function<void(const std::string& emotion)> on_llm_emotion;
    std::function<void(const std::string& payload)> on_mcp_message;
    std::function<void()> on_reboot_requested;
    std::function<void(
        const std::string& status,
        const std::string& message,
        const std::string& emotion)> on_alert;
    std::function<void(const std::string& payload)> on_custom_message;
};

/**
 * @brief 封装小智 MQTT/WebSocket 选择、会话控制和下行消息分类。
 */
class XiaozhiClient {
public:
    XiaozhiClient() = default;
    ~XiaozhiClient() = default;

    XiaozhiClient(const XiaozhiClient&) = delete;
    XiaozhiClient& operator=(const XiaozhiClient&) = delete;

    /**
     * @brief 保存应用层事件回调。
     * @param callbacks 固件生命周期内使用的小智会话回调集合。
     */
    void SetCallbacks(XiaozhiClientCallbacks callbacks);

    /**
     * @brief 根据配置创建并启动 MQTT 或 WebSocket 协议。
     * @param has_mqtt_config OTA 响应是否包含 MQTT 配置。
     * @param has_websocket_config OTA 响应是否包含 WebSocket 配置。
     * @return 底层协议启动成功时返回 true，否则返回 false。
     */
    bool Start(bool has_mqtt_config, bool has_websocket_config);

    /** @brief 打开一轮小智音频会话。 */
    bool OpenAudioChannel();
    /** @brief 关闭当前小智音频会话。 */
    void CloseAudioChannel(bool send_goodbye = true);
    /** @brief 查询当前小智音频会话是否可用。 */
    bool IsAudioChannelOpened() const;
    /** @brief 上传一帧已经编码的 Opus 音频。 */
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet);
    /** @brief 通知小智云端开始接收用户语音。 */
    void SendStartListening(ListeningMode mode);
    /** @brief 通知小智云端本轮用户语音已经结束。 */
    void SendStopListening();
    /** @brief 向小智云端上报本地检测到的唤醒词。 */
    void SendWakeWordDetected(const std::string& wake_word);
    /** @brief 请求小智云端中断当前 TTS 播报。 */
    void SendAbortSpeaking(AbortReason reason);
    /** @brief 通过小智会话发送 MCP JSON-RPC 负载。 */
    void SendMcpMessage(const std::string& payload);
    /** @brief 获取服务端协商的下行音频采样率。 */
    int server_sample_rate() const;
    /** @brief 释放底层协议和音频会话。 */
    void Reset();

private:
    XiaozhiClientCallbacks callbacks_;
    std::unique_ptr<Protocol> protocol_;

    /**
     * @brief 按 type 和 state 分类小智云端 JSON 消息。
     * @param root 仅在当前协议回调期间有效的 JSON 根对象。
     */
    void HandleIncomingJson(const cJSON* root);
};

#endif  // XIAOZHI_CLIENT_H
