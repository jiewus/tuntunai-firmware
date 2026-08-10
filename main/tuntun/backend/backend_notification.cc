/**
 * @file backend_notification.cc
 * @brief 囤囤AI主动通知、业务 MQTT 和设备心跳实现。
 */

#include "tuntun/backend/backend_service.h"
#include "tuntun/backend/backend_utils.h"

#include <cJSON.h>
#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/idf_additions.h>
#include <http.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <unordered_set>
#include <utility>

#include "app/application.h"
#include "assets/lang_config.h"
#include "audio/demuxer/ogg_demuxer.h"
#include "boards/common/board.h"
#include "display/display.h"
#include "mcp/mcp_server.h"
#include "system/settings.h"
#include "system/system_info.h"

using namespace tuntun::backend_internal;

/**
 * @brief 在设备已绑定且联网时把通知同步请求加入常驻后端 Worker。
 * @param reconnect_mqtt true 表示同时按需重新连接业务 EMQX。
 */
void BackendService::StartNotificationSync(bool reconnect_mqtt)
{
    if (!network_connected_.load())
    {
        return;
    }
    {
        std::lock_guard<std::mutex> binding_lock(binding_mutex_);
        if (device_access_token_.empty() || device_id_.empty())
        {
            return;
        }
    }
    std::lock_guard<std::mutex> lock(notification_mutex_);
    if (reconnect_mqtt)
    {
        notification_reconnect_requested_.store(true);
    }
    if (notification_task_handle_ != nullptr
        || notification_playback_task_handle_ != nullptr
        || notification_confirmation_task_handle_ != nullptr)
    {
        return;
    }
    notification_task_handle_ = backend_worker_task_handle_;
    if (!EnqueueBackendJob(BackendJobType::NotificationSync))
    {
        notification_task_handle_ = nullptr;
        ESP_LOGE(kTag, "后端 Worker 队列已满，无法安排主动通知同步");
    }
}

/**
 * @brief 按需连接业务 EMQX，并通过 HTTPS 获取第一条可处理通知。
 */
