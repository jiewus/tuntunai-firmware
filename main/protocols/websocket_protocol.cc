/**
 * @file websocket_protocol.cc
 * @brief WebSocket 会话、握手和音频帧传输实现。
 */
#include "websocket_protocol.h"
#include "board.h"
#include "system_info.h"
#include "application.h"
#include "settings.h"

#include <cstring>
#include <cJSON.h>
#include <esp_log.h>
#include <arpa/inet.h>
#include "assets/lang_config.h"

#define TAG "WS"

/**
 * @brief 创建事件组，但尚不连接服务器。
 * @details 事件组用于 OpenAudioChannel() 等待服务端 hello，网络对象按需创建。
 */
WebsocketProtocol::WebsocketProtocol() {
    event_group_handle_ = xEventGroupCreate();
}

/**
 * @brief 关闭连接并释放事件组。
 * @details websocket_ 智能指针负责释放连接资源，析构函数只需销毁 hello 同步事件组。
 */
WebsocketProtocol::~WebsocketProtocol() {
    vEventGroupDelete(event_group_handle_);
}

/**
 * @brief 读取 OTA 下发配置并建立 WebSocket。
 * @return 始终返回 true，实际连接会延迟到 OpenAudioChannel() 调用时执行。
 * @details WebSocket 仅在需要语音会话时连接，避免设备空闲期间长期占用 TLS 连接和内存。
 */
bool WebsocketProtocol::Start() {
    // 音频通道按需连接，协议初始化阶段不提前建立 WebSocket。
    return true;
}

/**
 * @brief 按协商的二进制协议版本封装并发送音频包。
 * @param packet 待发送的 Opus 音频包；调用后所有权转移给本方法。
 * @return WebSocket 已连接且二进制帧发送成功时返回 true，否则返回 false。
 * @details 协议 v2 包含版本、时间戳和 32 位负载长度，v3 使用更紧凑的 16 位长度头；
 *          其他版本直接发送 Opus 负载，以兼容旧服务端。
 */
bool WebsocketProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    if (websocket_ == nullptr || !websocket_->IsConnected()) {
        return false;
    }

    if (version_ == 2) {
        std::string serialized;
        serialized.resize(sizeof(BinaryProtocol2) + packet->payload.size());
        auto bp2 = (BinaryProtocol2*)serialized.data();
        bp2->version = htons(version_);
        bp2->type = 0;
        bp2->reserved = 0;
        bp2->timestamp = htonl(packet->timestamp);
        bp2->payload_size = htonl(packet->payload.size());
        memcpy(bp2->payload, packet->payload.data(), packet->payload.size());

        return websocket_->Send(serialized.data(), serialized.size(), true);
    } else if (version_ == 3) {
        std::string serialized;
        serialized.resize(sizeof(BinaryProtocol3) + packet->payload.size());
        auto bp3 = (BinaryProtocol3*)serialized.data();
        bp3->type = 0;
        bp3->reserved = 0;
        bp3->payload_size = htons(packet->payload.size());
        memcpy(bp3->payload, packet->payload.data(), packet->payload.size());

        return websocket_->Send(serialized.data(), serialized.size(), true);
    } else {
        return websocket_->Send(packet->payload.data(), packet->payload.size(), true);
    }
}

/**
 * @brief 发送一条 WebSocket 文本帧。
 * @param text UTF-8 JSON 文本。
 * @return 连接有效且发送成功时返回 true，否则返回 false。
 * @details 发送失败会设置协议错误并通知应用层显示服务器错误。
 */
bool WebsocketProtocol::SendText(const std::string& text) {
    if (websocket_ == nullptr || !websocket_->IsConnected()) {
        return false;
    }

    if (!websocket_->Send(text)) {
        ESP_LOGE(TAG, "Failed to send text: %s", text.c_str());
        SetError(Lang::Strings::SERVER_ERROR);
        return false;
    }

    return true;
}

/**
 * @brief WebSocket 存在且已连接时返回 true。
 * @return 网络对象存在、连接有效、协议无错误且最近通信未超时时返回 true。
 */
bool WebsocketProtocol::IsAudioChannelOpened() const {
    return websocket_ != nullptr && websocket_->IsConnected() && !error_occurred_ && !IsTimeout();
}

