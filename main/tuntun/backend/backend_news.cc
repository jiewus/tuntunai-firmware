/**
 * @file backend_news.cc
 * @brief 囤囤AI每日新闻简报公共 MCP 工具实现。
 */

#include "tuntun/backend/backend_service.h"
#include "tuntun/backend/backend_utils.h"

#include <cJSON.h>
#include <esp_log.h>

#include <cstring>
#include <memory>
#include <new>
#include <utility>

using namespace tuntun::backend_internal;

/**
 * @brief 校验网络、绑定和并发状态后把新闻简报请求加入常驻 Worker。
 */
void BackendService::StartNewsBriefingTask(DynamicToolCompletion completion)
{
    if (!network_connected_.load())
    {
        completion("设备网络尚未连接，暂时无法获取新闻简报。", true);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(binding_mutex_);
        if (device_access_token_.empty())
        {
            completion("设备尚未绑定囤囤AI，无法获取新闻简报。", true);
            return;
        }
    }
    {
        std::lock_guard<std::mutex> lock(dynamic_mcp_mutex_);
        if (news_briefing_task_handle_ != nullptr)
        {
            completion("新闻简报正在获取中，请稍后再试。", true);
            return;
        }
        news_briefing_task_handle_ = backend_worker_task_handle_;
    }
    auto *context = new (std::nothrow) NewsBriefingTaskContext();
    if (context == nullptr)
    {
        {
            std::lock_guard<std::mutex> lock(dynamic_mcp_mutex_);
            news_briefing_task_handle_ = nullptr;
        }
        completion("设备内存不足，无法获取新闻简报。", true);
        return;
    }
    context->completion = std::move(completion);
    if (!EnqueueBackendJob(BackendJobType::NewsBriefing, context))
    {
        auto rejected = std::move(context->completion);
        delete context;
        {
            std::lock_guard<std::mutex> lock(dynamic_mcp_mutex_);
            news_briefing_task_handle_ = nullptr;
        }
        rejected("设备后台任务队列已满，暂时无法获取新闻简报。", true);
    }
}

/**
 * @brief 使用设备 Token 获取后端已经生成或复用的今日新闻简报。
 */
void BackendService::RunNewsBriefingTask(DynamicToolCompletion &completion)
{
    std::string access_token;
    {
        std::lock_guard<std::mutex> lock(binding_mutex_);
        access_token = device_access_token_;
    }
    if (!network_connected_.load() || access_token.empty())
    {
        FinishToolRequest(completion, "设备网络或认证状态不可用，暂时无法获取新闻简报。", true);
        return;
    }
    const HttpResponse response = SendJsonRequest(
        "GET",
        kNewsBriefingPath,
        access_token,
        "",
        kNotificationResponseMaxBytes,
        kHttpTimeoutMs);
    cJSON *root = nullptr;
    cJSON *data = nullptr;
    std::string response_message;
    const bool envelope_valid = response.transport_succeeded
        && response.status_code == 200
        && ParseSuccessData(response.body, &root, &data, response_message);
    if (!envelope_valid)
    {
        cJSON_Delete(root);
        ESP_LOGW(kTag, "每日新闻简报请求失败，HTTP 状态码=%d", response.status_code);
        const bool authentication_failed = response.status_code == 401 || response.status_code == 403;
        FinishToolRequest(
            completion,
            authentication_failed
                ? "设备认证已失效，请重新绑定设备。"
                : "每日新闻简报获取失败，请稍后重试。",
            true);
        return;
    }
    std::string content;
    const bool content_valid = ReadBoundedString(
        data,
        "content",
        kNotificationTextMaxBytes,
        content);
    cJSON_Delete(root);
    if (!content_valid || content.empty())
    {
        FinishToolRequest(completion, "平台返回的新闻简报格式无效。", true);
        return;
    }
    FinishToolRequest(completion, content, false);
}
