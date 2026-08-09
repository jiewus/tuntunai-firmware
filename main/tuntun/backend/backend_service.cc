/**
 * @file backend_service.cc
 * @brief 囤囤AI后端常驻 Worker、MCP 注册和连接生命周期实现。
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
 * @brief 获取固件生命周期内唯一的业务后端服务。
 * @return BackendService 单例引用。
 */
BackendService &BackendService::GetInstance()
{
    static BackendService instance;
    return instance;
}

/**
 * @brief 把排队的后端操作逐个取出并执行。
 * @param context 指向 BackendService 单例。
 * @details Worker 常驻运行并通过任务通知休眠；单个任务异常只记录日志，不会退出 Worker，
 *          从而保证后续天气、备忘录、MCP 和通知请求仍能继续处理。
 */
void BackendService::BackendWorkerTaskEntry(void *context)
{
    auto *service = static_cast<BackendService *>(context);
    while (true)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (true)
        {
            BackendJob job;
            {
                std::lock_guard<std::mutex> lock(service->backend_job_mutex_);
                if (service->backend_job_count_ == 0)
                {
                    break;
                }
                job = service->backend_jobs_[service->backend_job_head_];
                service->backend_jobs_[service->backend_job_head_] = {};
                service->backend_job_head_ =
                    (service->backend_job_head_ + 1) % kBackendWorkerQueueCapacity;
                --service->backend_job_count_;
            }
            try
            {
                service->ExecuteBackendJob(job);
            }
            catch (const std::exception &exception)
            {
                ESP_LOGE(kTag, "后端常驻 Worker 任务异常，原因=%s", exception.what());
            }
            catch (...)
            {
                ESP_LOGE(kTag, "后端常驻 Worker 任务发生未知异常");
            }
        }
    }
}

/**
 * @brief 把一个非实时后端操作加入常驻 Worker 的固定环形队列。
 * @param type 要执行的后端任务类型。
 * @param context 任务上下文；无上下文任务传入空指针。
 * @return 成功入队时返回 true；Worker 不可用或队列已满时返回 false。
 */
bool BackendService::EnqueueBackendJob(BackendJobType type, void* context)
{
    if (backend_worker_task_handle_ == nullptr)
    {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(backend_job_mutex_);
        if (backend_job_count_ >= kBackendWorkerQueueCapacity)
        {
            return false;
        }
        backend_jobs_[backend_job_tail_] = {type, context};
        backend_job_tail_ = (backend_job_tail_ + 1) % kBackendWorkerQueueCapacity;
        ++backend_job_count_;
    }
    xTaskNotifyGive(backend_worker_task_handle_);
    return true;
}

/**
 * @brief 执行动态 MCP 清单同步任务并释放调度标记。
 */
void BackendService::ExecuteMcpManifestJob()
{
    try
    {
        RunMcpManifestSync();
    }
    catch (const std::exception &exception)
    {
        ESP_LOGE(kTag, "MCP 清单同步任务异常，原因=%s", exception.what());
    }
    catch (...)
    {
        ESP_LOGE(kTag, "MCP 清单同步任务发生未知异常");
    }
    std::lock_guard<std::mutex> lock(dynamic_mcp_mutex_);
    mcp_manifest_task_handle_ = nullptr;
}

/**
 * @brief 执行动态 MCP 工具并释放任务上下文和调度标记。
 * @param context 动态 MCP 工具执行上下文。
 */
void BackendService::ExecuteDynamicToolJob(DynamicToolTaskContext* context)
{
    std::unique_ptr<DynamicToolTaskContext> task_context(context);
    try
    {
        RunDynamicToolExecution(
            task_context->tool_name,
            task_context->tool_revision,
            task_context->completion);
    }
    catch (const std::exception &exception)
    {
        ESP_LOGE(kTag, "动态 MCP 工具执行异常，原因=%s", exception.what());
        FinishToolRequest(task_context->completion, kDynamicToolFallbackError, true);
    }
    catch (...)
    {
        ESP_LOGE(kTag, "动态 MCP 工具执行发生未知异常");
        FinishToolRequest(task_context->completion, kDynamicToolFallbackError, true);
    }
    if (task_context->completion)
    {
        FinishToolRequest(task_context->completion, kDynamicToolFallbackError, true);
    }
    bool refresh_manifest = false;
    {
        std::lock_guard<std::mutex> lock(dynamic_mcp_mutex_);
        dynamic_tool_task_handle_ = nullptr;
        refresh_manifest = mcp_manifest_refresh_requested_;
        mcp_manifest_refresh_requested_ = false;
    }
    if (refresh_manifest)
    {
        StartMcpManifestSync();
    }
}

/**
 * @brief 执行备忘录语音操作并释放任务上下文。
 * @param context 备忘录语音操作上下文。
 */
void BackendService::ExecuteMemoToolJob(MemoToolTaskContext* context)
{
    std::unique_ptr<MemoToolTaskContext> task_context(context);
    try
    {
        RunMemoToolTask(*task_context);
    }
    catch (const std::exception &exception)
    {
        ESP_LOGE(kTag, "备忘录语音任务异常，原因=%s", exception.what());
        FinishToolRequest(task_context->completion, "备忘录操作失败，请稍后重试。", true);
    }
    catch (...)
    {
        ESP_LOGE(kTag, "备忘录语音任务发生未知异常");
        FinishToolRequest(task_context->completion, "备忘录操作失败，请稍后重试。", true);
    }
    if (task_context->completion)
    {
        FinishToolRequest(task_context->completion, "备忘录操作未能完成，请稍后重试。", true);
    }
    {
        std::lock_guard<std::mutex> lock(memo_mutex_);
        memo_task_handle_ = nullptr;
    }
    if ((memo_refresh_requested_.load() || memo_retry_due_.load())
        && screensaver_active_.load())
    {
        StartMemoSync(true);
    }
}

/**
 * @brief 执行自定义提醒语音操作并释放任务上下文。
 * @param context 自定义提醒语音操作上下文。
 */
