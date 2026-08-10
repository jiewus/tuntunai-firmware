/**
 * @file backend_memo.cc
 * @brief 囤囤AI备忘录语音工具、屏保同步和退避重试实现。
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
 * @brief 校验语音参数并创建单个备忘录后台任务。
 * @param context 调用方分配且由本方法接管的任务上下文。
 */
void BackendService::StartMemoToolTask(MemoToolTaskContext *context)
{
    if (context == nullptr)
    {
        return;
    }
    if (!network_connected_.load())
    {
        FinishToolRequest(context->completion, "设备网络尚未连接，暂时无法操作备忘录。", true);
        delete context;
        return;
    }
    if ((context->operation == MemoToolOperation::Create
         || context->operation == MemoToolOperation::Update)
        && (context->content.empty()
            || context->content.size() > kMemoContentMaxBytes
            || context->content.find_first_not_of(" \t\r\n") == std::string::npos))
    {
        FinishToolRequest(context->completion, "备忘录内容为空或过长，请重新说明。", true);
        delete context;
        return;
    }
    if ((context->operation == MemoToolOperation::Update
         || context->operation == MemoToolOperation::Delete)
        && !IsSafeMemoId(context->memo_id))
    {
        FinishToolRequest(context->completion, "备忘录编号无效，请先查询目标备忘录。", true);
        delete context;
        return;
    }
    if ((context->operation == MemoToolOperation::Create
         || context->operation == MemoToolOperation::Update)
        && context->remind_at.size() > kMemoReminderTimeMaxBytes)
    {
        FinishToolRequest(context->completion, "备忘录提醒时间格式无效，请重新说明。", true);
        delete context;
        return;
    }
    if (context->operation == MemoToolOperation::Update
        && context->status != 1 && context->status != 2)
    {
        FinishToolRequest(context->completion, "备忘录状态无效，请重新查询目标备忘录。", true);
        delete context;
        return;
    }

    bool has_device_credential = false;
    {
        std::lock_guard<std::mutex> lock(binding_mutex_);
        has_device_credential = !device_access_token_.empty();
    }
    if (!has_device_credential)
    {
        FinishToolRequest(context->completion, "设备尚未绑定囤囤AI，无法操作备忘录。", true);
        delete context;
        return;
    }

    bool task_busy = false;
    {
        std::lock_guard<std::mutex> lock(memo_mutex_);
        if (memo_task_handle_ != nullptr)
        {
            task_busy = true;
        }
        else
        {
            memo_task_handle_ = backend_worker_task_handle_;
        }
    }

    if (task_busy)
    {
        FinishToolRequest(
            context->completion,
            "备忘录服务正在处理其他请求，请稍后再试。",
            true);
        delete context;
        return;
    }

    if (!EnqueueBackendJob(BackendJobType::MemoTool, context))
    {
        {
            std::lock_guard<std::mutex> lock(memo_mutex_);
            memo_task_handle_ = nullptr;
        }
        FinishToolRequest(
            context->completion,
            "设备后台任务队列已满，暂时无法操作备忘录。",
            true);
        delete context;
    }
}

/**
 * @brief 调用后端备忘录创建、列表、修改、删除或统计接口，并生成中文 MCP 结果。
 * @param context 已通过固件输入边界校验的任务上下文。
 */
