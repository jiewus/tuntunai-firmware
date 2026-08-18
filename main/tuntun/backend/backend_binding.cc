/**
 * @file backend_binding.cc
 * @brief 囤囤AI设备绑定会话、页面和凭据持久化实现。
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
 * @brief 启动独立绑定任务，或复用已经显示的有效绑定码。
 * @param request_new_session true 申请新会话；false 恢复 NVS 会话。
 * @param completion MCP 工具结果回调，自动恢复时为空。
 */
void BackendService::StartBindingTask(bool request_new_session,
                                      BindingCompletion completion)
{
    if (request_new_session)
    {
        bool device_is_bound = false;
        {
            std::lock_guard<std::mutex> lock(binding_mutex_);
            device_is_bound = !device_id_.empty() && !device_access_token_.empty();
            if (!device_is_bound)
            {
                binding_page_dismissed_ = false;
            }
        }
        if (device_is_bound)
        {
            if (completion)
            {
                completion(
                    "当前设备已经绑定囤囤AI。如需解绑，请登录囤囤AI后台操作。",
                    false);
            }
            return;
        }
    }

    if (!network_connected_.load())
    {
        if (completion)
        {
            completion("设备网络尚未连接，暂时无法生成绑定码。", true);
        }
        return;
    }

    std::string active_code;
    bool task_already_running = false;
    bool task_start_failed = false;
    {
        std::lock_guard<std::mutex> lock(binding_mutex_);
        if (binding_task_handle_ != nullptr)
        {
            task_already_running = true;
            active_code = binding_code_;
        }
        else
        {
            auto *context = new (std::nothrow) BindingTaskContext{
                this, std::move(completion), request_new_session};
            if (context == nullptr)
            {
                task_start_failed = true;
            }
            else
            {
                binding_task_handle_ = nullptr;
                BaseType_t created = xTaskCreate(
                    BindingTaskEntry, "tuntun_bind", 8192, context, 1,
                    &binding_task_handle_);
                if (created == pdPASS)
                {
                    return;
                }
                completion = std::move(context->completion);
                delete context;
                binding_task_handle_ = nullptr;
                task_start_failed = true;
            }
        }
    }

    if (task_start_failed && !completion)
    {
        ESP_LOGE(kTag, "内存不足，无法启动设备绑定任务");
        return;
    }
    if (task_start_failed)
    {
        completion("设备内存不足，无法启动绑定流程。", true);
        return;
    }

    if (!task_already_running)
    {
        return;
    }

    if (!active_code.empty())
    {
        ShowBindingPage(active_code, kBindingPageMessage);
    }
    if (completion)
    {
        completion(active_code.empty()
                       ? "设备正在生成绑定码，请稍候。"
                       : "绑定码已经显示在设备屏幕上，请在囤囤AI网页端完成绑定。",
                   false);
    }
}

/**
 * @brief FreeRTOS 绑定任务入口，确保上下文和任务句柄始终释放。
 * @param context BindingTaskContext 指针。
 */
void BackendService::BindingTaskEntry(void *context)
{
    std::unique_ptr<BindingTaskContext> task_context(
        static_cast<BindingTaskContext *>(context));
    BackendService *service = task_context->service;
    try
    {
        service->RunBindingTask(task_context->request_new_session,
                                task_context->completion);
    }
    catch (const std::exception &exception)
    {
        ESP_LOGE(kTag, "设备绑定任务异常，原因=%s", exception.what());
        const bool binding_page_visible = service->IsBindingPageVisible();
        if (binding_page_visible)
        {
            service->ShowBindingPage("", "绑定流程异常，请稍后重试");
        }
        FinishToolRequest(task_context->completion,
                          "设备绑定流程异常，请稍后重试。", true);
        if (binding_page_visible)
        {
            vTaskDelay(pdMS_TO_TICKS(3000));
            service->HideBindingPage();
            Application::GetInstance().EndCurrentConversationAndEnterScreensaver();
        }
    }
    catch (...)
    {
        ESP_LOGE(kTag, "设备绑定任务发生未知异常");
        const bool binding_page_visible = service->IsBindingPageVisible();
        if (binding_page_visible)
        {
            service->ShowBindingPage("", "绑定流程异常，请稍后重试");
        }
        FinishToolRequest(task_context->completion,
                          "设备绑定流程异常，请稍后重试。", true);
        if (binding_page_visible)
        {
            vTaskDelay(pdMS_TO_TICKS(3000));
            service->HideBindingPage();
            Application::GetInstance().EndCurrentConversationAndEnterScreensaver();
        }
    }
    if (task_context->completion)
    {
        FinishToolRequest(task_context->completion,
                          "设备绑定流程未能完成，请稍后重试。", true);
    }
    bool start_bound_device_sync = false;
    {
        std::lock_guard<std::mutex> lock(service->binding_mutex_);
        service->binding_task_handle_ = nullptr;
        start_bound_device_sync = !service->device_access_token_.empty();
    }
    if (start_bound_device_sync)
    {
        Application::GetInstance().Schedule([service]()
        {
            service->StartMcpManifestSync();
            if (service->screensaver_active_.load())
            {
                service->StartWeatherSync(true);
                service->StartPendingReminderSync(true);
            }
        });
    }
    // 绑定成功后会写入 NVS；Flash 缓存关闭期间任务栈必须保留在片内 SRAM。
    vTaskDelete(nullptr);
}

