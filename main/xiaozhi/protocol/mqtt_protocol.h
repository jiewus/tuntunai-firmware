#ifndef MQTT_PROTOCOL_H
#define MQTT_PROTOCOL_H


#include "xiaozhi/protocol/protocol.h"
#include <mqtt.h>
#include <udp.h>
#include <cJSON.h>
#include <mbedtls/aes.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <esp_timer.h>

#include <functional>
#include <string>
#include <map>
#include <mutex>
#include <memory>
#include <atomic>

/**
 * @file mqtt_protocol.h
 * @brief MQTT 控制面加加密 UDP 音频面的协议实现。
 */

#define MQTT_PING_INTERVAL_SECONDS 90
#define MQTT_RECONNECT_INTERVAL_MS 60000

#define MQTT_PROTOCOL_SERVER_HELLO_EVENT (1 << 0)

/**
 * @brief 使用 MQTT 交换控制 JSON、使用 AES-CTR 加密 UDP 传输实时音频。
 *
 * MQTT 负责 hello、listen、MCP 等可靠消息；UDP 负责低延迟 Opus 数据。音频通道
 * 由 channel_mutex_ 保护，定时重连回调通过 alive_ 避免析构后访问对象。
 */
class MqttProtocol : public Protocol {
public:
    /**
     * @brief 初始化 AES 上下文、事件组和重连定时器。
     */
    MqttProtocol();
    /**
     * @brief 停止定时器和网络对象，并使所有延迟回调失效。
     */
    ~MqttProtocol();

    /**
     * @brief 根据 OTA 配置连接 MQTT 并订阅下行主题。
     */
    bool Start() override;
    /**
     * @brief 添加序号与时间戳、AES 加密后通过 UDP 发送音频。
     */
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;
    /**
     * @brief 通过 MQTT 请求服务器创建 UDP 音频会话并等待 hello。
     */
    bool OpenAudioChannel() override;
    /**
     * @brief 关闭 UDP 会话并按需发布 goodbye。
     */
    void CloseAudioChannel(bool send_goodbye = true) override;
    /**
     * @brief UDP 对象和会话参数均有效时返回 true。
     */
    bool IsAudioChannelOpened() const override;

private:
    // 延迟回调共享的存活标志；析构时置 false，防止定时任务访问已释放的 this。
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
    
    EventGroupHandle_t event_group_handle_;

    std::string publish_topic_;

    std::mutex channel_mutex_;
    std::unique_ptr<Mqtt> mqtt_;
    std::unique_ptr<Udp> udp_;
    mbedtls_aes_context aes_ctx_;
    std::string aes_nonce_;
    std::string udp_server_;
    int udp_port_;
    uint32_t local_sequence_;
    uint32_t remote_sequence_;
    /** @brief 当前会话累计发现的音频丢包数量。 */
    uint32_t remote_sequence_gap_count_ = 0;
    /** @brief 上一次输出音频丢包汇总日志的单调时钟时间。 */
    int64_t remote_sequence_last_warning_us_ = 0;
    esp_timer_handle_t reconnect_timer_;

    /**
     * @brief 创建并连接 MQTT 客户端。
     * @param report_error 失败时是否上报到应用层。
     */
    bool StartMqttClient(bool report_error=false);
    /**
     * @brief 解析服务端 UDP 地址、端口、AES nonce 和会话参数。
     */
    void ParseServerHello(const cJSON* root);
    /**
     * @brief 把偶数长度十六进制字符串转换为原始字节。
     */
    std::string DecodeHexString(const std::string& hex_string);

    /**
     * @brief 向设备发布主题发送 JSON 文本。
     */
    bool SendText(const std::string& text) override;
    /**
     * @brief 生成 MQTT 协议使用的客户端 hello JSON。
     */
    std::string GetHelloMessage();
};


#endif // MQTT_PROTOCOL_H
