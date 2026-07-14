/**
 * @file backend_mqtt_client.cc
 * @brief 独立业务 MQTT 连接和固定容量回调队列实现。
 */

#include "backend_mqtt_client.h"

#include <algorithm>
#include <cstring>

#include <cJSON.h>
#include <esp_log.h>

#include "boards/common/board.h"

namespace {

constexpr const char* kTag = "TuntunMqtt";
constexpr UBaseType_t kEventQueueLength = 8;
constexpr size_t kMaximumPayloadBytes = 1024;
constexpr int kKeepAliveSeconds = 90;

}  // namespace

/**
 * @brief 创建用于跨任务复制 BackendEventHint 的固定队列。
 */
BackendMqttClient::BackendMqttClient() {
    event_queue_ = xQueueCreate(kEventQueueLength, sizeof(BackendEventHint));
    if (event_queue_ == nullptr) {
        ESP_LOGE(kTag, "Failed to create business MQTT event queue");
    }
}

/**
 * @brief 停止网络客户端后释放 FreeRTOS 队列。
 */
BackendMqttClient::~BackendMqttClient() {
    Stop();
    if (event_queue_ != nullptr) {
        vQueueDelete(event_queue_);
        event_queue_ = nullptr;
    }
}

/**
 * @brief 使用后端配置创建业务 MQTT，并在连接回调中订阅设备唯一主题。
 * @param config 后端认证接口返回的连接配置。
 * @return 初次连接成功且订阅请求已提交时返回 true。
 */
bool BackendMqttClient::Start(const BackendMqttConfig& config) {
    Stop();
    if (!config.enabled) {
        ESP_LOGI(kTag, "Business MQTT is disabled by backend");
        return false;
    }
    if (event_queue_ == nullptr || config.host.empty() || config.port <= 0
        || config.client_id.empty() || config.username.empty() || config.password.empty()
        || config.event_topic.empty() || config.event_topic.size() > 192
        || config.event_topic.find('+') != std::string::npos
        || config.event_topic.find('#') != std::string::npos) {
        ESP_LOGW(kTag, "Business MQTT config is incomplete");
        return false;
    }
    if (!config.tls || config.port != 8883) {
        ESP_LOGW(kTag, "Business MQTT requires TLS on port 8883");
        return false;
    }

    event_topic_ = config.event_topic;
    qos_ = std::clamp(config.qos, 0, 1);
    connected_signal_.store(false);

    mqtt_ = Board::GetInstance().GetNetwork()->CreateMqtt(1);
    if (!mqtt_) {
        ESP_LOGW(kTag, "Cannot create business MQTT client");
        return false;
    }
    mqtt_->SetKeepAlive(kKeepAliveSeconds);
    mqtt_->OnConnected([this]() {
        if (mqtt_ != nullptr && mqtt_->Subscribe(event_topic_, qos_)) {
            connected_signal_.store(true);
            ESP_LOGI(kTag, "Business MQTT connected and subscribed");
        } else {
            ESP_LOGW(kTag, "Business MQTT subscribe failed");
        }
    });
    mqtt_->OnDisconnected([]() {
        ESP_LOGW(kTag, "Business MQTT disconnected; Xiaozhi protocol remains available");
    });
    mqtt_->OnError([](const std::string& error) {
        ESP_LOGW(kTag, "Business MQTT error: %s", error.c_str());
    });
    mqtt_->OnMessage([this](const std::string& topic, const std::string& payload) {
        HandleMessage(topic, payload);
    });

    const bool connected = mqtt_->Connect(
        config.host,
        config.port,
        config.client_id,
        config.username,
        config.password);
    if (!connected) {
        ESP_LOGW(kTag, "Initial business MQTT connection failed, code=%d", mqtt_->GetLastError());
    }
    return connected;
}

/**
 * @brief 销毁底层客户端并重置回调队列和连接信号。
 */
void BackendMqttClient::Stop() {
    connected_signal_.store(false);
    if (mqtt_ != nullptr) {
        mqtt_->Disconnect();
        mqtt_.reset();
    }
    if (event_queue_ != nullptr) {
        xQueueReset(event_queue_);
    }
    event_topic_.clear();
}

/**
 * @brief 读取底层 MQTT 的当前连接状态。
 * @return 独立业务连接可用时返回 true。
 */
