/**
 * @file xiaozhi_client.cc
 * @brief 小智云端会话、消息分类和传输协议统一入口实现。
 */

#include "xiaozhi/client/xiaozhi_client.h"

#include <cstring>
#include <utility>

#include <esp_log.h>
#include "sdkconfig.h"

#include "xiaozhi/protocol/mqtt_protocol.h"
#include "xiaozhi/protocol/websocket_protocol.h"

#define TAG "XiaozhiClient"

namespace {

std::string SerializeJson(const cJSON* value) {
    if (value == nullptr) {
        return {};
    }
    char* serialized = cJSON_PrintUnformatted(value);
    if (serialized == nullptr) {
        return {};
    }
    std::string result(serialized);
    cJSON_free(serialized);
    return result;
}

}  // namespace

void XiaozhiClient::SetCallbacks(XiaozhiClientCallbacks callbacks) {
    callbacks_ = std::move(callbacks);
}

bool XiaozhiClient::Start(bool has_mqtt_config, bool has_websocket_config) {
    Reset();
    if (has_mqtt_config) {
        protocol_ = std::make_unique<MqttProtocol>();
    } else if (has_websocket_config) {
        protocol_ = std::make_unique<WebsocketProtocol>();
    } else {
        ESP_LOGW(TAG, "未指定小智云端协议，默认使用 MQTT");
        protocol_ = std::make_unique<MqttProtocol>();
    }

    protocol_->OnConnected([this]() {
        if (callbacks_.on_connected) {
            callbacks_.on_connected();
        }
    });
    protocol_->OnNetworkError([this](const std::string& message) {
        if (callbacks_.on_network_error) {
            callbacks_.on_network_error(message);
        }
    });
    protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
        if (callbacks_.on_incoming_audio) {
            callbacks_.on_incoming_audio(std::move(packet));
        }
    });
    protocol_->OnAudioChannelOpened([this]() {
        if (callbacks_.on_audio_channel_opened) {
            callbacks_.on_audio_channel_opened();
        }
    });
    protocol_->OnAudioChannelClosed([this]() {
        if (callbacks_.on_audio_channel_closed) {
            callbacks_.on_audio_channel_closed();
        }
    });
    protocol_->OnIncomingJson([this](const cJSON* root) {
        HandleIncomingJson(root);
    });
    return protocol_->Start();
}

bool XiaozhiClient::OpenAudioChannel() {
    return protocol_ != nullptr && protocol_->OpenAudioChannel();
}

void XiaozhiClient::CloseAudioChannel(bool send_goodbye) {
    if (protocol_ != nullptr) {
        protocol_->CloseAudioChannel(send_goodbye);
    }
}

bool XiaozhiClient::IsAudioChannelOpened() const {
    return protocol_ != nullptr && protocol_->IsAudioChannelOpened();
}

bool XiaozhiClient::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    return protocol_ != nullptr && protocol_->SendAudio(std::move(packet));
}

void XiaozhiClient::SendStartListening(ListeningMode mode) {
    if (protocol_ != nullptr) {
        protocol_->SendStartListening(mode);
    }
}

void XiaozhiClient::SendStopListening() {
    if (protocol_ != nullptr) {
        protocol_->SendStopListening();
    }
}

void XiaozhiClient::SendWakeWordDetected(const std::string& wake_word) {
    if (protocol_ != nullptr) {
        protocol_->SendWakeWordDetected(wake_word);
    }
}

void XiaozhiClient::SendAbortSpeaking(AbortReason reason) {
    if (protocol_ != nullptr) {
        protocol_->SendAbortSpeaking(reason);
    }
}

void XiaozhiClient::SendMcpMessage(const std::string& payload) {
    if (protocol_ != nullptr) {
        protocol_->SendMcpMessage(payload);
    }
}

int XiaozhiClient::server_sample_rate() const {
    return protocol_ == nullptr ? 0 : protocol_->server_sample_rate();
}