void BackendService::ExecuteCustomReminderToolJob(CustomReminderToolTaskContext* context)
{
    std::unique_ptr<CustomReminderToolTaskContext> task_context(context);
    try
    {
        RunCustomReminderToolTask(*task_context);
    }
    catch (const std::exception &exception)
    {
        ESP_LOGE(kTag, "自定义提醒语音任务异常，原因=%s", exception.what());
        FinishToolRequest(task_context->completion, "自定义提醒操作失败，请稍后重试。", true);
    }
    catch (...)
    {
        ESP_LOGE(kTag, "自定义提醒语音任务发生未知异常");
        FinishToolRequest(task_context->completion, "自定义提醒操作失败，请稍后重试。", true);
    }
    if (task_context->completion)
    {
        FinishToolRequest(
            task_context->completion,
            "自定义提醒操作未能完成，请稍后重试。",
            true);
    }
    std::lock_guard<std::mutex> lock(custom_reminder_mutex_);
    custom_reminder_task_handle_ = nullptr;
}

/**
 * @brief 执行每日新闻简报请求并释放任务上下文和占用标记。
 */
void BackendService::ExecuteNewsBriefingJob(NewsBriefingTaskContext* context)
{
    std::unique_ptr<NewsBriefingTaskContext> task_context(context);
    try
    {
        RunNewsBriefingTask(task_context->completion);
    }
    catch (const std::exception &exception)
    {
        ESP_LOGE(kTag, "每日新闻简报任务异常，原因=%s", exception.what());
        FinishToolRequest(task_context->completion, "每日新闻简报获取失败，请稍后重试。", true);
    }
    catch (...)
    {
        ESP_LOGE(kTag, "每日新闻简报任务发生未知异常");
        FinishToolRequest(task_context->completion, "每日新闻简报获取失败，请稍后重试。", true);
    }
    if (task_context->completion)
    {
        FinishToolRequest(task_context->completion, "每日新闻简报未能完成，请稍后重试。", true);
    }
    std::lock_guard<std::mutex> lock(dynamic_mcp_mutex_);
    news_briefing_task_handle_ = nullptr;
}

/**
 * @brief 执行备忘录屏保同步任务。
 */
void BackendService::ExecuteMemoSyncJob()
{
    try
    {
        RunMemoSync();
    }
    catch (const std::exception &exception)
    {
        ESP_LOGE(kTag, "备忘录同步任务异常，原因=%s", exception.what());
        ShowMemoStatusIfUnavailable("备忘录同步失败");
        ScheduleMemoRetry();
    }
    catch (...)
    {
        ESP_LOGE(kTag, "备忘录同步任务发生未知异常");
        ShowMemoStatusIfUnavailable("备忘录同步失败");
        ScheduleMemoRetry();
    }
    bool retry_or_refresh = false;
    {
        std::lock_guard<std::mutex> lock(memo_mutex_);
        memo_task_handle_ = nullptr;
        retry_or_refresh = memo_refresh_requested_.load() || memo_retry_due_.load();
    }
    if (retry_or_refresh && screensaver_active_.load())
    {
        StartMemoSync(true);
    }
}

/**
 * @brief 执行天气屏保同步任务。
 */
void BackendService::ExecuteWeatherSyncJob()
{
    try
    {
        RunWeatherSync();
    }
    catch (const std::exception &exception)
    {
        ESP_LOGE(kTag, "天气同步任务异常，原因=%s", exception.what());
        ShowWeatherStatusIfUnavailable("天气同步失败");
    }
    catch (...)
    {
        ESP_LOGE(kTag, "天气同步任务发生未知异常");
        ShowWeatherStatusIfUnavailable("天气同步失败");
    }
    bool refresh_requested = false;
    {
        std::lock_guard<std::mutex> lock(weather_mutex_);
        weather_task_handle_ = nullptr;
        refresh_requested = weather_refresh_requested_.load();
    }
    if (screensaver_active_.load())
    {
        if (refresh_requested)
        {
            StartWeatherSync(true);
        }
        else
        {
            StartMemoSync(false);
        }
    }
}

/**
 * @brief 执行天气城市设置任务并释放上下文。
 * @param context 天气城市设置上下文。
 */
void BackendService::ExecuteWeatherLocationJob(WeatherLocationTaskContext* context)
{
    std::unique_ptr<WeatherLocationTaskContext> task_context(context);
    try
    {
        RunWeatherLocationTask(
            task_context->location_name,
            task_context->use_ip_auto,
            task_context->completion);
    }
    catch (const std::exception &exception)
    {
        ESP_LOGE(kTag, "天气位置设置任务异常，原因=%s", exception.what());
        FinishToolRequest(task_context->completion, "天气城市设置失败，请稍后重试。", true);
    }
    catch (...)
    {
        ESP_LOGE(kTag, "天气位置设置任务发生未知异常");
        FinishToolRequest(task_context->completion, "天气城市设置失败，请稍后重试。", true);
    }
    std::lock_guard<std::mutex> lock(weather_mutex_);
    weather_location_task_handle_ = nullptr;
}

/**
 * @brief 执行天气播报设置任务并释放上下文。
 * @param context 天气播报设置上下文。
 */
void BackendService::ExecuteWeatherAnnouncementJob(WeatherAnnouncementTaskContext* context)
{
    std::unique_ptr<WeatherAnnouncementTaskContext> task_context(context);
    try
    {
        RunWeatherAnnouncementTask(
            task_context->enabled,
            task_context->announcement_time,
            task_context->completion);
    }
    catch (const std::exception &exception)
    {
        ESP_LOGE(kTag, "天气播报设置任务异常，原因=%s", exception.what());
        FinishToolRequest(task_context->completion, "天气播报设置失败，请稍后重试。", true);
    }
    catch (...)
    {
        ESP_LOGE(kTag, "天气播报设置任务发生未知异常");
        FinishToolRequest(task_context->completion, "天气播报设置失败，请稍后重试。", true);
    }
    std::lock_guard<std::mutex> lock(weather_mutex_);
    weather_announcement_task_handle_ = nullptr;
}

/**
 * @brief 执行主动通知同步任务。
 */
void BackendService::ExecuteNotificationSyncJob()
{
    notification_reconnect_requested_.exchange(false);
    try
    {
        RunNotificationSync();
    }
    catch (const std::exception &exception)
    {
        ESP_LOGE(kTag, "主动通知同步任务异常，原因=%s", exception.what());
    }
    catch (...)
    {
        ESP_LOGE(kTag, "主动通知同步任务发生未知异常");
    }
    bool continue_sync = false;
    {
        std::lock_guard<std::mutex> lock(notification_mutex_);
        notification_task_handle_ = nullptr;
        continue_sync = (notification_hint_count_ > 0
                          || notification_reconnect_requested_.load())
            && notification_playback_task_handle_ == nullptr
            && notification_confirmation_task_handle_ == nullptr;
    }
    if (continue_sync)
    {
        StartNotificationSync(false);
    }
}