void BackendService::RunNotificationSync()
{
    if (!network_connected_.load())
    {
        return;
    }
    NotificationHint hint;
    const bool has_hint = DequeueNotificationHint(hint);
    if (has_hint)
    {
        ESP_LOGI(kTag, "正在处理主动通知提示，通知标识=%s，投递标识=%s",
                 hint.notification_id.data(), hint.delivery_id.data());
    }

    bool mqtt_connected = false;
    {
        std::lock_guard<std::mutex> lock(notification_mutex_);
        mqtt_connected = notification_mqtt_ != nullptr && notification_mqtt_->IsConnected();
    }
    if (!mqtt_connected)
    {
        std::unique_ptr<Mqtt> disconnected_mqtt;
        {
            std::lock_guard<std::mutex> lock(notification_mutex_);
            active_notification_mqtt_.store(nullptr);
            heartbeat_topic_.clear();
            disconnected_mqtt = std::move(notification_mqtt_);
        }
        if (disconnected_mqtt != nullptr)
        {
            disconnected_mqtt->Disconnect();
        }
        if (!ConnectNotificationMqtt())
        {
            ScheduleNotificationMqttReconnect();
        }
    }

    std::string access_token;
    {
        std::lock_guard<std::mutex> lock(binding_mutex_);
        access_token = device_access_token_;
    }
    const std::string notification_path = has_hint
        ? "/api/device/notifications/" + std::string(hint.notification_id.data())
        : kNotificationPendingPath;
    const HttpResponse response = SendJsonRequest(
        "GET",
        notification_path.c_str(),
        access_token,
        "",
        kNotificationResponseMaxBytes);
    if (!response.transport_succeeded || response.status_code != 200)
    {
        ESP_LOGW(kTag, "主动通知同步失败，HTTP状态码=%d", response.status_code);
        return;
    }

    cJSON *root = cJSON_Parse(response.body.c_str());
    if (root == nullptr)
    {
        ESP_LOGW(kTag, "主动通知响应不是有效JSON");
        return;
    }
    cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
    cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (!cJSON_IsNumber(code) || code->valueint != 0
        || (has_hint ? !cJSON_IsObject(data) : !cJSON_IsArray(data)))
    {
        cJSON_Delete(root);
        ESP_LOGW(kTag, "主动通知响应结构无效");
        return;
    }
    cJSON *item = has_hint ? data : cJSON_GetArrayItem(data, 0);
    if (!cJSON_IsObject(item))
    {
        cJSON_Delete(root);
        return;
    }

    PendingNotification notification;
    cJSON *source_type = cJSON_GetObjectItemCaseSensitive(item, "source_type");
    cJSON *notification_mode = cJSON_GetObjectItemCaseSensitive(item, "notification_mode");
    cJSON *has_audio = cJSON_GetObjectItemCaseSensitive(item, "has_audio");
    cJSON *audio_segment_count = cJSON_GetObjectItemCaseSensitive(item, "audio_segment_count");
    const int parsed_audio_segment_count = cJSON_IsNumber(audio_segment_count)
        ? audio_segment_count->valueint
        : (cJSON_IsTrue(has_audio) ? 1 : 0);
    const bool valid = ReadBoundedString(
                           item,
                           "notification_id",
                           50,
                           notification.notification_id) &&
                       ReadBoundedString(
                           item,
                           "delivery_id",
                           50,
                           notification.delivery_id) &&
                       ReadBoundedString(
                           item,
                           "display_text",
                           kNotificationTextMaxBytes,
                           notification.display_text) &&
                       cJSON_IsNumber(source_type) &&
                       source_type->valueint >= kNotificationSourceScheduledMcp &&
                       source_type->valueint <= kNotificationSourceDailyNewsBriefing &&
                       cJSON_IsNumber(notification_mode) &&
                       (notification_mode->valueint == kNotificationModeDirect ||
                        notification_mode->valueint == kNotificationModeConfirm) &&
                       cJSON_IsBool(has_audio) &&
                       parsed_audio_segment_count >= 0 &&
                       parsed_audio_segment_count <= kNotificationMaximumAudioSegments &&
                       (cJSON_IsTrue(has_audio)
                            ? parsed_audio_segment_count > 0
                            : parsed_audio_segment_count == 0);
    if (!valid)
    {
        cJSON_Delete(root);
        ESP_LOGW(kTag, "主动通知字段校验失败");
        return;
    }
    if (has_hint && notification.delivery_id != hint.delivery_id.data())
    {
        cJSON_Delete(root);
        ESP_LOGW(kTag, "主动通知详情投递标识不匹配");
        return;
    }
    notification.notification_mode = notification_mode->valueint;
    notification.has_audio = cJSON_IsTrue(has_audio);
    notification.audio_segment_count = parsed_audio_segment_count;
    if (source_type->valueint == kNotificationSourceScheduledMcp)
    {
        cJSON *tool_name = cJSON_GetObjectItemCaseSensitive(item, "mcp_tool_name");
        notification.source_title = cJSON_IsString(tool_name) && tool_name->valuestring != nullptr
            ? GetVisibleDynamicToolName(tool_name->valuestring)
            : "MCP 通知";
    }
    else
    {
        if (source_type->valueint == kNotificationSourceWeather)
        {
            notification.source_title = "天气播报";
        }
        else if (source_type->valueint == kNotificationSourceMemo)
        {
            notification.source_title = "备忘录提醒";
        }
        else if (source_type->valueint == kNotificationSourceDailyNewsBriefing)
        {
            notification.source_title = "每日简报";
        }
        else
        {
            notification.source_title = "自定义提醒";
        }
    }
    notification.valid = true;
    cJSON_Delete(root);
    HandlePendingNotification(notification);
}

/**
 * @brief 把合法 MQTT 通知提示写入固定容量队列并按投递标识去重。
 * @param notification_id MQTT 消息中的通知 UUID。
 * @param delivery_id MQTT 消息中的设备投递 UUID。
 * @return 提示成功入队时返回 true；重复或队列已满时返回 false。
 */
bool BackendService::EnqueueNotificationHint(
    const std::string &notification_id,
    const std::string &delivery_id)
{
    std::lock_guard<std::mutex> lock(notification_mutex_);
    for (size_t offset = 0; offset < notification_hint_count_; ++offset)
    {
        const size_t index = (notification_hint_head_ + offset) % notification_hints_.size();
        if (delivery_id == notification_hints_[index].delivery_id.data())
        {
            return false;
        }
    }
    if (notification_hint_count_ >= notification_hints_.size())
    {
        ESP_LOGW(kTag, "主动通知提示队列已满，等待HTTPS周期补偿");
        return false;
    }

    auto &hint = notification_hints_[notification_hint_tail_];
    std::snprintf(hint.notification_id.data(), hint.notification_id.size(), "%s",
                  notification_id.c_str());
    std::snprintf(hint.delivery_id.data(), hint.delivery_id.size(), "%s",
                  delivery_id.c_str());
    notification_hint_tail_ = (notification_hint_tail_ + 1) % notification_hints_.size();
    ++notification_hint_count_;
    return true;
}