void BackendService::RunMemoToolTask(MemoToolTaskContext &context)
{
    if (!network_connected_.load())
    {
        FinishToolRequest(context.completion, "设备网络尚未连接，暂时无法操作备忘录。", true);
        return;
    }
    std::string access_token;
    {
        std::lock_guard<std::mutex> lock(binding_mutex_);
        access_token = device_access_token_;
    }
    if (access_token.empty())
    {
        FinishToolRequest(context.completion, "设备尚未绑定囤囤AI，无法操作备忘录。", true);
        return;
    }

    if (context.operation == MemoToolOperation::Create)
    {
        const std::string request_body = BuildMemoCreateRequestJson(
            context.content, context.remind_at);
        if (request_body.empty())
        {
            FinishToolRequest(context.completion, "设备内存不足，无法生成备忘录请求。", true);
            return;
        }
        const HttpResponse response = SendJsonRequest(
            "POST", kMemosPath, access_token, request_body, kMemoResponseMaxBytes);
        cJSON *root = nullptr;
        cJSON *data = nullptr;
        std::string response_message;
        bool parsed = response.transport_succeeded
            && response.status_code == 201
            && ParseSuccessData(response.body, &root, &data, response_message);
        std::string memo_id;
        std::string saved_content;
        if (parsed)
        {
            parsed = ReadBoundedString(data, "id", 50, memo_id)
                && IsSafeMemoId(memo_id)
                && ReadBoundedString(
                    data, "content", kMemoContentMaxBytes, saved_content);
        }
        cJSON_Delete(root);
        if (!parsed)
        {
            ESP_LOGW(kTag, "创建备忘录失败，HTTP 状态码=%d", response.status_code);
            FinishToolRequest(
                context.completion,
                response_message.empty() ? "创建备忘录失败，请稍后重试。" : response_message,
                true);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(memo_mutex_);
            memo_snapshot_.valid = false;
            memo_snapshot_.contents.clear();
            memo_last_success_us_ = 0;
        }
        std::string result = "备忘录已创建，ID：" + memo_id + "，内容："
            + TruncateUtf8(saved_content, kMemoQueryItemMaxBytes);
        if (!context.remind_at.empty())
        {
            result += "。提醒时间：" + context.remind_at;
        }
        ESP_LOGI(kTag, "语音创建备忘录成功");
        FinishToolRequest(context.completion, result, false);
        return;
    }

    if (context.operation == MemoToolOperation::Query)
    {
        std::string path = std::string(kMemosPath) + "?page_index=1&page_size=5";
        if (context.status != 0)
        {
            path += "&status=" + std::to_string(context.status);
        }
        if (context.time_range != 0)
        {
            path += "&time_range=" + std::to_string(context.time_range);
        }
        const HttpResponse response = SendJsonRequest(
            "GET", path.c_str(), access_token, "", kMemoResponseMaxBytes);
        cJSON *root = nullptr;
        cJSON *data = nullptr;
        std::string response_message;
        bool parsed = response.transport_succeeded
            && response.status_code == 200
            && ParseSuccessData(response.body, &root, &data, response_message);
        uint32_t total_count = 0;
        cJSON *records = nullptr;
        if (parsed)
        {
            records = cJSON_GetObjectItemCaseSensitive(data, "records");
            parsed = ReadUnsignedInteger(
                         cJSON_GetObjectItemCaseSensitive(data, "total_count"),
                         true, total_count)
                && cJSON_IsArray(records)
                && cJSON_GetArraySize(records) <= static_cast<int>(kMaximumScreensaverMemoCount);
        }

        std::string result;
        if (parsed && total_count == 0)
        {
            result = "没有找到符合条件的备忘录。";
        }
        else if (parsed)
        {
            const int record_count = cJSON_GetArraySize(records);
            if (record_count == 0)
            {
                parsed = false;
            }
            else
            {
                result = "共找到" + std::to_string(total_count) + "条备忘录，以下是前"
                    + std::to_string(record_count) + "条：";
                for (int index = 0; index < record_count && parsed; ++index)
                {
                    cJSON *record = cJSON_GetArrayItem(records, index);
                    std::string memo_id;
                    std::string content;
                    uint32_t memo_status = 0;
                    parsed = cJSON_IsObject(record)
                        && ReadBoundedString(record, "id", 50, memo_id)
                        && IsSafeMemoId(memo_id)
                        && ReadBoundedString(
                            record, "content", kMemoContentMaxBytes, content)
                        && ReadUnsignedInteger(
                            cJSON_GetObjectItemCaseSensitive(record, "status"),
                            false, memo_status)
                        && (memo_status == 1 || memo_status == 2);
                    if (!parsed)
                    {
                        break;
                    }
                    result += std::to_string(index + 1) + ". ID：" + memo_id
                        + "，内容：" + TruncateUtf8(content, kMemoQueryItemMaxBytes)
                        + (memo_status == 1 ? "，状态：未完成" : "，状态：已完成");
                    cJSON *remind_at = cJSON_GetObjectItemCaseSensitive(record, "remind_at");
                    if (cJSON_IsString(remind_at)
                        && remind_at->valuestring != nullptr
                        && std::strlen(remind_at->valuestring) <= kMemoReminderTimeMaxBytes)
                    {
                        result += "，提醒时间：";
                        result += remind_at->valuestring;
                    }
                    result += "；";
                }
            }
        }
        cJSON_Delete(root);
        if (!parsed || result.size() > kMemoQueryResultMaxBytes)
        {
            ESP_LOGW(kTag, "查询备忘录失败，HTTP 状态码=%d", response.status_code);
            FinishToolRequest(
                context.completion,
                response_message.empty() ? "查询备忘录失败，请稍后重试。" : response_message,
                true);
            return;
        }
        ESP_LOGI(kTag, "语音查询备忘录成功，匹配数量=%u", static_cast<unsigned>(total_count));
        FinishToolRequest(context.completion, result, false);
        return;
    }

    if (context.operation == MemoToolOperation::Update)
    {
        const std::string request_body = BuildMemoUpdateRequestJson(
            context.content, context.remind_at, context.status);
        if (request_body.empty())
        {
            FinishToolRequest(context.completion, "设备内存不足，无法生成备忘录修改请求。", true);
            return;
        }
        const std::string path = std::string(kMemosPath) + "/" + context.memo_id;
        const HttpResponse response = SendJsonRequest(
            "PUT", path.c_str(), access_token, request_body, kMemoResponseMaxBytes);
        cJSON *root = nullptr;
        cJSON *data = nullptr;
        std::string response_message;
        bool parsed = response.transport_succeeded
            && response.status_code == 200
            && ParseSuccessData(response.body, &root, &data, response_message);
        std::string saved_id;
        std::string saved_content;
        if (parsed)
        {
            parsed = ReadBoundedString(data, "id", 50, saved_id)
                && saved_id == context.memo_id
                && ReadBoundedString(
                    data, "content", kMemoContentMaxBytes, saved_content);
        }
        cJSON_Delete(root);
        if (!parsed)
        {
            ESP_LOGW(kTag, "修改备忘录失败，HTTP 状态码=%d", response.status_code);
            FinishToolRequest(
                context.completion,
                response_message.empty() ? "修改备忘录失败，请稍后重试。" : response_message,
                true);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(memo_mutex_);
            memo_snapshot_.valid = false;
            memo_snapshot_.contents.clear();
            memo_last_success_us_ = 0;
        }
        std::string result = "备忘录已修改，ID：" + saved_id + "，内容："
            + TruncateUtf8(saved_content, kMemoQueryItemMaxBytes);
        if (!context.remind_at.empty())
        {
            result += "，提醒时间：" + context.remind_at;
        }
        result += context.status == 2 ? "，状态：已完成。" : "，状态：未完成。";
        ESP_LOGI(kTag, "语音修改备忘录成功");
        FinishToolRequest(context.completion, result, false);
        return;
    }

    if (context.operation == MemoToolOperation::Delete)
    {
        const std::string path = std::string(kMemosPath) + "/" + context.memo_id;
        const HttpResponse response = SendJsonRequest(
            "DELETE", path.c_str(), access_token, "", kMemoResponseMaxBytes);
        std::string response_message;
        const bool parsed = response.transport_succeeded
            && response.status_code == 200
            && ParseSuccessEnvelope(response.body, response_message);
        if (!parsed)
        {
            ESP_LOGW(kTag, "删除备忘录失败，HTTP 状态码=%d", response.status_code);
            FinishToolRequest(
                context.completion,
                response_message.empty() ? "删除备忘录失败，请稍后重试。" : response_message,
                true);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(memo_mutex_);
            memo_snapshot_.valid = false;
            memo_snapshot_.contents.clear();
            memo_last_success_us_ = 0;
        }
        ESP_LOGI(kTag, "语音删除备忘录成功");
        FinishToolRequest(
            context.completion,
            "备忘录已删除，ID：" + context.memo_id + "。",
            false);
        return;
    }

    const HttpResponse response = SendJsonRequest(
        "GET", kMemoStatisticsPath, access_token, "", kMemoResponseMaxBytes);
    cJSON *root = nullptr;
    cJSON *data = nullptr;
    std::string response_message;
    bool parsed = response.transport_succeeded
        && response.status_code == 200
        && ParseSuccessData(response.body, &root, &data, response_message);
    uint32_t total_count = 0;
    uint32_t unexpired_count = 0;
    uint32_t today_count = 0;
    uint32_t tomorrow_count = 0;
    uint32_t this_week_count = 0;
    if (parsed)
    {
        parsed = ReadUnsignedInteger(
                     cJSON_GetObjectItemCaseSensitive(data, "total_count"), true, total_count)
            && ReadUnsignedInteger(
                cJSON_GetObjectItemCaseSensitive(data, "unexpired_count"), true, unexpired_count)
            && ReadUnsignedInteger(
                cJSON_GetObjectItemCaseSensitive(data, "today_count"), true, today_count)
            && ReadUnsignedInteger(
                cJSON_GetObjectItemCaseSensitive(data, "tomorrow_count"), true, tomorrow_count)
            && ReadUnsignedInteger(
                cJSON_GetObjectItemCaseSensitive(data, "this_week_count"), true, this_week_count);
    }
    cJSON_Delete(root);
    if (!parsed)
    {
        ESP_LOGW(kTag, "统计备忘录失败，HTTP 状态码=%d", response.status_code);
        FinishToolRequest(
            context.completion,
            response_message.empty() ? "统计备忘录失败，请稍后重试。" : response_message,
            true);
        return;
    }

    std::string result = "备忘录统计：全部" + std::to_string(total_count)
        + "条，未到期" + std::to_string(unexpired_count)
        + "条，今天" + std::to_string(today_count)
        + "条，明天" + std::to_string(tomorrow_count)
        + "条，本周" + std::to_string(this_week_count) + "条。";
    ESP_LOGI(kTag, "语音统计备忘录成功");
    FinishToolRequest(context.completion, result, false);
}