/**
 * @brief 通过业务 EMQX 发布一次设备心跳。
 * @details 心跳不等待业务响应；发布动作在后端常驻 Worker 中执行，避免阻塞定时器任务和界面线程。
 */
void BackendService::ExecuteHeartbeatJob()
{
    bool published = false;
    {
        std::lock_guard<std::mutex> lock(notification_mutex_);
        if (notification_mqtt_ != nullptr
            && notification_mqtt_->IsConnected()
            && !heartbeat_topic_.empty())
        {
            published = notification_mqtt_->Publish(
                heartbeat_topic_, "{\"type\":\"device.heartbeat\",\"revision\":1}", 0);
        }
    }
    if (published)
    {
        ESP_LOGD(kTag, "设备 MQTT 心跳已发送");
    }
    else if (network_connected_.load())
    {
        ESP_LOGW(kTag, "设备 MQTT 心跳发送失败，等待下一周期重试");
    }
    heartbeat_job_pending_.store(false);
}

/**
 * @brief 根据固定任务类型分派常驻 Worker 工作。
 * @param job 从固定环形队列取出的任务。
 */
void BackendService::ExecuteBackendJob(const BackendJob& job)
{
    switch (job.type)
    {
    case BackendJobType::McpManifest:
        ExecuteMcpManifestJob();
        break;
    case BackendJobType::DynamicTool:
        ExecuteDynamicToolJob(static_cast<DynamicToolTaskContext*>(job.context));
        break;
    case BackendJobType::MemoTool:
        ExecuteMemoToolJob(static_cast<MemoToolTaskContext*>(job.context));
        break;
    case BackendJobType::CustomReminderTool:
        ExecuteCustomReminderToolJob(static_cast<CustomReminderToolTaskContext*>(job.context));
        break;
    case BackendJobType::MemoSync:
        ExecuteMemoSyncJob();
        break;
    case BackendJobType::WeatherSync:
        ExecuteWeatherSyncJob();
        break;
    case BackendJobType::WeatherLocation:
        ExecuteWeatherLocationJob(static_cast<WeatherLocationTaskContext*>(job.context));
        break;
    case BackendJobType::WeatherAnnouncement:
        ExecuteWeatherAnnouncementJob(
            static_cast<WeatherAnnouncementTaskContext*>(job.context));
        break;
    case BackendJobType::NotificationSync:
        ExecuteNotificationSyncJob();
        break;
    case BackendJobType::Heartbeat:
        ExecuteHeartbeatJob();
        break;
    case BackendJobType::NewsBriefing:
        ExecuteNewsBriefingJob(static_cast<NewsBriefingTaskContext*>(job.context));
        break;
    }
}

/**
 * @brief 从 NVS 恢复后端绑定状态。
 */
void BackendService::Start()
{
    if (started_)
    {
        return;
    }
    started_ = true;
    const BaseType_t worker_created = CreatePsramBackendTask(
        BackendWorkerTaskEntry,
        "backend_worker",
        kBackendWorkerStackSize,
        this,
        kBackendWorkerPriority,
        &backend_worker_task_handle_);
    if (worker_created != pdPASS)
    {
        backend_worker_task_handle_ = nullptr;
        ESP_LOGE(kTag, "后端常驻 Worker 创建失败，后台同步功能暂不可用");
    }
    /*
     * 依赖库的 Debug 日志会输出完整 HTTP 请求头，其中可能包含绑定会话 Token 或设备 Token。
     * 将底层客户端固定在 Info 级别，仍保留连接和错误日志，同时阻止认证头进入串口日志。
     */
    esp_log_level_set("HttpClient", ESP_LOG_INFO);
    LoadBindingState();

    const esp_timer_create_args_t weather_timer_args = {
        .callback = WeatherTimerCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "weather_sync",
        .skip_unhandled_events = true,
    };
    esp_err_t timer_error = esp_timer_create(&weather_timer_args, &weather_timer_);
    if (timer_error == ESP_OK)
    {
        timer_error = esp_timer_start_periodic(weather_timer_, kWeatherCheckIntervalUs);
    }
    if (timer_error != ESP_OK)
    {
        ESP_LOGE(kTag, "天气定时器启动失败，原因=%s",
                 esp_err_to_name(timer_error));
        if (weather_timer_ != nullptr)
        {
            esp_timer_delete(weather_timer_);
            weather_timer_ = nullptr;
        }
    }

    const esp_timer_create_args_t memo_timer_args = {
        .callback = MemoTimerCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "memo_sync",
        .skip_unhandled_events = true,
    };
    timer_error = esp_timer_create(&memo_timer_args, &memo_timer_);
    if (timer_error == ESP_OK)
    {
        timer_error = esp_timer_start_periodic(memo_timer_, kMemoCheckIntervalUs);
    }
    if (timer_error != ESP_OK)
    {
        ESP_LOGE(kTag, "备忘录定时器启动失败，原因=%s",
                 esp_err_to_name(timer_error));
        if (memo_timer_ != nullptr)
        {
            esp_timer_delete(memo_timer_);
            memo_timer_ = nullptr;
        }
    }

    const esp_timer_create_args_t memo_retry_timer_args = {
        .callback = MemoRetryTimerCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "memo_retry",
        .skip_unhandled_events = true,
    };
    timer_error = esp_timer_create(&memo_retry_timer_args, &memo_retry_timer_);
    if (timer_error != ESP_OK)
    {
        ESP_LOGE(kTag, "备忘录重试定时器创建失败，原因=%s",
                 esp_err_to_name(timer_error));
        memo_retry_timer_ = nullptr;
    }

    const esp_timer_create_args_t notification_timer_args = {
        .callback = NotificationTimerCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "notify_sync",
        .skip_unhandled_events = true,
    };
    timer_error = esp_timer_create(&notification_timer_args, &notification_timer_);
    if (timer_error == ESP_OK)
    {
        timer_error = esp_timer_start_periodic(
            notification_timer_, kNotificationCheckIntervalUs);
    }
    if (timer_error != ESP_OK)
    {
        ESP_LOGE(kTag, "主动通知定时器启动失败，原因=%s",
                 esp_err_to_name(timer_error));
        if (notification_timer_ != nullptr)
        {
            esp_timer_delete(notification_timer_);
            notification_timer_ = nullptr;
        }
    }

    const esp_timer_create_args_t notification_reconnect_timer_args = {
        .callback = NotificationReconnectTimerCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "notify_mqtt",
        .skip_unhandled_events = true,
    };
    timer_error = esp_timer_create(
        &notification_reconnect_timer_args,
        &notification_reconnect_timer_);
    if (timer_error != ESP_OK)
    {
        ESP_LOGE(kTag, "业务EMQX重连定时器创建失败，原因=%s",
                 esp_err_to_name(timer_error));
        notification_reconnect_timer_ = nullptr;
    }

    const esp_timer_create_args_t heartbeat_timer_args = {
        .callback = HeartbeatTimerCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "device_heartbeat",
        .skip_unhandled_events = true,
    };
    timer_error = esp_timer_create(&heartbeat_timer_args, &heartbeat_timer_);
    if (timer_error != ESP_OK)
    {
        ESP_LOGE(kTag, "设备心跳定时器创建失败，原因=%s",
                 esp_err_to_name(timer_error));
        heartbeat_timer_ = nullptr;
    }

    ESP_LOGI(kTag, "囤囤AI后端服务已初始化，接口地址=%s，设备凭据=%s",
             CONFIG_TUNTUN_API_URL, device_access_token_.empty() ? "缺失" : "可用");
}