/**
 * @brief 从固定容量队列取出最早到达的一条 MQTT 提示。
 * @param hint 接收已经完成边界校验的提示副本。
 * @return 队列存在提示时返回 true。
 */
bool BackendService::DequeueNotificationHint(NotificationHint &hint)
{
    std::lock_guard<std::mutex> lock(notification_mutex_);
    if (notification_hint_count_ == 0)
    {
        return false;
    }
    hint = notification_hints_[notification_hint_head_];
    notification_hints_[notification_hint_head_] = NotificationHint{};
    notification_hint_head_ = (notification_hint_head_ + 1) % notification_hints_.size();
    --notification_hint_count_;
    return true;
}

/**
 * @brief 在网络可用时按当前指数退避间隔安排业务 EMQX 重连。
 */
void BackendService::ScheduleNotificationMqttReconnect()
{
    if (!network_connected_.load() || notification_reconnect_timer_ == nullptr)
    {
        return;
    }
    const uint32_t delay_seconds = notification_reconnect_delay_seconds_.load();
    esp_timer_stop(notification_reconnect_timer_);
    const esp_err_t error = esp_timer_start_once(
        notification_reconnect_timer_,
        static_cast<uint64_t>(delay_seconds) * 1000ULL * 1000ULL);
    if (error != ESP_OK)
    {
        ESP_LOGW(kTag, "业务EMQX重连定时器启动失败，原因=%s", esp_err_to_name(error));
        return;
    }
    notification_reconnect_delay_seconds_.store(
        std::min(delay_seconds * 2, kNotificationReconnectMaximumSeconds));
    ESP_LOGW(kTag, "业务EMQX将在%u秒后重连", static_cast<unsigned>(delay_seconds));
}

/**
 * @brief 获取业务 EMQX 独立凭据并订阅当前设备唯一通知主题。
 * @return 连接和订阅成功时返回 true。
 */