/**
 * @brief 在屏保备忘录缓存需要更新时把请求加入常驻后端 Worker。
 * @param force_refresh true 忽略最近成功时间；false 遵守 30 分钟本地新鲜周期。
 */
void BackendService::StartMemoSync(bool force_refresh)
{
    if (notification_playback_active_.load())
    {
        if (force_refresh || memo_retry_due_.load() || memo_refresh_requested_.load())
        {
            memo_refresh_requested_.store(true);
        }
        return;
    }
    if (!screensaver_active_.load() || !network_connected_.load())
    {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(binding_mutex_);
        if (device_access_token_.empty())
        {
            return;
        }
    }
    bool mqtt_refresh_requested = false;
    {
        std::lock_guard<std::mutex> lock(memo_mutex_);
        if (memo_task_handle_ != nullptr)
        {
            return;
        }
        mqtt_refresh_requested = memo_refresh_requested_.exchange(false);
        const bool retry_due = memo_retry_due_.load();
        if (!force_refresh && memo_retry_pending_.load() && !retry_due
            && !mqtt_refresh_requested)
        {
            return;
        }
        force_refresh = force_refresh || mqtt_refresh_requested || retry_due;
        const int64_t now_us = esp_timer_get_time();
        if (!force_refresh && memo_snapshot_.valid && memo_last_success_us_ > 0
            && now_us - memo_last_success_us_ < kMemoRefreshIntervalUs)
        {
            return;
        }
        memo_task_handle_ = backend_worker_task_handle_;
        memo_retry_pending_.store(false);
        memo_retry_due_.store(false);
        if (memo_retry_timer_ != nullptr)
        {
            esp_timer_stop(memo_retry_timer_);
        }
    }

    if (!EnqueueBackendJob(BackendJobType::MemoSync))
    {
        {
            std::lock_guard<std::mutex> lock(memo_mutex_);
            memo_task_handle_ = nullptr;
            if (mqtt_refresh_requested)
            {
                memo_refresh_requested_.store(true);
            }
        }
        ESP_LOGE(kTag, "后端 Worker 队列已满，无法安排备忘录同步");
        ShowMemoStatusIfUnavailable("备忘录任务启动失败");
        ScheduleMemoRetry();
    }
}