/**
 * @brief 注册设备绑定状态处理、天气设置和备忘录 MCP 工具。
 * @param server 设备 MCP 服务器。
 */
void BackendService::RegisterMcpTools(McpServer &server)
{
    // 本地清单工具不访问后端，只介绍最近一次已经成功安装到设备的动态 MCP 快照。
    server.AddTool(
        "self.tuntun.list_custom_mcp_tools",
        "显示并介绍当前设备已经加载的全部自定义 MCP 工具。用户询问有几个自定义工具包、"
        "有几个自定义 MCP、有哪些自定义工具、查看自定义 MCP 列表或类似问题时必须调用。"
        "本工具没有参数；不要回答没有权限，也不要执行清单中的动态工具。",
        PropertyList(),
        [this](const PropertyList &properties) -> ReturnValue
        {
            (void)properties;
            std::vector<DynamicToolSummary> summaries;
            bool manifest_loaded = false;
            {
                std::lock_guard<std::mutex> lock(dynamic_mcp_mutex_);
                summaries = dynamic_tool_summaries_;
                manifest_loaded = mcp_manifest_loaded_;
            }

            std::string screen_title =
                "自定义 MCP（" + std::to_string(summaries.size()) + "）";
            std::vector<std::string> screen_items;
            std::string speech_text;
            if (!manifest_loaded)
            {
                screen_title = "自定义 MCP";
                screen_items.push_back("清单尚未同步");
                speech_text = "当前设备还没有同步到自定义 MCP 清单，请确认设备已经绑定并联网后稍后再试。";
            }
            else if (summaries.empty())
            {
                screen_items.push_back("暂无自定义 MCP");
                speech_text = "你当前没有配置可用的自定义 MCP。";
            }
            else
            {
                speech_text = "你当前配置了 " + std::to_string(summaries.size())
                    + " 个自定义 MCP。";
                for (size_t index = 0; index < summaries.size(); ++index)
                {
                    const std::string display_name = summaries[index].display_name.empty()
                        ? "未命名自定义 MCP"
                        : summaries[index].display_name;
                    std::string screen_item =
                        std::to_string(index + 1) + ". " + display_name
                        + "\n" + summaries[index].name;
                    screen_items.push_back(std::move(screen_item));

                    speech_text += "第 " + std::to_string(index + 1) + " 个是 "
                        + display_name;
                    if (!summaries[index].description.empty())
                    {
                        speech_text += "，" + summaries[index].description;
                    }
                    speech_text += "。";
                }
            }

            auto &board = Board::GetInstance();
            board.WakeUpScreen();
            auto *display = board.GetDisplay();
            if (display != nullptr)
            {
                display->ShowCustomMcpList(screen_title, screen_items);
            }
            ESP_LOGI(kTag, "正在显示并介绍自定义 MCP，数量=%u",
                     static_cast<unsigned>(summaries.size()));
            Application::GetInstance().RequestConversationEndAfterSpeaking();
            return speech_text;
        });

    server.AddTool(
        "self.tuntun.play_pending_notification",
        "播放当前设备正在等待用户确认的囤囤AI主动通知。仅当设备刚询问是否播报，且用户明确回答播放、好的或需要时调用。调用后设备会立即结束当前确认会话并直接播放通知，不要再生成确认回复。",
        PropertyList(),
        [this](const PropertyList &properties) -> ReturnValue
        {
            (void)properties;
            {
                std::lock_guard<std::mutex> lock(notification_mutex_);
                if (!pending_notification_.valid)
                {
                    return std::string("当前没有等待确认的通知。");
                }
            }
            auto &app = Application::GetInstance();
            app.EndCurrentConversationForLocalPlayback();
            app.Schedule([this]()
            {
                StartPendingNotificationPlayback();
            });
            return std::string("用户已确认，设备将直接播放通知。不要生成任何口头回复。");
        });

    server.AddTool(
        "self.tuntun.dismiss_pending_notification",
        "拒绝播放当前设备正在等待用户确认的囤囤AI主动通知。仅当用户明确回答不播放、不需要或取消时调用。",
        PropertyList(),
        [this](const PropertyList &properties) -> ReturnValue
        {
            (void)properties;
            PendingNotification notification;
            {
                std::lock_guard<std::mutex> lock(notification_mutex_);
                if (!pending_notification_.valid)
                {
                    return std::string("当前没有等待确认的通知。");
                }
                notification = pending_notification_;
                pending_notification_ = PendingNotification{};
            }
            AckNotification(notification, kNotificationAckDismissed);
            return std::string("已取消本次通知播报。");
        });

    // 绑定工具在设备 MCP 服务器上注册为异步工具，确保在网络请求完成前不会阻塞语音会话。
    server.AddAsyncTool(
        "self.tuntun.bind_device",
        "Check whether this device is already bound. If it is bound, report that state directly. "
        "Otherwise generate a TuntunLife platform device binding code and show it on the screen. "
        "Call this tool only when the user explicitly asks to bind or connect this device to the "
        "TuntunLife platform. The user must finish binding in the TuntunLife web application. "
        "Do not read the web address aloud.",
        PropertyList(),
        [this](const PropertyList &properties, McpToolCompletion completion)
        {
            (void)properties;
            StartBindingTask(true,
                             [completion = std::move(completion)](const std::string &message,
                                                                  bool is_error) mutable
                             {
                                 if (!is_error)
                                 {
                                     Application::GetInstance()
                                         .RequestConversationEndAfterSpeaking();
                                 }
                                 completion(McpToolResult{message, is_error});
                             });
        });

    server.AddTool(
        "self.tuntun.exit_binding_page",
        "关闭当前设备绑定页面。用户在绑定页面显示期间说退出绑定、关闭绑定页面、取消显示"
        "或返回时必须调用。只关闭页面，不取消后台绑定会话。",
        PropertyList(),
        [this](const PropertyList &properties) -> ReturnValue
        {
            (void)properties;
            const bool closed = ExitBindingPage();
            Application::GetInstance().RequestConversationEndAfterSpeaking();
            return closed
                ? std::string("已退出设备绑定页面。")
                : std::string("当前没有正在显示的设备绑定页面。");
        });

    // 解绑工具在设备 MCP 服务器上注册为同步工具，确保在语音会话中立即返回说明文本。
    server.AddTool(
        "self.tuntun.unbind_device",
        "Explain how to unbind this device from the TuntunLife platform. Call this tool when the "
        "user explicitly asks to unbind or remove this device. Device-side voice unbinding is not "
        "allowed; the user must sign in to the TuntunLife web application and unbind it there. "
        "If the user only wants to close the binding page, call self.tuntun.exit_binding_page instead.",
        PropertyList(),
        [](const PropertyList &properties) -> ReturnValue
        {
            (void)properties;
            return std::string(
                "为保证设备归属安全，设备端不支持语音解绑。请登录囤囤AI后台，在设备管理中完成解绑。");
        });

    // 天气位置设置工具在设备 MCP 服务器上注册为异步工具，确保在网络请求完成前不会阻塞语音会话。
    server.AddAsyncTool(
        "self.tuntun.set_weather_location",
        "Set the fixed weather city or district for this device. Call this tool when the user "
        "asks to change the weather location. Pass the complete Chinese city or district name, "
        "for example 上海市松江区. The platform resolves the internal location code automatically.",
        PropertyList({Property("location_name", kPropertyTypeString)}),
        [this](const PropertyList &properties, McpToolCompletion completion)
        {
            StartWeatherLocationTask(
                properties["location_name"].value<std::string>(),
                false,
                [completion = std::move(completion)](const std::string &message,
                                                     bool is_error) mutable
                {
                    completion(McpToolResult{message, is_error});
                });
        });

    // 天气位置自动设置工具在设备 MCP 服务器上注册为异步工具，确保在网络请求完成前不会阻塞语音会话。
    server.AddAsyncTool(
        "self.tuntun.set_weather_ip_auto",
        "Switch this device to automatic weather location based on the device public IP address. "
        "Call this tool when the user asks to use automatic location, current network location, "
        "or IP-based weather location. This tool has no arguments.",
        PropertyList(),
        [this](const PropertyList &properties, McpToolCompletion completion)
        {
            (void)properties;
            StartWeatherLocationTask(
                "",
                true,
                [completion = std::move(completion)](const std::string &message,
                                                     bool is_error) mutable
                {
                    completion(McpToolResult{message, is_error});
                });
        });

    server.AddAsyncTool(
        "self.tuntun.set_weather_announcement",
        "设置当前设备的每日天气播报。用户必须给出具体播报时间；announcement_time 使用 HH:mm:ss。"
        "到达设置时间后设备直接播报天气。",
        PropertyList({
            Property("announcement_time", kPropertyTypeString)
        }),
        [this](const PropertyList &properties, McpToolCompletion completion)
        {
            StartWeatherAnnouncementTask(
                true,
                properties["announcement_time"].value<std::string>(),
                [completion = std::move(completion)](const std::string &message,
                                                     bool is_error) mutable
                {
                    completion(McpToolResult{message, is_error});
                });
        });

    server.AddAsyncTool(
        "self.tuntun.disable_weather_announcement",
        "关闭当前设备的每日天气播报。用户明确要求停止、关闭或取消天气播报时调用。",
        PropertyList(),
        [this](const PropertyList &properties, McpToolCompletion completion)
        {
            (void)properties;
            StartWeatherAnnouncementTask(
                false,
                "",
                [completion = std::move(completion)](const std::string &message,
                                                     bool is_error) mutable
                {
                    completion(McpToolResult{message, is_error});
                });
        });

    // 创建工具要求模型先通过对话补齐精确提醒日期和时间，不能把事项日期直接当作提醒时间。
    server.AddAsyncTool(
        "self.tuntun.create_memo",
        "创建囤囤AI备忘录，不能回答没有此功能。调用前必须取得具体提醒日期和时间：用户只说事项日期时"
        "询问何时提醒，只说提醒日期时继续询问几点；明确说不提醒才传空 remind_at。content 传正文，"
        "remind_at 传带 +08:00 偏移的 RFC 3339 绝对时间。",
        PropertyList({
            Property("content", kPropertyTypeString),
            Property("remind_at", kPropertyTypeString)
        }),
        [this](const PropertyList &properties, McpToolCompletion completion)
        {
            auto *context = new (std::nothrow) MemoToolTaskContext();
            if (context == nullptr)
            {
                completion(McpToolResult{"设备内存不足，无法创建备忘录。", true});
                return;
            }
            context->service = this;
            context->operation = MemoToolOperation::Create;
            context->content = properties["content"].value<std::string>();
            context->remind_at = properties["remind_at"].value<std::string>();
            context->completion =
                [completion = std::move(completion)](const std::string &message,
                                                     bool is_error) mutable
                {
                    completion(McpToolResult{message, is_error});
                };
            StartMemoToolTask(context);
        });

    // 查询工具使用固定数值范围，避免大模型传入无法稳定解析的自然语言筛选值。
    server.AddAsyncTool(
        "self.tuntun.query_memos",
        "查询囤囤AI备忘录详情、完成状态和备忘录 ID。用户询问有哪些备忘，或修改、删除目标的 ID 未知时调用。"
        "status：0=全部，1=未完成，2=已完成；time_range：0=不限，1=今天，2=明天，3=本周，4=未到期。",
        PropertyList({
            Property("status", kPropertyTypeInteger, 1, 0, 2),
            Property("time_range", kPropertyTypeInteger, 0, 0, 4)
        }),
        [this](const PropertyList &properties, McpToolCompletion completion)
        {
            auto *context = new (std::nothrow) MemoToolTaskContext();
            if (context == nullptr)
            {
                completion(McpToolResult{"设备内存不足，无法查询备忘录。", true});
                return;
            }
            context->service = this;
            context->operation = MemoToolOperation::Query;
            context->status = properties["status"].value<int>();
            context->time_range = properties["time_range"].value<int>();
            context->completion =
                [completion = std::move(completion)](const std::string &message,
                                                     bool is_error) mutable
                {
                    completion(McpToolResult{message, is_error});
                };
            StartMemoToolTask(context);
        });

    // 修改工具要求完整目标值；变更提醒时间时同样必须先通过对话补齐日期和具体时间。
    server.AddAsyncTool(
        "self.tuntun.update_memo",
        "修改囤囤AI备忘录。先 query_memos 取得 ID 和原值；修改提醒但缺少日期或具体时间时必须继续询问，"
        "不修改提醒则保留原值，明确取消提醒才传空 remind_at。传修改后的完整 content、remind_at、status，"
        "status：1=未完成，2=已完成。",
        PropertyList({
            Property("memo_id", kPropertyTypeString),
            Property("content", kPropertyTypeString),
            Property("remind_at", kPropertyTypeString),
            Property("status", kPropertyTypeInteger, 1, 2)
        }),
        [this](const PropertyList &properties, McpToolCompletion completion)
        {
            auto *context = new (std::nothrow) MemoToolTaskContext();
            if (context == nullptr)
            {
                completion(McpToolResult{"设备内存不足，无法修改备忘录。", true});
                return;
            }
            context->service = this;
            context->operation = MemoToolOperation::Update;
            context->memo_id = properties["memo_id"].value<std::string>();
            context->content = properties["content"].value<std::string>();
            context->remind_at = properties["remind_at"].value<std::string>();
            context->status = properties["status"].value<int>();
            context->completion =
                [completion = std::move(completion)](const std::string &message,
                                                     bool is_error) mutable
                {
                    completion(McpToolResult{message, is_error});
                };
            StartMemoToolTask(context);
        });

    // 删除工具只接受查询接口返回的 ID，不支持用正文模糊删除。
    server.AddAsyncTool(
        "self.tuntun.delete_memo",
        "删除一条囤囤AI备忘录。仅在用户明确要求删除时调用；若 memo_id 未知，必须先调用 "
        "self.tuntun.query_memos 获取准确 ID，禁止猜测或使用备忘录正文代替 ID。",
        PropertyList({Property("memo_id", kPropertyTypeString)}),
        [this](const PropertyList &properties, McpToolCompletion completion)
        {
            auto *context = new (std::nothrow) MemoToolTaskContext();
            if (context == nullptr)
            {
                completion(McpToolResult{"设备内存不足，无法删除备忘录。", true});
                return;
            }
            context->service = this;
            context->operation = MemoToolOperation::Delete;
            context->memo_id = properties["memo_id"].value<std::string>();
            context->completion =
                [completion = std::move(completion)](const std::string &message,
                                                     bool is_error) mutable
                {
                    completion(McpToolResult{message, is_error});
                };
            StartMemoToolTask(context);
        });

    // 统计工具直接返回后端同一时刻计算的总数和常用时间范围数量，避免模型自行统计分页结果。
    server.AddAsyncTool(
        "self.tuntun.get_memo_statistics",
        "统计囤囤AI备忘录数量，返回全部、未到期、今天、明天和本周数量。用户询问有多少条备忘录，"
        "或今天、明天、本周是否有备忘录时调用，不要根据分页查询结果自行计算。",
        PropertyList(),
        [this](const PropertyList &properties, McpToolCompletion completion)
        {
            (void)properties;
            auto *context = new (std::nothrow) MemoToolTaskContext();
            if (context == nullptr)
            {
                completion(McpToolResult{"设备内存不足，无法统计备忘录。", true});
                return;
            }
            context->service = this;
            context->operation = MemoToolOperation::Statistics;
            context->completion =
                [completion = std::move(completion)](const std::string &message,
                                                     bool is_error) mutable
                {
                    completion(McpToolResult{message, is_error});
                };
            StartMemoToolTask(context);
        });

    // 创建提醒前必须补齐精确首次执行时间；循环提醒还必须取得明确的分钟间隔。
    server.AddAsyncTool(
        "self.tuntun.create_custom_reminder",
        "创建囤囤AI自定义提醒，不能回答没有此功能。调用前必须取得具体首次执行日期和时间；"
        "schedule_type：1=指定时间执行一次，2=按固定分钟间隔循环。一次性提醒 interval_minutes 传0、"
        "allowed_time_ranges 传空字符串；循环提醒必须传大于0的 interval_minutes，可用"
        "allowed_time_ranges 限制每日提醒时段，格式为08:00-12:00,14:00-22:00，空字符串表示全天。",
        PropertyList({
            Property("content", kPropertyTypeString),
            Property("schedule_type", kPropertyTypeInteger, 1, 2),
            Property("first_run_at", kPropertyTypeString),
            Property("interval_minutes", kPropertyTypeInteger, 0, kMaximumCustomReminderIntervalMinutes),
            Property("allowed_time_ranges", kPropertyTypeString)
        }),
        [this](const PropertyList &properties, McpToolCompletion completion)
        {
            auto *context = new (std::nothrow) CustomReminderToolTaskContext();
            if (context == nullptr)
            {
                completion(McpToolResult{"设备内存不足，无法创建自定义提醒。", true});
                return;
            }
            context->service = this;
            context->operation = CustomReminderToolOperation::Create;
            context->content = properties["content"].value<std::string>();
            context->schedule_type = properties["schedule_type"].value<int>();
            context->first_run_at = properties["first_run_at"].value<std::string>();
            context->interval_minutes = properties["interval_minutes"].value<int>();
            context->allowed_time_ranges = properties["allowed_time_ranges"].value<std::string>();
            context->completion =
                [completion = std::move(completion)](const std::string &message,
                                                     bool is_error) mutable
                {
                    completion(McpToolResult{message, is_error});
                };
            StartCustomReminderToolTask(context);
        });

    // 查询结果包含准确 ID 和完整计划配置，供后续编辑、删除和启停操作使用。
    server.AddAsyncTool(
        "self.tuntun.query_custom_reminders",
        "查询囤囤AI自定义提醒及其准确 ID。用户询问有哪些提醒，或编辑、删除、启停目标 ID 未知时调用。"
        "status_filter：0=全部，1=启用，2=停用，3=已完成；schedule_type：0=全部，1=一次性，2=间隔循环。",
        PropertyList({
            Property("status_filter", kPropertyTypeInteger, 0, 0, 3),
            Property("schedule_type", kPropertyTypeInteger, 0, 0, 2)
        }),
        [this](const PropertyList &properties, McpToolCompletion completion)
        {
            auto *context = new (std::nothrow) CustomReminderToolTaskContext();
            if (context == nullptr)
            {
                completion(McpToolResult{"设备内存不足，无法查询自定义提醒。", true});
                return;
            }
            context->service = this;
            context->operation = CustomReminderToolOperation::Query;
            context->status_filter = properties["status_filter"].value<int>();
            context->schedule_type = properties["schedule_type"].value<int>();
            context->completion =
                [completion = std::move(completion)](const std::string &message,
                                                     bool is_error) mutable
                {
                    completion(McpToolResult{message, is_error});
                };
            StartCustomReminderToolTask(context);
        });

    // 编辑请求传完整计划配置，固件先读取原记录并保留原启停状态。
    server.AddAsyncTool(
        "self.tuntun.update_custom_reminder",
        "编辑囤囤AI自定义提醒。必须先 query_custom_reminders 获取准确 ID 和原配置，再传编辑后的完整内容。"
        "不修改的字段原样传回；修改时间时必须取得具体日期和时间。schedule_type：1=一次性，2=间隔循环；"
        "一次性提醒 interval_minutes 传0且 allowed_time_ranges 传空字符串；循环时段格式为"
        "08:00-12:00,14:00-22:00，空字符串表示全天。编辑不会改变原来的启用或停用状态。",
        PropertyList({
            Property("reminder_id", kPropertyTypeString),
            Property("content", kPropertyTypeString),
            Property("schedule_type", kPropertyTypeInteger, 1, 2),
            Property("first_run_at", kPropertyTypeString),
            Property("interval_minutes", kPropertyTypeInteger, 0, kMaximumCustomReminderIntervalMinutes),
            Property("allowed_time_ranges", kPropertyTypeString)
        }),
        [this](const PropertyList &properties, McpToolCompletion completion)
        {
            auto *context = new (std::nothrow) CustomReminderToolTaskContext();
            if (context == nullptr)
            {
                completion(McpToolResult{"设备内存不足，无法编辑自定义提醒。", true});
                return;
            }
            context->service = this;
            context->operation = CustomReminderToolOperation::Update;
            context->reminder_id = properties["reminder_id"].value<std::string>();
            context->content = properties["content"].value<std::string>();
            context->schedule_type = properties["schedule_type"].value<int>();
            context->first_run_at = properties["first_run_at"].value<std::string>();
            context->interval_minutes = properties["interval_minutes"].value<int>();
            context->allowed_time_ranges = properties["allowed_time_ranges"].value<std::string>();
            context->completion =
                [completion = std::move(completion)](const std::string &message,
                                                     bool is_error) mutable
                {
                    completion(McpToolResult{message, is_error});
                };
            StartCustomReminderToolTask(context);
        });

    server.AddAsyncTool(
        "self.tuntun.delete_custom_reminder",
        "删除一条囤囤AI自定义提醒。仅在用户明确要求删除时调用；若 reminder_id 未知，必须先调用"
        " query_custom_reminders 获取准确 ID，禁止猜测或用提醒正文代替 ID。",
        PropertyList({Property("reminder_id", kPropertyTypeString)}),
        [this](const PropertyList &properties, McpToolCompletion completion)
        {
            auto *context = new (std::nothrow) CustomReminderToolTaskContext();
            if (context == nullptr)
            {
                completion(McpToolResult{"设备内存不足，无法删除自定义提醒。", true});
                return;
            }
            context->service = this;
            context->operation = CustomReminderToolOperation::Delete;
            context->reminder_id = properties["reminder_id"].value<std::string>();
            context->completion =
                [completion = std::move(completion)](const std::string &message,
                                                     bool is_error) mutable
                {
                    completion(McpToolResult{message, is_error});
                };
            StartCustomReminderToolTask(context);
        });

    server.AddAsyncTool(
        "self.tuntun.enable_custom_reminder",
        "启用或恢复一条已停用的囤囤AI自定义提醒。若 reminder_id 未知，必须先调用"
        " query_custom_reminders 获取准确 ID。已完成的一次性提醒不能重新启用。",
        PropertyList({Property("reminder_id", kPropertyTypeString)}),
        [this](const PropertyList &properties, McpToolCompletion completion)
        {
            auto *context = new (std::nothrow) CustomReminderToolTaskContext();
            if (context == nullptr)
            {
                completion(McpToolResult{"设备内存不足，无法启用自定义提醒。", true});
                return;
            }
            context->service = this;
            context->operation = CustomReminderToolOperation::Enable;
            context->reminder_id = properties["reminder_id"].value<std::string>();
            context->completion =
                [completion = std::move(completion)](const std::string &message,
                                                     bool is_error) mutable
                {
                    completion(McpToolResult{message, is_error});
                };
            StartCustomReminderToolTask(context);
        });

    server.AddAsyncTool(
        "self.tuntun.disable_custom_reminder",
        "停用或停止一条囤囤AI自定义提醒。若 reminder_id 未知，必须先调用"
        " query_custom_reminders 获取准确 ID。停用后不会继续产生新的提醒通知。",
        PropertyList({Property("reminder_id", kPropertyTypeString)}),
        [this](const PropertyList &properties, McpToolCompletion completion)
        {
            auto *context = new (std::nothrow) CustomReminderToolTaskContext();
            if (context == nullptr)
            {
                completion(McpToolResult{"设备内存不足，无法停用自定义提醒。", true});
                return;
            }
            context->service = this;
            context->operation = CustomReminderToolOperation::Disable;
            context->reminder_id = properties["reminder_id"].value<std::string>();
            context->completion =
                [completion = std::move(completion)](const std::string &message,
                                                     bool is_error) mutable
                {
                    completion(McpToolResult{message, is_error});
                };
            StartCustomReminderToolTask(context);
        });

    server.AddAsyncTool(
        "self.tuntun.get_daily_news_briefing",
        "获取当前用户已经订阅的今日新闻简报。当用户询问今天有什么新闻、今日简报或订阅领域最新消息时调用。"
        "分类、去重、排序和摘要由囤囤AI平台完成，不要自行补充新闻内容。",
        PropertyList(),
        [this](const PropertyList &properties, McpToolCompletion completion)
        {
            (void)properties;
            StartNewsBriefingTask(
                [completion = std::move(completion)](const std::string &message,
                                                     bool is_error) mutable
                {
                    completion(McpToolResult{message, is_error});
                });
        });
}