bool BackendService::ConnectNotificationMqtt()
{
    std::string access_token;
    {
        std::lock_guard<std::mutex> lock(binding_mutex_);
        access_token = device_access_token_;
    }
    const HttpResponse response = SendJsonRequest(
        "GET",
        kNotificationMqttConfigPath,
        access_token,
        "",
        kNotificationResponseMaxBytes);
    if (!response.transport_succeeded || response.status_code != 200)
    {
        ESP_LOGW(kTag, "业务EMQX配置获取失败，HTTP状态码=%d", response.status_code);
        return false;
    }
    cJSON *root = nullptr;
    cJSON *data = nullptr;
    std::string message;
    if (!ParseSuccessData(response.body, &root, &data, message))
    {
        cJSON_Delete(root);
        ESP_LOGW(kTag, "业务EMQX配置响应无效");
        return false;
    }
    std::string host;
    std::string client_id;
    std::string username;
    std::string password;
    std::string topic;
    std::string heartbeat_topic;
    cJSON *port = cJSON_GetObjectItemCaseSensitive(data, "port");
    cJSON *use_tls = cJSON_GetObjectItemCaseSensitive(data, "use_tls");
    const bool valid = ReadBoundedString(data, "host", 255, host) &&
                       ReadBoundedString(data, "client_id", 128, client_id) &&
                       ReadBoundedString(data, "username", 256, username) &&
                       ReadBoundedString(data, "password", 256, password) &&
                       ReadBoundedString(data, "topic", 255, topic) &&
                       ReadBoundedString(data, "heartbeat_topic", 255, heartbeat_topic) &&
                       cJSON_IsNumber(port) && port->valueint > 0 && port->valueint <= 65535 &&
                       cJSON_IsTrue(use_tls);
    const int broker_port = cJSON_IsNumber(port) ? port->valueint : 0;
    cJSON_Delete(root);
    if (!valid)
    {
        ESP_LOGW(kTag, "业务EMQX配置字段校验失败");
        return false;
    }

    auto *network = Board::GetInstance().GetNetwork();
    if (network == nullptr)
    {
        return false;
    }
    auto mqtt = network->CreateMqtt(1);
    if (mqtt == nullptr)
    {
        return false;
    }
    Mqtt *mqtt_client = mqtt.get();
    mqtt->SetKeepAlive(120);
    mqtt->OnMessage([this, topic](const std::string &received_topic,
                                 const std::string &payload)
                    {
        if (received_topic != topic || payload.size() > 1024)
        {
            return;
        }
        cJSON *message_root = cJSON_Parse(payload.c_str());
        cJSON *type = message_root == nullptr
            ? nullptr
            : cJSON_GetObjectItemCaseSensitive(message_root, "type");
        cJSON *revision = message_root == nullptr
            ? nullptr
            : cJSON_GetObjectItemCaseSensitive(message_root, "revision");
        const bool memo_changed = cJSON_IsString(type)
            && std::strcmp(type->valuestring, "memo.changed") == 0
            && cJSON_IsNumber(revision)
            && revision->valueint == 1;
        if (memo_changed)
        {
            cJSON_Delete(message_root);
            memo_refresh_requested_.store(true);
            ESP_LOGI(kTag, "收到备忘录变更提示，准备刷新屏保备忘录");
            Application::GetInstance().Schedule([this]()
            {
                StartMemoSync(true);
            });
            return;
        }

        const bool weather_changed = cJSON_IsString(type)
            && std::strcmp(type->valuestring, "weather.changed") == 0
            && cJSON_IsNumber(revision)
            && revision->valueint == 1;
        if (weather_changed)
        {
            cJSON_Delete(message_root);
            weather_refresh_requested_.store(true);
            ESP_LOGI(kTag, "收到天气设置变更提示，准备刷新屏保天气");
            Application::GetInstance().Schedule([this]()
            {
                StartWeatherSync(true);
            });
            return;
        }

        const bool mcp_changed = cJSON_IsString(type)
            && std::strcmp(type->valuestring, "mcp.changed") == 0
            && cJSON_IsNumber(revision)
            && revision->valueint == 1;
        if (mcp_changed)
        {
            cJSON_Delete(message_root);
            ESP_LOGI(kTag, "收到 MCP 清单变更提示，准备刷新自定义 MCP");
            Application::GetInstance().Schedule([this]()
            {
                StartMcpManifestSync();
            });
            return;
        }

        std::string notification_id;
        std::string delivery_id;
        const bool notification_ready = cJSON_IsString(type)
            && std::strcmp(type->valuestring, "notification.ready") == 0
            && cJSON_IsNumber(revision)
            && revision->valueint == 1
            && ReadBoundedString(message_root, "notification_id", 50, notification_id)
            && ReadBoundedString(message_root, "delivery_id", 50, delivery_id);
        cJSON_Delete(message_root);
        if (!notification_ready)
        {
            ESP_LOGW(kTag, "收到的主动通知提示格式无效，已等待HTTPS补偿");
            return;
        }
        if (EnqueueNotificationHint(notification_id, delivery_id))
        {
            ESP_LOGI(kTag, "主动通知提示已入队，通知标识=%s，投递标识=%s",
                     notification_id.c_str(), delivery_id.c_str());
            StartNotificationSync(false);
        }
    });
    mqtt->OnConnected([this, topic, mqtt_client]()
                      {
        if (active_notification_mqtt_.load() != mqtt_client)
        {
            return;
        }
        if (!mqtt_client->Subscribe(topic, 1))
        {
            ESP_LOGW(kTag, "业务EMQX自动重连后订阅失败");
            ScheduleNotificationMqttReconnect();
            return;
        }
        notification_reconnect_delay_seconds_.store(kNotificationReconnectInitialSeconds);
        if (notification_reconnect_timer_ != nullptr)
        {
            esp_timer_stop(notification_reconnect_timer_);
        }
        StartHeartbeatPublishing();
        ESP_LOGI(kTag, "业务EMQX已自动重连并恢复设备通知订阅");
        weather_refresh_requested_.store(true);
        Application::GetInstance().Schedule([this]()
        {
            StartWeatherSync(true);
        });
        StartNotificationSync(false);
    });
    mqtt->OnDisconnected([this, mqtt_client]()
                         {
        if (active_notification_mqtt_.load() != mqtt_client)
        {
            return;
        }
        StopHeartbeatPublishing();
        ESP_LOGW(kTag, "业务EMQX连接已断开，准备指数退避重连");
        ScheduleNotificationMqttReconnect();
    });
    if (!mqtt->Connect(host, broker_port, client_id, username, password))
    {
        ESP_LOGW(kTag, "业务EMQX连接失败，错误码=0x%x", mqtt->GetLastError());
        return false;
    }
    if (!mqtt->Subscribe(topic, 1))
    {
        ESP_LOGW(kTag, "业务EMQX主题订阅失败");
        mqtt->Disconnect();
        return false;
    }
    std::unique_ptr<Mqtt> previous_mqtt;
    {
        std::lock_guard<std::mutex> lock(notification_mutex_);
        previous_mqtt = std::move(notification_mqtt_);
        notification_mqtt_ = std::move(mqtt);
        active_notification_mqtt_.store(notification_mqtt_.get());
        heartbeat_topic_ = heartbeat_topic;
    }
    if (previous_mqtt != nullptr)
    {
        previous_mqtt->Disconnect();
    }
    notification_reconnect_delay_seconds_.store(kNotificationReconnectInitialSeconds);
    if (notification_reconnect_timer_ != nullptr)
    {
        esp_timer_stop(notification_reconnect_timer_);
    }
    StartHeartbeatPublishing();
    ESP_LOGI(kTag, "业务EMQX连接成功并已订阅当前设备通知主题");
    weather_refresh_requested_.store(true);
    Application::GetInstance().Schedule([this]()
    {
        StartWeatherSync(true);
    });
    return true;
}