/**
 * @brief 获取屏保前 5 条未完成备忘录并更新运行内存缓存。
 */
void BackendService::RunMemoSync()
{
    if (notification_playback_active_.load())
    {
        memo_refresh_requested_.store(true);
        return;
    }
    if (!screensaver_active_.load() || !network_connected_.load())
    {
        return;
    }
    std::string access_token;
    {
        std::lock_guard<std::mutex> lock(binding_mutex_);
        access_token = device_access_token_;
    }
    if (access_token.empty())
    {
        ShowMemoStatusIfUnavailable(kUnboundMemoPrompt);
        return;
    }

    const HttpResponse response = SendJsonRequest(
        "GET", kMemoScreensaverPath, access_token, "", kMemoResponseMaxBytes);
    cJSON *root = nullptr;
    cJSON *data = nullptr;
    std::string response_message;
    bool parsed = response.transport_succeeded
        && response.status_code == 200
        && ParseSuccessData(response.body, &root, &data, response_message);
    MemoSnapshot snapshot;
    cJSON *records = nullptr;
    if (parsed)
    {
        records = cJSON_GetObjectItemCaseSensitive(data, "records");
        parsed = cJSON_IsArray(records)
            && cJSON_GetArraySize(records) <= static_cast<int>(kMaximumScreensaverMemoCount);
    }
    if (parsed)
    {
        const int record_count = cJSON_GetArraySize(records);
        snapshot.contents.reserve(static_cast<size_t>(record_count));
        for (int index = 0; index < record_count; ++index)
        {
            cJSON *record = cJSON_GetArrayItem(records, index);
            std::string content;
            if (!cJSON_IsObject(record)
                || !ReadBoundedString(record, "content", kMemoContentMaxBytes, content))
            {
                parsed = false;
                break;
            }

            cJSON *remind_at_item = cJSON_GetObjectItemCaseSensitive(record, "remind_at");
            std::string reminder_line;
            if (cJSON_IsNull(remind_at_item))
            {
                reminder_line = "未设置时间";
            }
            else
            {
                std::string remind_at;
                if (!ReadBoundedString(
                        record, "remind_at", kMemoReminderTimeMaxBytes, remind_at)
                    || !FormatMemoReminderLine(remind_at, reminder_line))
                {
                    parsed = false;
                    break;
                }
            }
            snapshot.contents.push_back(
                "[" + reminder_line + "]\n" + std::move(content));
        }
    }
    cJSON_Delete(root);

    if (!parsed)
    {
        ESP_LOGW(kTag, "备忘录同步失败，HTTP 状态码=%d", response.status_code);
        ShowMemoStatusIfUnavailable(
            response.status_code == 401 || response.status_code == 403
                ? "设备认证已失效"
                : "备忘录同步失败");
        ScheduleMemoRetry();
        return;
    }

    snapshot.valid = true;
    {
        std::lock_guard<std::mutex> lock(memo_mutex_);
        memo_snapshot_ = snapshot;
        memo_last_success_us_ = esp_timer_get_time();
    }
    ResetMemoRetry();
    if (screensaver_active_.load())
    {
        ShowMemoSnapshot(snapshot);
    }
    ESP_LOGI(kTag, "备忘录同步成功，显示数量=%u",
             static_cast<unsigned>(snapshot.contents.size()));
}

