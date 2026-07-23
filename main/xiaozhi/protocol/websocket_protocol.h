#ifndef _WEBSOCKET_PROTOCOL_H_
#define _WEBSOCKET_PROTOCOL_H_


#include "xiaozhi/protocol/protocol.h"

#include <web_socket.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

/**
 * @file websocket_protocol.h
 * @brief 基于 WebSocket 的小智云端协议实现。
 */

#define WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT (1 << 0) ///< 已收到服务器 hello。

/**
 * @brief 使用单条 WebSocket 连接同时传输 JSON 控制消息与 Opus 音频。
 */
class WebsocketProtocol : public Protocol {
public:
    /**
     * @brief 创建事件组，但尚不连接服务器。
     */
    WebsocketProtocol();
    /**
     * @brief 关闭连接并释放事件组。
     */
    ~WebsocketProtocol();

    /**
     * @brief 读取 OTA 下发配置并建立 WebSocket。
     */
    bool Start() override;
    /**
     * @brief 按协商的二进制协议版本封装并发送音频包。
     */
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;
    /**
     * @brief 发送客户端 hello 并等待服务器 hello 完成音频参数协商。
     */
    bool OpenAudioChannel() override;
    /**
     * @brief 可选发送 goodbye 后关闭 WebSocket。
     */
    void CloseAudioChannel(bool send_goodbye = true) override;
    /**
     * @brief WebSocket 存在且已连接时返回 true。
     */
    bool IsAudioChannelOpened() const override;

private:
    EventGroupHandle_t event_group_handle_;
    std::unique_ptr<WebSocket> websocket_;
    int version_ = 1;

    /**
     * @brief 解析服务器 hello 中的会话编号、采样率和帧长。
     * @param root JSON 根对象。
     */
    void ParseServerHello(const cJSON* root);
    /**
     * @brief 发送一条 WebSocket 文本帧。
     * @param text UTF-8 JSON 文本。
     */
    bool SendText(const std::string& text) override;
    /**
     * @brief 生成包含设备能力和协议版本的客户端 hello JSON。
     */
    std::string GetHelloMessage();
};

#endif