/**
 * @brief 根据设备状态直接播放、询问确认或延迟当前通知。
 * @param notification 当前通知快照。
 */
void BackendService::HandlePendingNotification(const PendingNotification &notification)
{
    auto &app = Application::GetInstance();
    AckNotification(notification, kNotificationAckReceived);
    if (app.GetDeviceState() != kDeviceStateIdle)
    {
        AckNotification(notification, kNotificationAckDeferred);
        ESP_LOGI(kTag, "设备当前忙碌，主动通知已延迟");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(notification_mutex_);
        pending_notification_ = notification;
    }
    if (notification.notification_mode == kNotificationModeDirect)
    {
        StartPendingNotificationPlayback();
        return;
    }
    app.Schedule([notification]()
                 {
        auto &board = Board::GetInstance();
        board.WakeUpScreen(true);
        auto *display = board.GetDisplay();
        if (display != nullptr)
        {
            display->SetScreensaverMode(false);
            display->SetStatus(notification.source_title.c_str());
            display->SetChatMessage("assistant", "您有一个任务通知，是否需要播报？");
        }
    });
    StartNotificationConfirmation();
}

/**
 * @brief 创建确认模式的内置询问语和超时任务。
 */
void BackendService::StartNotificationConfirmation()
{
    std::lock_guard<std::mutex> lock(notification_mutex_);
    if (!pending_notification_.valid || notification_confirmation_task_handle_ != nullptr)
    {
        return;
    }
    if (CreatePsramBackendTask(
            NotificationConfirmationTaskEntry,
            "notify_confirm",
            kNotificationTaskStackSize,
            this,
            kNotificationTaskPriority,
            &notification_confirmation_task_handle_) != pdPASS)
    {
        notification_confirmation_task_handle_ = nullptr;
        ESP_LOGE(kTag, "主动通知确认任务创建失败");
    }
}

/**
 * @brief 主动通知确认任务入口。
 */
void BackendService::NotificationConfirmationTaskEntry(void *context)
{
    auto *service = static_cast<BackendService *>(context);
    service->RunNotificationConfirmation();
    bool continue_sync = false;
    {
        std::lock_guard<std::mutex> lock(service->notification_mutex_);
        service->notification_confirmation_task_handle_ = nullptr;
        continue_sync = (service->notification_hint_count_ > 0
                         || service->notification_reconnect_requested_.load())
            && service->notification_playback_task_handle_ == nullptr;
    }
    if (continue_sync)
    {
        service->StartNotificationSync(false);
    }
    DeleteCurrentPsramTask();
}

/**
 * @brief 播放固件内置确认语，以自动停止模式监听回答，并在超时后结束监听和延迟投递。
 */