/**
 * @brief 可选发送 goodbye 后关闭 WebSocket。
 * @param send_goodbye 保留用于统一 Protocol 接口；WebSocket 关闭帧已表达会话结束，因此本实现忽略该参数。
 * @details 释放 WebSocket 对象会关闭底层连接，断开回调负责通知应用层恢复空闲状态。
 */
void WebsocketProtocol::CloseAudioChannel(bool send_goodbye) {
    (void)send_goodbye;  // WebSocket 通过关闭连接结束会话，不需要额外发送 goodbye JSON。
    websocket_.reset();
}

/**
 * @brief 发送客户端 hello 并等待服务器 hello 完成音频参数协商。
 * @return 连接、客户端 hello 发送和服务端 hello 协商全部成功时返回 true，否则返回 false。
 * @details 从 NVS 读取 URL、令牌和二进制协议版本，设置设备身份请求头并注册数据回调。二进制帧
 *          按协议版本解析为 Opus 包，文本帧按 JSON 消息分发。连接后最多等待十秒获取服务端音频参数。
 */
bool WebsocketProtocol::OpenAudioChannel() {
    Settings settings("websocket", false);
    std::string url = settings.GetString("url");
    std::string token = settings.GetString("token");
    int version = settings.GetInt("version");
    if (version != 0) {
        version_ = version;
    }

    error_occurred_ = false;

    auto network = Board::GetInstance().GetNetwork();
    websocket_ = network->CreateWebSocket(1);
    if (websocket_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create websocket");
        return false;
    }

    if (!token.empty()) {
        // 仅保存裸令牌时自动补充 Bearer 前缀；已有认证方案前缀的值保持不变。
        if (token.find(" ") == std::string::npos) {
            token = "Bearer " + token;
        }
        websocket_->SetHeader("Authorization", token.c_str());
    }
    websocket_->SetHeader("Protocol-Version", std::to_string(version_).c_str());
    websocket_->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    websocket_->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());

    websocket_->OnData([this](const char* data, size_t len, bool binary) {
        if (binary) {
            if (on_incoming_audio_ != nullptr) {
                if (version_ == 2) {
                    BinaryProtocol2* bp2 = (BinaryProtocol2*)data;
                    bp2->version = ntohs(bp2->version);
                    bp2->type = ntohs(bp2->type);
                    bp2->timestamp = ntohl(bp2->timestamp);
                    bp2->payload_size = ntohl(bp2->payload_size);
                    auto payload = (uint8_t*)bp2->payload;
                    on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
                        .sample_rate = server_sample_rate_,
                        .frame_duration = server_frame_duration_,
                        .timestamp = bp2->timestamp,
                        .payload = std::vector<uint8_t>(payload, payload + bp2->payload_size)
                    }));
                } else if (version_ == 3) {
                    BinaryProtocol3* bp3 = (BinaryProtocol3*)data;
                    bp3->type = bp3->type;
                    bp3->payload_size = ntohs(bp3->payload_size);
                    auto payload = (uint8_t*)bp3->payload;
                    on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
                        .sample_rate = server_sample_rate_,
                        .frame_duration = server_frame_duration_,
                        .timestamp = 0,
                        .payload = std::vector<uint8_t>(payload, payload + bp3->payload_size)
                    }));
                } else {
                    on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
                        .sample_rate = server_sample_rate_,
                        .frame_duration = server_frame_duration_,
                        .timestamp = 0,
                        .payload = std::vector<uint8_t>((uint8_t*)data, (uint8_t*)data + len)
                    }));
                }
            }
        } else {
            // 文本帧是 JSON 控制消息；hello 在协议层处理，其余消息转交应用层。
            auto root = cJSON_ParseWithLength(data, len);
            auto type = cJSON_GetObjectItem(root, "type");
            if (cJSON_IsString(type)) {
                if (strcmp(type->valuestring, "hello") == 0) {
                    ParseServerHello(root);
                } else {
                    if (on_incoming_json_ != nullptr) {
                        on_incoming_json_(root);
                    }
                }
            } else {
                ESP_LOGE(TAG, "Missing message type, data: %s", std::string(data, len).c_str());
            }
            cJSON_Delete(root);
        }
        last_incoming_time_ = std::chrono::steady_clock::now();
    });

    websocket_->OnDisconnected([this]() {
        ESP_LOGI(TAG, "Websocket disconnected");
        if (on_audio_channel_closed_ != nullptr) {
            on_audio_channel_closed_();
        }
    });

    ESP_LOGI(TAG, "Connecting to websocket server: %s with version: %d", url.c_str(), version_);
    if (!websocket_->Connect(url.c_str())) {
        ESP_LOGE(TAG, "Failed to connect to websocket server, code=%d", websocket_->GetLastError());
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        return false;
    }

    // 发送客户端能力、音频格式和协议版本，启动会话参数协商。
    auto message = GetHelloMessage();
    if (!SendText(message)) {
        return false;
    }

    // 最多等待十秒；超时说明服务端未完成握手，应用层会显示连接错误。
    EventBits_t bits = xEventGroupWaitBits(event_group_handle_, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT, pdTRUE, pdFALSE, pdMS_TO_TICKS(10000));
    if (!(bits & WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT)) {
        ESP_LOGE(TAG, "Failed to receive server hello");
        SetError(Lang::Strings::SERVER_TIMEOUT);
        return false;
    }

    if (on_audio_channel_opened_ != nullptr) {
        on_audio_channel_opened_();
    }

    return true;
}