void XiaozhiClient::Reset() {
    if (protocol_ != nullptr && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
    protocol_.reset();
}

void XiaozhiClient::HandleIncomingJson(const cJSON* root) {
    const cJSON* type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type)) {
        ESP_LOGW(TAG, "收到缺少类型的小智云端消息");
        return;
    }

    if (std::strcmp(type->valuestring, "tts") == 0) {
        const cJSON* state = cJSON_GetObjectItemCaseSensitive(root, "state");
        if (!cJSON_IsString(state)) {
            ESP_LOGW(TAG, "收到缺少状态的 TTS 消息");
            return;
        }
        if (std::strcmp(state->valuestring, "start") == 0) {
            if (callbacks_.on_tts_started) {
                callbacks_.on_tts_started();
            }
        } else if (std::strcmp(state->valuestring, "stop") == 0) {
            if (callbacks_.on_tts_stopped) {
                callbacks_.on_tts_stopped();
            }
        } else if (std::strcmp(state->valuestring, "sentence_start") == 0) {
            const cJSON* text = cJSON_GetObjectItemCaseSensitive(root, "text");
            if (cJSON_IsString(text) && callbacks_.on_tts_sentence) {
                callbacks_.on_tts_sentence(text->valuestring);
            }
        }
        return;
    }

    if (std::strcmp(type->valuestring, "stt") == 0) {
        const cJSON* text = cJSON_GetObjectItemCaseSensitive(root, "text");
        if (cJSON_IsString(text) && callbacks_.on_stt_text) {
            callbacks_.on_stt_text(text->valuestring);
        }
        return;
    }

    if (std::strcmp(type->valuestring, "llm") == 0) {
        const cJSON* emotion = cJSON_GetObjectItemCaseSensitive(root, "emotion");
        if (cJSON_IsString(emotion) && callbacks_.on_llm_emotion) {
            callbacks_.on_llm_emotion(emotion->valuestring);
        }
        return;
    }

    if (std::strcmp(type->valuestring, "mcp") == 0) {
        const cJSON* payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
        if (cJSON_IsObject(payload) && callbacks_.on_mcp_message) {
            callbacks_.on_mcp_message(SerializeJson(payload));
        }
        return;
    }

    if (std::strcmp(type->valuestring, "system") == 0) {
        const cJSON* command = cJSON_GetObjectItemCaseSensitive(root, "command");
        if (cJSON_IsString(command)
            && std::strcmp(command->valuestring, "reboot") == 0
            && callbacks_.on_reboot_requested) {
            callbacks_.on_reboot_requested();
        } else if (cJSON_IsString(command)) {
            ESP_LOGW(TAG, "收到未知系统命令，命令=%s", command->valuestring);
        }
        return;
    }

    if (std::strcmp(type->valuestring, "alert") == 0) {
        const cJSON* status = cJSON_GetObjectItemCaseSensitive(root, "status");
        const cJSON* message = cJSON_GetObjectItemCaseSensitive(root, "message");
        const cJSON* emotion = cJSON_GetObjectItemCaseSensitive(root, "emotion");
        if (cJSON_IsString(status)
            && cJSON_IsString(message)
            && cJSON_IsString(emotion)
            && callbacks_.on_alert) {
            callbacks_.on_alert(status->valuestring, message->valuestring, emotion->valuestring);
        } else {
            ESP_LOGW(TAG, "告警消息缺少状态、正文或表情字段");
        }
        return;
    }

#if CONFIG_RECEIVE_CUSTOM_MESSAGE
    if (std::strcmp(type->valuestring, "custom") == 0) {
        const cJSON* payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
        if (cJSON_IsObject(payload) && callbacks_.on_custom_message) {
            callbacks_.on_custom_message(SerializeJson(payload));
        } else {
            ESP_LOGW(TAG, "自定义消息缺少 payload 对象");
        }
        return;
    }
#endif

    ESP_LOGW(TAG, "收到未知小智消息类型，类型=%s", type->valuestring);
}