void BackendService::RunNotificationConfirmation()
{
    auto &app = Application::GetInstance();
    app.PlaySound(Lang::Sounds::OGG_NOTIFICATION_PROMPT);
    app.GetAudioService().WaitForPlaybackQueueEmpty();
    app.Schedule([]()
    {
        Application::GetInstance().StartListening(kListeningModeAutoStop);
    });
    vTaskDelay(pdMS_TO_TICKS(kNotificationConfirmationTimeoutMs));

    PendingNotification notification;
    {
        std::lock_guard<std::mutex> lock(notification_mutex_);
        if (!pending_notification_.valid || notification_playback_task_handle_ != nullptr)
        {
            return;
        }
        notification = pending_notification_;
        pending_notification_ = PendingNotification{};
    }
    const DeviceState state = app.GetDeviceState();
    if (state == kDeviceStateConnecting || state == kDeviceStateListening)
    {
        app.StopListening();
    }
    AckNotification(notification, kNotificationAckDeferred);
    ESP_LOGI(kTag, "主动通知确认超时，已结束监听并延迟到下一次允许询问时间");
}

/**
 * @brief 创建等待设备空闲后下载并播放当前通知的独立任务。
 */
void BackendService::StartPendingNotificationPlayback()
{
    std::lock_guard<std::mutex> lock(notification_mutex_);
    if (!pending_notification_.valid || notification_playback_task_handle_ != nullptr)
    {
        return;
    }
    notification_playback_interrupted_.store(false);
    notification_playback_active_.store(true);
    if (CreatePsramBackendTask(
            NotificationPlaybackTaskEntry,
            "notify_play",
            kNotificationPlaybackTaskStackSize,
            this,
            kNotificationTaskPriority,
            &notification_playback_task_handle_) != pdPASS)
    {
        notification_playback_task_handle_ = nullptr;
        notification_playback_active_.store(false);
        ESP_LOGE(kTag, "主动通知播放任务创建失败");
    }
}

/**
 * @brief 主动通知音频播放任务入口。
 * @param context 指向当前 BackendService 单例。
 */
void BackendService::NotificationPlaybackTaskEntry(void *context)
{
    auto *service = static_cast<BackendService *>(context);
    service->RunPendingNotificationPlayback();
    service->notification_playback_active_.store(false);
    bool continue_sync = false;
    {
        std::lock_guard<std::mutex> lock(service->notification_mutex_);
        service->notification_playback_task_handle_ = nullptr;
        continue_sync = !service->pending_notification_.valid
            && Application::GetInstance().GetDeviceState() == kDeviceStateIdle;
    }
    if (continue_sync)
    {
        service->StartNotificationSync(false);
    }
    bool refresh_manifest = false;
    {
        std::lock_guard<std::mutex> lock(service->dynamic_mcp_mutex_);
        refresh_manifest = service->mcp_manifest_refresh_requested_;
        service->mcp_manifest_refresh_requested_ = false;
    }
    if (refresh_manifest)
    {
        service->StartMcpManifestSync();
    }
    if (service->weather_refresh_requested_.load())
    {
        service->StartWeatherSync(true);
    }
    if (service->memo_refresh_requested_.load() || service->memo_retry_due_.load())
    {
        service->StartMemoSync(true);
    }
    DeleteCurrentPsramTask();
}

/**
 * @brief 通过单条 HTTP 连接流式下载并实时解封装通知 Ogg/Opus 音频。
 * @param path 当前通知音频的 API 相对路径。
 * @param access_token 当前绑定设备的 Bearer Token。
 * @return 至少成功送入一个 Opus 包且传输未失败、未被用户中断时返回 true。
 */