/**
 * @brief 把有效备忘录快照写入当前 LCD 表盘。
 * @param snapshot 已完成字段和数量校验的运行内存快照。
 */
void BackendService::ShowMemoSnapshot(const MemoSnapshot &snapshot)
{
    if (!screensaver_active_.load() || !snapshot.valid)
    {
        return;
    }
    Display *display = Board::GetInstance().GetDisplay();
    if (display != nullptr)
    {
        display->SetScreensaverMemos(snapshot.contents);
    }
}

/**
 * @brief 没有历史备忘录可保留时，向屏保备忘录区域写入固定状态文本。
 * @param message 加载、网络、认证或错误状态。
 */
void BackendService::ShowMemoStatusIfUnavailable(const std::string &message)
{
    if (!screensaver_active_.load())
    {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(memo_mutex_);
        if (memo_snapshot_.valid)
        {
            return;
        }
    }
    Display *display = Board::GetInstance().GetDisplay();
    if (display != nullptr)
    {
        display->SetScreensaverMemos({message});
    }
}

/**
 * @brief 在屏保显示期间使用最近缓存并按需同步天气和备忘录。
 * @param active true 表示屏保可见；false 表示屏保已经退出。
 */
void BackendService::OnScreensaverChanged(bool active)
{
    screensaver_active_.store(active);
    if (!active)
    {
        return;
    }

    Display *display = Board::GetInstance().GetDisplay();
    if (display == nullptr)
    {
        return;
    }
    MemoSnapshot memo_snapshot;
    {
        std::lock_guard<std::mutex> lock(memo_mutex_);
        memo_snapshot = memo_snapshot_;
    }
    if (memo_snapshot.valid)
    {
        ShowMemoSnapshot(memo_snapshot);
    }
    else
    {
        bool has_device_credential = false;
        {
            std::lock_guard<std::mutex> lock(binding_mutex_);
            has_device_credential = !device_access_token_.empty();
        }
        if (!has_device_credential)
        {
            ShowMemoStatusIfUnavailable(kUnboundMemoPrompt);
        }
        else if (!network_connected_.load())
        {
            ShowMemoStatusIfUnavailable("等待网络连接");
        }
        else
        {
            ShowMemoStatusIfUnavailable("备忘录加载中");
        }
    }

    WeatherSnapshot snapshot;
    {
        std::lock_guard<std::mutex> lock(weather_mutex_);
        snapshot = weather_snapshot_;
    }
    if (snapshot.valid)
    {
        ShowWeatherSnapshot(snapshot);
    }
    else
    {
        bool has_device_credential = false;
        {
            std::lock_guard<std::mutex> lock(binding_mutex_);
            has_device_credential = !device_access_token_.empty();
        }
        if (!has_device_credential)
        {
            ShowWeatherStatusIfUnavailable("");
        }
        else if (!network_connected_.load())
        {
            ShowWeatherStatusIfUnavailable("等待网络连接");
        }
        else
        {
            ShowWeatherStatusIfUnavailable("天气加载中");
        }
    }
    StartWeatherSync(false);
    StartMemoSync(false);
}