bool BackendMqttClient::IsConnected() const {
    return mqtt_ != nullptr && mqtt_->IsConnected();
}

/**
 * @brief 无等待取出一条 MQTT 提示。
 * @param hint 用于接收固定结构的输出对象。
 * @return 成功取出时返回 true。
 */
bool BackendMqttClient::TryPop(BackendEventHint& hint) {
    return event_queue_ != nullptr && xQueueReceive(event_queue_, &hint, 0) == pdTRUE;
}

/**
 * @brief 原子消费连接成功信号。
 * @return 标志原值；每次连接最多触发一次 Worker 补偿同步。
 */
bool BackendMqttClient::ConsumeConnectedSignal() {
    return connected_signal_.exchange(false);
}

/**
 * @brief 在 MQTT 网络回调中执行有界验证并写入固定队列。
 * @param topic 消息实际到达的主题。
 * @param payload 不允许超过 1024 字节的轻量 JSON。
 */
void BackendMqttClient::HandleMessage(const std::string& topic, const std::string& payload) {
    if (event_queue_ == nullptr || topic != event_topic_ || payload.empty()
        || payload.size() > kMaximumPayloadBytes) {
        ESP_LOGW(kTag, "Discarded invalid business MQTT notification");
        return;
    }

    cJSON* root = cJSON_ParseWithLength(payload.data(), payload.size());
    if (root == nullptr) {
        ESP_LOGW(kTag, "Discarded malformed business MQTT JSON");
        return;
    }
    const cJSON* event_id = cJSON_GetObjectItemCaseSensitive(root, "event_id");
    const cJSON* type = cJSON_GetObjectItemCaseSensitive(root, "type");
    const cJSON* revision = cJSON_GetObjectItemCaseSensitive(root, "revision");

    BackendEventHint hint;
    if (!cJSON_IsString(event_id) || event_id->valuestring == nullptr
        || std::strlen(event_id->valuestring) != 36
        || !cJSON_IsString(type) || type->valuestring == nullptr
        || !cJSON_IsNumber(revision) || revision->valuedouble < 0) {
        cJSON_Delete(root);
        ESP_LOGW(kTag, "Discarded incomplete business MQTT notification");
        return;
    }
    std::memcpy(hint.event_id, event_id->valuestring, 36);
    hint.event_id[36] = '\0';
    hint.type = ParseEventType(type->valuestring);
    hint.revision = static_cast<uint64_t>(revision->valuedouble);
    cJSON_Delete(root);

    if (hint.type == BackendEventType::Unknown) {
        ESP_LOGW(kTag, "Discarded unknown business MQTT event type");
        return;
    }
    if (xQueueSend(event_queue_, &hint, 0) != pdTRUE) {
        /*
         * 队列满时丢弃提示是安全的：Worker 会在 MQTT 重连和固定周期执行 HTTPS
         * 权威同步，MQTT 本身从来不是唯一数据来源。
         */
        ESP_LOGW(kTag, "Business MQTT event queue is full; HTTPS sync will compensate");
    }
}

/**
 * @brief 把后端稳定字符串映射为内部事件枚举。
 * @param value 非空、零结尾的类型字符串。
 * @return 已知映射或 Unknown。
 */
BackendEventType BackendMqttClient::ParseEventType(const char* value) {
    if (std::strcmp(value, "screen.notification") == 0) {
        return BackendEventType::ScreenNotification;
    }
    if (std::strcmp(value, "data.refresh") == 0) {
        return BackendEventType::DataRefresh;
    }
    if (std::strcmp(value, "voice.reminder") == 0) {
        return BackendEventType::VoiceReminder;
    }
    if (std::strcmp(value, "reminder.schedule_changed") == 0) {
        return BackendEventType::ReminderScheduleChanged;
    }
    if (std::strcmp(value, "device.config_changed") == 0) {
        return BackendEventType::DeviceConfigChanged;
    }
    if (std::strcmp(value, "tool.manifest_changed") == 0) {
        return BackendEventType::ToolManifestChanged;
    }
    if (std::strcmp(value, "workflow.result") == 0) {
        return BackendEventType::WorkflowResult;
    }
    return BackendEventType::Unknown;
}