bool BackendService::StreamNotificationAudio(
    const std::string &path,
    const std::string &access_token)
{
    BackendHttpLease request_lock(true);
    auto *network = Board::GetInstance().GetNetwork();
    if (network == nullptr)
    {
        return false;
    }

    OggDemuxer demuxer;
    size_t packet_count = 0;
    bool queue_succeeded = true;
    demuxer.OnDemuxerFinished(
        [this, &packet_count, &queue_succeeded](
            const uint8_t *data,
            int sample_rate,
            int frame_duration,
            size_t size)
        {
            if (notification_playback_interrupted_.load())
            {
                return;
            }
            if (Application::GetInstance().GetAudioService().PushDataToDecodeQueue(
                    data, size, sample_rate, frame_duration, true))
            {
                ++packet_count;
            }
            else
            {
                queue_succeeded = false;
            }
        });

    auto http = network->CreateHttp();
    if (http == nullptr)
    {
        return false;
    }
    http->SetTimeout(kHttpTimeoutMs);
    http->SetHeader("Accept", "audio/ogg");
    http->SetHeader("Authorization", "Bearer " + access_token);
    http->SetHeader("User-Agent", SystemInfo::GetUserAgent());
    http->SetHeader(kFirmwareVersionHeader, esp_app_get_description()->version);
    if (!http->Open("GET", BuildApiUrl(path.c_str())))
    {
        ESP_LOGW(kTag, "通知音频请求失败，错误码=0x%x", http->GetLastError());
        return false;
    }

    const int status_code = http->GetStatusCode();
    if (status_code != 200)
    {
        ESP_LOGW(kTag, "通知音频响应无效，HTTP状态码=%d", status_code);
        http->Close();
        return false;
    }
    const size_t declared_length = http->GetBodyLength();
    if (declared_length > kNotificationAudioMaxBytes)
    {
        ESP_LOGW(kTag, "通知音频响应超过限制，字节数=%u",
                 static_cast<unsigned>(declared_length));
        http->Close();
        return false;
    }

    size_t received_size = 0;
    std::array<char, kNotificationAudioReadChunkBytes> read_buffer{};
    while (true)
    {
        if (notification_playback_interrupted_.load())
        {
            http->Close();
            return false;
        }
        const int read_size = http->Read(read_buffer.data(), read_buffer.size());
        if (read_size < 0)
        {
            ESP_LOGW(kTag, "通知音频响应读取失败");
            http->Close();
            return false;
        }
        if (read_size == 0)
        {
            break;
        }
        received_size += static_cast<size_t>(read_size);
        if (received_size > kNotificationAudioMaxBytes
            || demuxer.Process(
                reinterpret_cast<const uint8_t *>(read_buffer.data()),
                static_cast<size_t>(read_size)) != static_cast<size_t>(read_size))
        {
            ESP_LOGW(kTag, "通知音频流超过限制或Ogg解析失败");
            http->Close();
            return false;
        }
    }
    http->Close();
    if (received_size == 0 || (declared_length != 0 && received_size != declared_length))
    {
        ESP_LOGW(kTag, "通知音频响应长度不完整");
        return false;
    }

    return queue_succeeded
        && packet_count > 0
        && !notification_playback_interrupted_.load();
}

/**
 * @brief 下载和播放当前通知音频，并确认开始与完成状态。
 */