/**
 * @brief 标记网络可用，并在后台静默恢复未完成绑定会话的轮询。
 * @details 自动恢复只检查旧会话状态，不显示绑定页面；绑定页面只能由用户主动调用绑定工具打开。
 */
void BackendService::OnNetworkConnected()
{
    network_connected_.store(true);
    bool has_pending_session = false;
    bool has_device_credential = false;
    {
        std::lock_guard<std::mutex> lock(binding_mutex_);
        has_pending_session = !binding_session_token_.empty() && !binding_code_.empty();
        has_device_credential = !device_access_token_.empty();
    }
    if (has_pending_session)
    {
        StartBindingTask(false);
    }
    if (screensaver_active_.load())
    {
        if (has_device_credential)
        {
            weather_refresh_requested_.store(true);
            memo_refresh_requested_.store(true);
            ShowWeatherStatusIfUnavailable("天气加载中");
            ShowMemoStatusIfUnavailable("备忘录加载中");
            StartWeatherSync(true);
            StartMemoSync(true);
        }
        else
        {
            ShowWeatherStatusIfUnavailable("");
            ShowMemoStatusIfUnavailable(kUnboundMemoPrompt);
        }
    }
    StartMcpManifestSync();
    StartNotificationSync(true);
}

/**
 * @brief 标记网络不可用，活动绑定任务会暂停请求并等待恢复。
 */