/**
 * @brief 申请或恢复绑定会话，持续轮询并完成设备绑定流程。
 * @param request_new_session true 创建新会话并显示绑定页面；false 在后台静默恢复已有会话。
 * @param completion 生成绑定码后返回 MCP 结果的一次性回调。
 */
void BackendService::RunBindingTask(bool request_new_session,
                                    BindingCompletion &completion)
{
    std::string binding_code;
    std::string session_token;
    {
        std::lock_guard<std::mutex> lock(binding_mutex_);
        binding_code = binding_code_;
        session_token = binding_session_token_;
    }

    if (request_new_session)
    {
        ShowBindingPage("", "正在获取绑定码...");
        const std::string request_json = BuildBindingRequestJson();
        if (request_json.empty())
        {
            ShowBindingPage("", "设备信息生成失败，请稍后重试");
            FinishToolRequest(completion,
                              "设备信息生成失败，无法申请绑定码。", true);
            vTaskDelay(pdMS_TO_TICKS(3000));
            HideBindingPage();
            return;
        }

        HttpResponse response = SendJsonRequest(
            "POST", kBindingRequestPath, "", request_json);
        cJSON *root = nullptr;
        cJSON *data = nullptr;
        std::string message = "平台暂时无法生成绑定码";
        bool parsed = response.transport_succeeded && (response.status_code == 200 || response.status_code == 201) && ParseSuccessData(response.body, &root, &data, message);
        if (parsed)
        {
            cJSON *code = cJSON_GetObjectItemCaseSensitive(data, "binding_code");
            cJSON *token = cJSON_GetObjectItemCaseSensitive(data, "binding_session_token");
            parsed = cJSON_IsString(code) && cJSON_IsString(token);
            if (parsed)
            {
                binding_code = code->valuestring;
                session_token = token->valuestring;
            }
        }
        cJSON_Delete(root);
        if (!parsed)
        {
            ESP_LOGW(kTag, "绑定码申请失败，HTTP 状态码=%d", response.status_code);
            ShowBindingPage("", message.empty() ? "获取绑定码失败，请稍后重试" : message);
            FinishToolRequest(
                completion,
                message.empty() ? "获取绑定码失败，请稍后重试。" : message,
                true);
            vTaskDelay(pdMS_TO_TICKS(3000));
            HideBindingPage();
            return;
        }

        SavePendingBinding(binding_code, session_token);
        ShowBindingPage(binding_code, kBindingPageMessage);
        FinishToolRequest(
            completion,
            "绑定码已经显示在设备屏幕上，请在囤囤AI网页端完成绑定。",
            false);
    }
    else
    {
        if (binding_code.empty() || session_token.empty())
        {
            return;
        }
    }

    while (true)
    {
        if (!network_connected_.load())
        {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        HttpResponse status_response = SendJsonRequest(
            "GET", kBindingStatusPath, session_token, "");
        cJSON *status_root = nullptr;
        cJSON *status_data = nullptr;
        std::string status_message;
        bool status_parsed = status_response.transport_succeeded && status_response.status_code == 200 && ParseSuccessData(status_response.body, &status_root, &status_data, status_message);
        int binding_status = 0;
        if (status_parsed)
        {
            cJSON *status = cJSON_GetObjectItemCaseSensitive(status_data, "status");
            status_parsed = cJSON_IsNumber(status);
            if (status_parsed)
            {
                binding_status = status->valueint;
            }
        }
        cJSON_Delete(status_root);

        if (!status_parsed)
        {
            if (status_response.status_code == 401 || status_response.status_code == 403)
            {
                const bool binding_page_visible = IsBindingPageVisible();
                ClearPendingBinding();
                if (binding_page_visible)
                {
                    ShowBindingPage("", "绑定码已过期，请重新申请");
                    vTaskDelay(pdMS_TO_TICKS(3000));
                    HideBindingPage();
                    Application::GetInstance().EndCurrentConversationAndEnterScreensaver();
                }
                return;
            }
            ESP_LOGW(kTag, "绑定状态查询失败，HTTP 状态码=%d",
                     status_response.status_code);
            vTaskDelay(pdMS_TO_TICKS(kBindingRetryIntervalMs));
            continue;
        }

        if (binding_status == kBindingStatusPending)
        {
            vTaskDelay(pdMS_TO_TICKS(kBindingPollIntervalMs));
            continue;
        }
        if (binding_status == kBindingStatusBound)
        {
            HttpResponse complete_response = SendJsonRequest(
                "POST", kBindingCompletePath, session_token, "{}");
            cJSON *complete_root = nullptr;
            cJSON *complete_data = nullptr;
            std::string complete_message;
            bool complete_parsed = complete_response.transport_succeeded && complete_response.status_code == 200 && ParseSuccessData(complete_response.body, &complete_root, &complete_data, complete_message);
            std::string device_id;
            std::string access_token;
            if (complete_parsed)
            {
                cJSON *device = cJSON_GetObjectItemCaseSensitive(complete_data, "device_id");
                cJSON *token = cJSON_GetObjectItemCaseSensitive(complete_data, "access_token");
                complete_parsed = cJSON_IsString(device) && cJSON_IsString(token);
                if (complete_parsed)
                {
                    device_id = device->valuestring;
                    access_token = token->valuestring;
                }
            }
            cJSON_Delete(complete_root);
            if (!complete_parsed)
            {
                ESP_LOGW(kTag, "设备绑定完成请求失败，HTTP 状态码=%d",
                         complete_response.status_code);
                vTaskDelay(pdMS_TO_TICKS(kBindingRetryIntervalMs));
                continue;
            }

            SaveDeviceCredential(device_id, access_token);
            const bool binding_page_visible = IsBindingPageVisible();
            ClearPendingBinding();
            if (binding_page_visible)
            {
                ShowBindingPage("", "绑定成功");
                vTaskDelay(pdMS_TO_TICKS(3000));
                HideBindingPage();
                Application::GetInstance().EndCurrentConversationAndEnterScreensaver();
            }
            ESP_LOGI(kTag, "设备绑定完成");
            return;
        }
        if (binding_status == kBindingStatusCompleted)
        {
            const bool binding_page_visible = IsBindingPageVisible();
            ClearPendingBinding();
            if (binding_page_visible)
            {
                ShowBindingPage("", "设备已完成绑定");
                vTaskDelay(pdMS_TO_TICKS(3000));
                HideBindingPage();
                Application::GetInstance().EndCurrentConversationAndEnterScreensaver();
            }
            return;
        }
        if (binding_status == kBindingStatusExpired || binding_status == kBindingStatusFailed)
        {
            const bool binding_page_visible = IsBindingPageVisible();
            ClearPendingBinding();
            if (binding_page_visible)
            {
                ShowBindingPage("", binding_status == kBindingStatusExpired
                                        ? "绑定码已过期，请重新申请"
                                        : "绑定失败，请重新申请");
                vTaskDelay(pdMS_TO_TICKS(3000));
                HideBindingPage();
                Application::GetInstance().EndCurrentConversationAndEnterScreensaver();
            }
            return;
        }

        ESP_LOGW(kTag, "设备绑定状态未知，状态值=%d", binding_status);
        vTaskDelay(pdMS_TO_TICKS(kBindingRetryIntervalMs));
    }
}

/**
 * @brief 从 NVS 加载临时绑定会话和最终设备凭据。
 */
void BackendService::LoadBindingState()
{
    Settings settings(kSettingsNamespace, false);
    std::lock_guard<std::mutex> lock(binding_mutex_);
    binding_code_ = settings.GetString(kBindingCodeKey);
    binding_session_token_ = settings.GetString(kBindingTokenKey);
    device_id_ = settings.GetString(kDeviceIdKey);
    device_access_token_ = settings.GetString(kDeviceTokenKey);
}

/**
 * @brief 处理业务 API 返回的设备认证失败。
 * @param status_code HTTP 状态码。
 * @param access_token 本次请求使用的 Bearer Token。
 */
void BackendService::HandleApiAuthenticationFailure(
    int status_code,
    const std::string &access_token)
{
    if ((status_code != 401 && status_code != 403)
        || !IsCurrentDeviceCredential(access_token))
    {
        return;
    }
    ScheduleDeviceStateClear(access_token, "业务 API 返回设备认证失败");
}

/**
 * @brief 判断给定 Token 是否仍是当前设备凭据。
 */
bool BackendService::IsCurrentDeviceCredential(const std::string &access_token)
{
    std::lock_guard<std::mutex> lock(binding_mutex_);
    return !access_token.empty() && device_access_token_ == access_token;
}

/**
 * @brief 把设备平台状态清理安排到 Application 主任务。
 */
void BackendService::ScheduleDeviceStateClear(
    const std::string &expected_access_token,
    const std::string &reason)
{
    if (!IsCurrentDeviceCredential(expected_access_token)
        || device_state_clear_scheduled_.exchange(true))
    {
        return;
    }
    Application::GetInstance().Schedule(
        [this, expected_access_token, reason]()
        {
            ClearDeviceState(expected_access_token, reason);
        });
}

/**
 * @brief 清除设备本地全部囤囤AI绑定凭据和关联运行状态。
 */
void BackendService::ClearDeviceState(
    const std::string &expected_access_token,
    const std::string &reason)
{
    {
        std::lock_guard<std::mutex> lock(binding_mutex_);
        if (expected_access_token.empty()
            || device_access_token_ != expected_access_token)
        {
            device_state_clear_scheduled_.store(false);
            return;
        }
        binding_code_.clear();
        binding_session_token_.clear();
        device_id_.clear();
        device_access_token_.clear();
        binding_page_dismissed_ = false;
    }

    Settings settings(kSettingsNamespace, true);
    settings.EraseKey(kBindingCodeKey);
    settings.EraseKey(kBindingTokenKey);
    settings.EraseKey(kDeviceIdKey);
    settings.EraseKey(kDeviceTokenKey);

    StopHeartbeatPublishing();
    notification_reconnect_requested_.store(false);
    notification_reconnect_delay_seconds_.store(kNotificationReconnectInitialSeconds);
    if (notification_reconnect_timer_ != nullptr)
    {
        esp_timer_stop(notification_reconnect_timer_);
    }
    std::unique_ptr<Mqtt> mqtt;
    {
        std::lock_guard<std::mutex> lock(notification_mutex_);
        active_notification_mqtt_.store(nullptr);
        heartbeat_topic_.clear();
        mqtt = std::move(notification_mqtt_);
        pending_notification_ = PendingNotification{};
        notification_hints_.fill(NotificationHint{});
        notification_hint_head_ = 0;
        notification_hint_tail_ = 0;
        notification_hint_count_ = 0;
    }
    if (mqtt != nullptr)
    {
        mqtt->Disconnect();
    }
    notification_playback_interrupted_.store(true);

    {
        std::lock_guard<std::mutex> lock(weather_mutex_);
        weather_snapshot_ = WeatherSnapshot{};
        weather_last_success_us_ = 0;
    }
    weather_refresh_requested_.store(false);

    if (pending_reminder_retry_timer_ != nullptr)
    {
        esp_timer_stop(pending_reminder_retry_timer_);
    }
    {
        std::lock_guard<std::mutex> lock(pending_reminder_mutex_);
        pending_reminder_snapshot_ = PendingReminderSnapshot{};
        pending_reminder_last_success_us_ = 0;
        pending_reminder_retry_index_ = 0;
    }
    pending_reminder_refresh_requested_.store(false);
    pending_reminder_retry_pending_.store(false);
    pending_reminder_retry_due_.store(false);

    {
        std::lock_guard<std::mutex> lock(dynamic_mcp_mutex_);
        mcp_manifest_refresh_requested_ = false;
    }
    ClearDynamicTools();
    HideBindingPage();
    ShowWeatherStatusIfUnavailable("");
    ShowPendingReminderStatusIfUnavailable(kUnboundPendingReminderPrompt);
    device_state_clear_scheduled_.store(false);
    ESP_LOGW(kTag, "已清除囤囤AI设备绑定和业务状态，原因=%s", reason.c_str());
}

/**
 * @brief 保存当前有效绑定码和会话 Token。
 * @param binding_code 屏幕显示数字码。
 * @param session_token 私有绑定会话 Token。
 */
void BackendService::SavePendingBinding(const std::string &binding_code,
                                        const std::string &session_token)
{
    {
        std::lock_guard<std::mutex> lock(binding_mutex_);
        binding_code_ = binding_code;
        binding_session_token_ = session_token;
    }
    Settings settings(kSettingsNamespace, true);
    settings.SetString(kBindingCodeKey, binding_code);
    settings.SetString(kBindingTokenKey, session_token);
}

/**
 * @brief 清除临时绑定码和会话 Token。
 */
void BackendService::ClearPendingBinding()
{
    {
        std::lock_guard<std::mutex> lock(binding_mutex_);
        binding_code_.clear();
        binding_session_token_.clear();
    }
    Settings settings(kSettingsNamespace, true);
    settings.EraseKey(kBindingCodeKey);
    settings.EraseKey(kBindingTokenKey);
}

/**
 * @brief 保存后端只签发一次的设备访问凭据。
 * @param device_id 后端设备 UUID。
 * @param access_token 设备 Bearer Token 明文。
 */
void BackendService::SaveDeviceCredential(const std::string &device_id,
                                          const std::string &access_token)
{
    {
        std::lock_guard<std::mutex> lock(binding_mutex_);
        device_id_ = device_id;
        device_access_token_ = access_token;
    }
    Settings settings(kSettingsNamespace, true);
    settings.SetString(kDeviceIdKey, device_id);
    settings.SetString(kDeviceTokenKey, access_token);
}
