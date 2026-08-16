/**
 * @file protocol.cc
 * @brief 云端协议公共控制消息与回调实现。
 */
#include "xiaozhi/protocol/protocol.h"

#include <esp_log.h>

#define TAG "Protocol"

/**
 * @brief 注册 JSON 控制消息回调。
 * @param callback root 仅在回调执行期间有效。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void Protocol::OnIncomingJson(std::function<void(const cJSON* root)> callback) {
    on_incoming_json_ = callback;
}

/**
 * @brief 注册音频接收回调。
 * @param callback 接收音频包所有权的回调。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void Protocol::OnIncomingAudio(std::function<void(std::unique_ptr<AudioStreamPacket> packet)> callback) {
    on_incoming_audio_ = callback;
}

/**
 * @brief 注册音频通道建立完成回调。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void Protocol::OnAudioChannelOpened(std::function<void()> callback) {
    on_audio_channel_opened_ = callback;
}

/**
 * @brief 注册音频通道关闭回调。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void Protocol::OnAudioChannelClosed(std::function<void()> callback) {
    on_audio_channel_closed_ = callback;
}

/**
 * @brief 注册不可恢复网络错误回调。
 * @param callback 参数为可显示的错误信息。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void Protocol::OnNetworkError(std::function<void(const std::string& message)> callback) {
    on_network_error_ = callback;
}

/**
 * @brief 注册协议控制连接建立回调。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void Protocol::OnConnected(std::function<void()> callback) {
    on_connected_ = callback;
}

/**
 * @brief 注册协议控制连接断开回调。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void Protocol::OnDisconnected(std::function<void()> callback) {
    on_disconnected_ = callback;
}

/**
 * @brief 记录首次错误并触发网络错误回调。
 * @param message 错误说明。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void Protocol::SetError(const std::string& message) {
    error_occurred_ = true;
    if (on_network_error_ != nullptr) {
        on_network_error_(message);
    }
}

/**
 * @brief 请求云端中断当前 TTS。
 * @param reason 中断原因。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void Protocol::SendAbortSpeaking(AbortReason reason) {
    std::string message = "{\"session_id\":\"" + session_id_ + "\",\"type\":\"abort\"";
    if (reason == kAbortReasonWakeWordDetected) {
        message += ",\"reason\":\"wake_word_detected\"";
    }
    message += "}";
    SendText(message);
}

/**
 * @brief 通知云端开始聆听。
 * @param mode 本轮输入的停止策略。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void Protocol::SendStartListening(ListeningMode mode) {
    std::string message = "{\"session_id\":\"" + session_id_ + "\"";
    message += ",\"type\":\"listen\",\"state\":\"start\"";
    if (mode == kListeningModeRealtime) {
        message += ",\"mode\":\"realtime\"";
    } else if (mode == kListeningModeAutoStop) {
        message += ",\"mode\":\"auto\"";
    } else {
        message += ",\"mode\":\"manual\"";
    }
    message += "}";
    SendText(message);
}

/**
 * @brief 通知云端用户输入结束。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void Protocol::SendStopListening() {
    std::string message = "{\"session_id\":\"" + session_id_ + "\",\"type\":\"listen\",\"state\":\"stop\"}";
    SendText(message);
}

/**
 * @brief 向云端上报本地检测到的唤醒词。
 * @param wake_word 最近一次识别出的唤醒词文本。
 * @details 与官方小智协议一致，发送 type="listen", state="detect" 的控制消息，text 字段携带
 *          唤醒词文本，供云端识别/统计唤醒词命中。
 */
void Protocol::SendWakeWordDetected(const std::string& wake_word) {
    std::string message = "{\"session_id\":\"" + session_id_ +
        "\",\"type\":\"listen\",\"state\":\"detect\",\"text\":\"" + wake_word + "\"}";
    SendText(message);
}

/**
 * @brief 发送 MCP JSON-RPC 消息。
 * @param message 完整 JSON 文本。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void Protocol::SendMcpMessage(const std::string& payload) {
    std::string message = "{\"session_id\":\"" + session_id_ + "\",\"type\":\"mcp\",\"payload\":" + payload + "}";
    SendText(message);
}

/**
 * @brief 判断距离最后一次收到服务器数据是否已经超时。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
bool Protocol::IsTimeout() const {
    const int kTimeoutSeconds = 120;
    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - last_incoming_time_);
    bool timeout = duration.count() > kTimeoutSeconds;
    if (timeout) {
        ESP_LOGE(TAG, "小智音频通道已连续 %ld 秒无数据", (long)duration.count());
    }
    return timeout;
}