void BackendService::OnNetworkDisconnected()
{
    network_connected_.store(false);
    StopHeartbeatPublishing();
    bool has_device_credential = false;
    {
        std::lock_guard<std::mutex> lock(binding_mutex_);
        has_device_credential = !device_access_token_.empty();
    }
    if (notification_reconnect_timer_ != nullptr)
    {
        esp_timer_stop(notification_reconnect_timer_);
    }
    notification_reconnect_delay_seconds_.store(kNotificationReconnectInitialSeconds);
    std::unique_ptr<Mqtt> mqtt;
    {
        std::lock_guard<std::mutex> lock(notification_mutex_);
        active_notification_mqtt_.store(nullptr);
        heartbeat_topic_.clear();
        mqtt = std::move(notification_mqtt_);
    }
    if (mqtt != nullptr)
    {
        mqtt->Disconnect();
    }
    ShowWeatherStatusIfUnavailable(has_device_credential ? "等待网络连接" : "");
    ShowMemoStatusIfUnavailable(
        has_device_credential ? "等待网络连接" : kUnboundMemoPrompt);
}

/**
 * @brief 语音 MCP 会话结束不取消设备级绑定流程。
 */
void BackendService::OnMcpDisconnected()
{
}

/**
 * @brief 判断设备绑定覆盖页面当前是否可见。
 * @return 绑定页面正在显示时返回 true，否则返回 false。
 */