/**
 * @brief esp_timer 周期回调，每 30 分钟为屏保备忘录执行一次兜底同步检查。
 * @param context 指向 BackendService 单例。
 */
void BackendService::MemoTimerCallback(void *context)
{
    auto *service = static_cast<BackendService *>(context);
    service->StartMemoSync(false);
}

/**
 * @brief 备忘录退避重试定时器回调，标记到期后尝试强制刷新一次。
 * @param context 指向 BackendService 单例。
 */
void BackendService::MemoRetryTimerCallback(void *context)
{
    auto *service = static_cast<BackendService *>(context);
    service->memo_retry_due_.store(true);
    service->StartMemoSync(true);
}

/**
 * @brief 按固定退避序列安排下一次备忘录刷新，达到 30 分钟后保持该间隔。
 */
void BackendService::ScheduleMemoRetry()
{
    uint32_t delay_minutes = 0;
    esp_err_t timer_error = ESP_ERR_INVALID_STATE;
    {
        std::lock_guard<std::mutex> lock(memo_mutex_);
        if (memo_retry_timer_ == nullptr)
        {
            return;
        }
        const size_t interval_index = std::min(
            memo_retry_index_, kMemoRetryIntervalsMinutes.size() - 1);
        delay_minutes = kMemoRetryIntervalsMinutes[interval_index];
        if (memo_retry_index_ + 1 < kMemoRetryIntervalsMinutes.size())
        {
            ++memo_retry_index_;
        }
        esp_timer_stop(memo_retry_timer_);
        memo_retry_pending_.store(true);
        memo_retry_due_.store(false);
        timer_error = esp_timer_start_once(
            memo_retry_timer_,
            static_cast<uint64_t>(delay_minutes) * 60ULL * 1000ULL * 1000ULL);
        if (timer_error != ESP_OK)
        {
            memo_retry_pending_.store(false);
        }
    }
    if (timer_error == ESP_OK)
    {
        ESP_LOGW(kTag, "备忘录将在%u分钟后重试同步",
                 static_cast<unsigned>(delay_minutes));
    }
    else
    {
        ESP_LOGE(kTag, "备忘录重试定时器启动失败，原因=%s",
                 esp_err_to_name(timer_error));
    }
}

/**
 * @brief 备忘录同步成功后停止退避定时器，并让下一轮失败重新从 1 分钟开始。
 */
void BackendService::ResetMemoRetry()
{
    std::lock_guard<std::mutex> lock(memo_mutex_);
    memo_retry_index_ = 0;
    memo_retry_pending_.store(false);
    memo_retry_due_.store(false);
    if (memo_retry_timer_ != nullptr)
    {
        esp_timer_stop(memo_retry_timer_);
    }
}