void BackendService::RunPendingNotificationPlayback()
{
    PendingNotification notification;
    {
        std::lock_guard<std::mutex> lock(notification_mutex_);
        notification = pending_notification_;
    }
    if (!notification.valid)
    {
        return;
    }
    for (int attempt = 0; attempt < 120; ++attempt)
    {
        if (notification_playback_interrupted_.load())
        {
            break;
        }
        auto &app = Application::GetInstance();
        if (app.GetDeviceState() == kDeviceStateIdle && app.GetAudioService().IsIdle())
        {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle)
    {
        AckNotification(notification, kNotificationAckDeferred);
        {
            std::lock_guard<std::mutex> lock(notification_mutex_);
            if (pending_notification_.delivery_id == notification.delivery_id)
            {
                pending_notification_ = PendingNotification{};
            }
        }
        return;
    }

    AckNotification(notification, kNotificationAckPlaying);
    Application::GetInstance().Schedule([notification]()
                                        {
        auto &board = Board::GetInstance();
        board.WakeUpScreen(true);
        auto *display = board.GetDisplay();
        if (display != nullptr)
        {
            display->SetAudioPlaybackMode(true);
            display->SetScreensaverMode(false);
            display->SetStatus(notification.source_title.c_str());
            display->SetChatMessage("assistant", notification.display_text.c_str());
        }
    });

    bool played = !notification.has_audio;
    if (notification.has_audio)
    {
        std::string access_token;
        {
            std::lock_guard<std::mutex> lock(binding_mutex_);
            access_token = device_access_token_;
        }
        played = true;
        for (int segment_number = 1;
             segment_number <= notification.audio_segment_count;
             ++segment_number)
        {
            if (notification_playback_interrupted_.load())
            {
                played = false;
                break;
            }
            std::string audio_path =
                "/api/device/notifications/" + notification.notification_id + "/audio";
            if (segment_number > 1)
            {
                audio_path += "/" + std::to_string(segment_number);
            }
            auto &audio_service = Application::GetInstance().GetAudioService();
            audio_service.ResetDecoder();
            audio_service.BeginStreamPlayback(kNotificationAudioPrebufferDurationMs);
            const bool streamed = StreamNotificationAudio(audio_path, access_token);
            if (streamed && !notification_playback_interrupted_.load())
            {
                audio_service.FinishStreamInput();
                audio_service.WaitForPlaybackQueueEmpty();
            }
            else
            {
                audio_service.ResetDecoder();
            }
            audio_service.EndStreamPlayback();
            if (!streamed)
            {
                played = false;
                break;
            }
        }
        if (!played && !notification_playback_interrupted_.load())
        {
            Application::GetInstance().GetAudioService().ResetDecoder();
        }
    }
    else
    {
        vTaskDelay(pdMS_TO_TICKS(3000));
    }

    const bool interrupted = notification_playback_interrupted_.load();
    AckNotification(notification, played || interrupted
        ? kNotificationAckCompleted
        : kNotificationAckFailed);
    {
        std::lock_guard<std::mutex> lock(notification_mutex_);
        if (pending_notification_.delivery_id == notification.delivery_id)
        {
            pending_notification_ = PendingNotification{};
        }
    }
    Application::GetInstance().Schedule([]()
                                        {
        auto &app = Application::GetInstance();
        auto *display = Board::GetInstance().GetDisplay();
        if (display != nullptr)
        {
            if (app.GetDeviceState() == kDeviceStateIdle)
            {
                display->SetScreensaverMode(true);
            }
            display->SetAudioPlaybackMode(false);
        }
    });
}

/**
 * @brief 向后端确认当前通知投递状态。
 */
bool BackendService::AckNotification(
    const PendingNotification &notification,
    int ack_type)
{
    std::string access_token;
    {
        std::lock_guard<std::mutex> lock(binding_mutex_);
        access_token = device_access_token_;
    }
    const std::string body = BuildNotificationAckJson(
        notification.delivery_id,
        ack_type);
    const std::string path =
        "/api/device/notifications/" + notification.notification_id + "/ack";
    const HttpResponse response = SendJsonRequest(
        "POST",
        path.c_str(),
        access_token,
        body,
        kNotificationResponseMaxBytes);
    std::string response_message;
    const bool succeeded = response.transport_succeeded &&
        response.status_code == 200 &&
        ParseSuccessEnvelope(response.body, response_message);
    if (!succeeded)
    {
        ESP_LOGW(kTag, "主动通知状态确认失败，动作=%d，HTTP状态码=%d",
                 ack_type, response.status_code);
    }
    return succeeded;
}

/**
 * @brief 五分钟 HTTPS 通知补偿同步定时器回调。
 * @param context 指向当前 BackendService 单例。
 */
void BackendService::NotificationTimerCallback(void *context)
{
    auto *service = static_cast<BackendService *>(context);
    service->StartNotificationSync(false);
}

/**
 * @brief 业务 EMQX 指数退避重连定时器回调。
 * @param context 指向当前 BackendService 单例。
 */
void BackendService::NotificationReconnectTimerCallback(void *context)
{
    auto *service = static_cast<BackendService *>(context);
    service->StartNotificationSync(true);
}

/**
 * @brief 启动两分钟心跳周期并立即安排一次心跳。
 */
void BackendService::StartHeartbeatPublishing()
{
    if (heartbeat_timer_ != nullptr)
    {
        esp_timer_stop(heartbeat_timer_);
        const esp_err_t error = esp_timer_start_periodic(
            heartbeat_timer_, kHeartbeatIntervalUs);
        if (error != ESP_OK)
        {
            ESP_LOGW(kTag, "设备心跳定时器启动失败，原因=%s", esp_err_to_name(error));
        }
    }
    EnqueueHeartbeat();
}

/**
 * @brief 停止业务 EMQX 心跳周期。
 */
void BackendService::StopHeartbeatPublishing()
{
    if (heartbeat_timer_ != nullptr)
    {
        esp_timer_stop(heartbeat_timer_);
    }
    heartbeat_job_pending_.store(false);
}

/**
 * @brief 在没有待处理心跳时把一次发布加入常驻后端 Worker。
 */
void BackendService::EnqueueHeartbeat()
{
    if (!network_connected_.load()
        || heartbeat_job_pending_.exchange(true))
    {
        return;
    }
    if (!EnqueueBackendJob(BackendJobType::Heartbeat))
    {
        heartbeat_job_pending_.store(false);
        ESP_LOGW(kTag, "后端 Worker 队列已满，无法安排设备心跳");
    }
}

/**
 * @brief 两分钟设备心跳定时器回调，只负责安排发布任务。
 * @param context 指向当前 BackendService 单例。
 */
void BackendService::HeartbeatTimerCallback(void *context)
{
    auto *service = static_cast<BackendService *>(context);
    service->EnqueueHeartbeat();
}