bool BackendService::IsBindingPageVisible() const
{
    return binding_page_visible_.load();
}

/**
 * @brief 由用户主动退出绑定覆盖页面，同时保留绑定会话和后台轮询。
 * @return 页面原本可见且已经关闭时返回 true，否则返回 false。
 */
bool BackendService::ExitBindingPage()
{
    std::lock_guard<std::mutex> lock(binding_mutex_);
    if (!binding_page_visible_.load())
    {
        return false;
    }
    binding_page_dismissed_ = true;
    binding_page_visible_.store(false);
    Display *display = Board::GetInstance().GetDisplay();
    if (display != nullptr)
    {
        display->HideDeviceBinding();
    }
    ESP_LOGI(kTag, "用户已退出设备绑定页面，后台绑定会话继续运行");
    return true;
}

/**
 * @brief 显示绑定流程状态，并确保用户可见背光已经恢复。
 * @param binding_code 非空时显示大号数字绑定码；空字符串表示只显示状态。
 * @param message 显示在绑定码下方的中文操作说明或结果。
 */
void BackendService::ShowBindingPage(
    const std::string &binding_code,
    const std::string &message)
{
    std::lock_guard<std::mutex> lock(binding_mutex_);
    if (binding_page_dismissed_)
    {
        return;
    }
    auto &board = Board::GetInstance();
    board.WakeUpScreen(true);
    Display *display = board.GetDisplay();
    if (display != nullptr)
    {
        display->ShowDeviceBinding(binding_code, message);
        binding_page_visible_.store(true);
    }
}

/**
 * @brief 隐藏绑定页面并恢复页面下方的正常界面。
 */
void BackendService::HideBindingPage()
{
    std::lock_guard<std::mutex> lock(binding_mutex_);
    binding_page_visible_.store(false);
    Display *display = Board::GetInstance().GetDisplay();
    if (display != nullptr)
    {
        display->HideDeviceBinding();
    }
}

/**
 * @brief 用户开始主动交互时中断正在下载或播放的后台通知。
 * @return true 表示存在通知播放任务且已经发出中断请求。
 */
bool BackendService::InterruptNotificationPlayback()
{
    std::lock_guard<std::mutex> lock(notification_mutex_);
    if (notification_playback_task_handle_ == nullptr)
    {
        return false;
    }
    notification_playback_interrupted_.store(true);
    return true;
}