/**
 * @brief 生成包含设备能力和协议版本的客户端 hello JSON。
 * @return 无多余空白的 hello JSON 字符串。
 * @details 消息声明 WebSocket 传输、MCP 能力以及 16 kHz 单声道 Opus 参数；启用服务端 AEC 时
 *          同时声明 aec 特性，使服务端选择匹配的音频处理链路。
 */
std::string WebsocketProtocol::GetHelloMessage() {
    // hello 的核心字段包括消息类型、协议版本、能力集合和音频编码参数。
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddNumberToObject(root, "version", version_);
    cJSON* features = cJSON_CreateObject();
#if CONFIG_USE_SERVER_AEC
    cJSON_AddBoolToObject(features, "aec", true);
#endif
    cJSON_AddBoolToObject(features, "mcp", true);
    cJSON_AddItemToObject(root, "features", features);
    cJSON_AddStringToObject(root, "transport", "websocket");
    cJSON* audio_params = cJSON_CreateObject();
    cJSON_AddStringToObject(audio_params, "format", "opus");
    cJSON_AddNumberToObject(audio_params, "sample_rate", 16000);
    cJSON_AddNumberToObject(audio_params, "channels", 1);
    cJSON_AddNumberToObject(audio_params, "frame_duration", OPUS_FRAME_DURATION_MS);
    cJSON_AddItemToObject(root, "audio_params", audio_params);
    auto json_str = cJSON_PrintUnformatted(root);
    std::string message(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return message;
}

/**
 * @brief 解析服务器 hello 中的会话编号、采样率和帧长。
 * @param root 服务端 hello JSON 根对象，仅在本次调用期间有效。
 * @details 先确认 transport 为 websocket，再保存会话编号和服务端 Opus 参数。解析完成后设置事件位，
 *          唤醒正在等待握手结果的 OpenAudioChannel()。
 */
void WebsocketProtocol::ParseServerHello(const cJSON* root) {
    auto transport = cJSON_GetObjectItem(root, "transport");
    if (transport == nullptr || strcmp(transport->valuestring, "websocket") != 0) {
        ESP_LOGE(TAG, "Unsupported transport: %s", transport->valuestring);
        return;
    }

    auto session_id = cJSON_GetObjectItem(root, "session_id");
    if (cJSON_IsString(session_id)) {
        session_id_ = session_id->valuestring;
        ESP_LOGI(TAG, "Session ID: %s", session_id_.c_str());
    }

    auto audio_params = cJSON_GetObjectItem(root, "audio_params");
    if (cJSON_IsObject(audio_params)) {
        auto sample_rate = cJSON_GetObjectItem(audio_params, "sample_rate");
        if (cJSON_IsNumber(sample_rate)) {
            server_sample_rate_ = sample_rate->valueint;
        }
        auto frame_duration = cJSON_GetObjectItem(audio_params, "frame_duration");
        if (cJSON_IsNumber(frame_duration)) {
            server_frame_duration_ = frame_duration->valueint;
        }
    }

    xEventGroupSetBits(event_group_handle_, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT);
}
