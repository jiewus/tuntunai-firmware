/**
 * @file backend_custom_reminder.cc
 * @brief 囤囤AI自定义提醒语音管理实现。
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
 * @brief 校验语音参数并创建单个自定义提醒后台任务。
 * @param context 调用方分配且由本方法接管的任务上下文。
 */
void BackendService::StartCustomReminderToolTask(CustomReminderToolTaskContext *context)
{
    if (context == nullptr)
    {
        return;
    }
    if (!network_connected_.load())
    {
        FinishToolRequest(context->completion, "设备网络尚未连接，暂时无法操作自定义提醒。", true);
        delete context;
        return;
    }

    const bool saves_reminder = context->operation == CustomReminderToolOperation::Create
        || context->operation == CustomReminderToolOperation::Update;
    if (saves_reminder
        && (context->content.empty()
            || context->content.size() > kCustomReminderContentMaxBytes
            || context->content.find_first_not_of(" \t\r\n") == std::string::npos))
    {
        FinishToolRequest(context->completion, "自定义提醒内容为空或过长，请重新说明。", true);
        delete context;
        return;
    }
    if (saves_reminder
        && (context->schedule_type < 1 || context->schedule_type > 2
            || context->first_run_at.empty()
            || context->first_run_at.size() > kCustomReminderTimeMaxBytes))
    {
        FinishToolRequest(context->completion, "自定义提醒类型或首次执行时间无效，请重新说明。", true);
        delete context;
        return;
    }
    if (saves_reminder && context->allowed_time_ranges.size() > kCustomReminderRangesMaxBytes)
    {
        FinishToolRequest(context->completion, "自定义提醒允许时段过多，请精简后重试。", true);
        delete context;
        return;
    }
    std::vector<CustomReminderTimeRange> ranges;
    if (saves_reminder
        && (!ParseCustomReminderRanges(context->allowed_time_ranges, ranges)
            || (context->schedule_type == 1
                && (context->interval_minutes != 0 || !ranges.empty()))
            || (context->schedule_type == 2
                && (context->interval_minutes < 1
                    || context->interval_minutes > kMaximumCustomReminderIntervalMinutes))))
    {
        FinishToolRequest(
            context->completion,
            "自定义提醒间隔或允许时段无效，时段应类似08:00-12:00,14:00-22:00。",
            true);
        delete context;
        return;
    }

    const bool requires_id = context->operation == CustomReminderToolOperation::Update
        || context->operation == CustomReminderToolOperation::Delete
        || context->operation == CustomReminderToolOperation::Enable
        || context->operation == CustomReminderToolOperation::Disable;
    if (requires_id && !IsSafeResourceId(context->reminder_id))
    {
        FinishToolRequest(context->completion, "自定义提醒编号无效，请先查询目标提醒。", true);
        delete context;
        return;
    }
    if (context->operation == CustomReminderToolOperation::Query
        && (context->status_filter < 0 || context->status_filter > 3
            || context->schedule_type < 0 || context->schedule_type > 2))
    {
        FinishToolRequest(context->completion, "自定义提醒查询条件无效，请重新说明。", true);
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
        FinishToolRequest(context->completion, "设备尚未绑定囤囤AI，无法操作自定义提醒。", true);
        delete context;
        return;
    }

    bool task_busy = false;
    {
        std::lock_guard<std::mutex> lock(custom_reminder_mutex_);
        if (custom_reminder_task_handle_ != nullptr)
        {
            task_busy = true;
        }
        else
        {
            custom_reminder_task_handle_ = backend_worker_task_handle_;
        }
    }
    if (task_busy)
    {
        FinishToolRequest(context->completion, "自定义提醒服务正在处理其他请求，请稍后再试。", true);
        delete context;
        return;
    }
    if (!EnqueueBackendJob(BackendJobType::CustomReminderTool, context))
    {
        {
            std::lock_guard<std::mutex> lock(custom_reminder_mutex_);
            custom_reminder_task_handle_ = nullptr;
        }
        FinishToolRequest(
            context->completion,
            "设备后台任务队列已满，暂时无法操作自定义提醒。",
            true);
        delete context;
    }
}

/**
 * @brief 调用后端自定义提醒接口并生成中文 MCP 结果。
 * @param context 已通过固件输入边界校验的任务上下文。
 */
void BackendService::RunCustomReminderToolTask(CustomReminderToolTaskContext &context)
{
    if (!network_connected_.load())
    {
        FinishToolRequest(context.completion, "设备网络尚未连接，暂时无法操作自定义提醒。", true);
        return;
    }
    std::string access_token;
    {
        std::lock_guard<std::mutex> lock(binding_mutex_);
        access_token = device_access_token_;
    }
    if (access_token.empty())
    {
        FinishToolRequest(context.completion, "设备尚未绑定囤囤AI，无法操作自定义提醒。", true);
        return;
    }

    const auto read_detail = [&](const std::string &reminder_id,
                                 CustomReminderDetail &detail,
                                 std::string &response_message) -> bool
    {
        const std::string path = std::string(kCustomRemindersPath) + "/" + reminder_id;
        const HttpResponse response = SendJsonRequest(
            "GET", path.c_str(), access_token, "", kCustomReminderResponseMaxBytes);
        cJSON *root = nullptr;
        cJSON *data = nullptr;
        const bool parsed = response.transport_succeeded
            && response.status_code == 200
            && ParseSuccessData(response.body, &root, &data, response_message)
            && ParseCustomReminderDetail(data, detail)
            && detail.id == reminder_id;
        cJSON_Delete(root);
        if (!parsed)
        {
            ESP_LOGW(kTag, "查询自定义提醒详情失败，HTTP 状态码=%d", response.status_code);
        }
        return parsed;
    };

    const auto format_ranges = [](const std::vector<CustomReminderTimeRange> &ranges)
    {
        if (ranges.empty())
        {
            return std::string("全天");
        }
        std::string result;
        for (size_t index = 0; index < ranges.size(); ++index)
        {
            if (index > 0)
            {
                result += "、";
            }
            result += ranges[index].start_time.substr(0, 5)
                + "至" + ranges[index].end_time.substr(0, 5);
        }
        return result;
    };

    if (context.operation == CustomReminderToolOperation::Create)
    {
        std::vector<CustomReminderTimeRange> ranges;
        if (!ParseCustomReminderRanges(context.allowed_time_ranges, ranges))
        {
            FinishToolRequest(context.completion, "自定义提醒允许时段格式无效。", true);
            return;
        }
        const std::string request_body = BuildCustomReminderRequestJson(
            context.content,
            context.schedule_type,
            1,
            context.first_run_at,
            context.interval_minutes,
            ranges);
        if (request_body.empty())
        {
            FinishToolRequest(context.completion, "设备内存不足，无法生成自定义提醒请求。", true);
            return;
        }
        const HttpResponse response = SendJsonRequest(
            "POST",
            kCustomRemindersPath,
            access_token,
            request_body,
            kCustomReminderResponseMaxBytes);
        cJSON *root = nullptr;
        cJSON *data = nullptr;
        std::string response_message;
        CustomReminderDetail detail;
        const bool parsed = response.transport_succeeded
            && response.status_code == 201
            && ParseSuccessData(response.body, &root, &data, response_message)
            && ParseCustomReminderDetail(data, detail);
        cJSON_Delete(root);
        if (!parsed)
        {
            ESP_LOGW(kTag, "创建自定义提醒失败，HTTP 状态码=%d", response.status_code);
            FinishToolRequest(
                context.completion,
                response_message.empty() ? "创建自定义提醒失败，请稍后重试。" : response_message,
                true);
            return;
        }
        std::string result = "自定义提醒已创建，ID：" + detail.id
            + "，内容：" + TruncateUtf8(detail.content, kCustomReminderQueryItemMaxBytes)
            + "，首次执行时间：" + detail.first_run_at;
        if (detail.schedule_type == 2)
        {
            result += "，每隔" + std::to_string(detail.interval_minutes)
                + "分钟执行，允许时段：" + format_ranges(detail.ranges);
        }
        result += "。";
        ESP_LOGI(kTag, "语音创建自定义提醒成功");
        FinishToolRequest(context.completion, result, false);
        return;
    }

    if (context.operation == CustomReminderToolOperation::Query)
    {
        std::string path = std::string(kCustomRemindersPath) + "?page_index=1&page_size=5";
        if (context.status_filter != 0)
        {
            const int backend_status = context.status_filter == 1
                ? 1
                : (context.status_filter == 2 ? 0 : 2);
            path += "&status=" + std::to_string(backend_status);
        }
        if (context.schedule_type != 0)
        {
            path += "&schedule_type=" + std::to_string(context.schedule_type);
        }
        const HttpResponse response = SendJsonRequest(
            "GET", path.c_str(), access_token, "", kCustomReminderResponseMaxBytes);
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
                         true,
                         total_count)
                && cJSON_IsArray(records)
                && cJSON_GetArraySize(records)
                    <= static_cast<int>(kMaximumCustomReminderQueryCount);
        }

        std::string result;
        if (parsed && total_count == 0)
        {
            result = "没有找到符合条件的自定义提醒。";
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
                result = "共找到" + std::to_string(total_count) + "条自定义提醒，以下是前"
                    + std::to_string(record_count) + "条：";
                for (int index = 0; index < record_count && parsed; ++index)
                {
                    CustomReminderDetail detail;
                    parsed = ParseCustomReminderDetail(
                        cJSON_GetArrayItem(records, index), detail);
                    if (!parsed)
                    {
                        break;
                    }
                    const char *status_text = detail.status == 0
                        ? "已停用"
                        : (detail.status == 1 ? "已启用" : "已完成");
                    result += std::to_string(index + 1) + ". ID：" + detail.id
                        + "，内容：" + TruncateUtf8(
                            detail.content, kCustomReminderQueryItemMaxBytes)
                        + "，状态：" + status_text
                        + "，类型：" + (detail.schedule_type == 1 ? "一次性" : "间隔循环")
                        + "，首次执行时间：" + detail.first_run_at;
                    if (detail.schedule_type == 2)
                    {
                        result += "，间隔" + std::to_string(detail.interval_minutes)
                            + "分钟，允许时段：" + format_ranges(detail.ranges);
                    }
                    result += "；";
                }
            }
        }
        cJSON_Delete(root);
        if (!parsed || result.size() > kCustomReminderQueryResultMaxBytes)
        {
            ESP_LOGW(kTag, "查询自定义提醒失败，HTTP 状态码=%d", response.status_code);
            FinishToolRequest(
                context.completion,
                response_message.empty() ? "查询自定义提醒失败，请稍后重试。" : response_message,
                true);
            return;
        }
        ESP_LOGI(kTag, "语音查询自定义提醒成功，匹配数量=%u", static_cast<unsigned>(total_count));
        FinishToolRequest(context.completion, result, false);
        return;
    }

    if (context.operation == CustomReminderToolOperation::Delete)
    {
        const std::string path = std::string(kCustomRemindersPath) + "/" + context.reminder_id;
        const HttpResponse response = SendJsonRequest(
            "DELETE", path.c_str(), access_token, "", kCustomReminderResponseMaxBytes);
        std::string response_message;
        const bool parsed = response.transport_succeeded
            && response.status_code == 200
            && ParseSuccessEnvelope(response.body, response_message);
        if (!parsed)
        {
            ESP_LOGW(kTag, "删除自定义提醒失败，HTTP 状态码=%d", response.status_code);
            FinishToolRequest(
                context.completion,
                response_message.empty() ? "删除自定义提醒失败，请稍后重试。" : response_message,
                true);
            return;
        }
        ESP_LOGI(kTag, "语音删除自定义提醒成功");
        FinishToolRequest(
            context.completion,
            "自定义提醒已删除，ID：" + context.reminder_id + "。",
            false);
        return;
    }

    CustomReminderDetail existing;
    std::string response_message;
    if (!read_detail(context.reminder_id, existing, response_message))
    {
        FinishToolRequest(
            context.completion,
            response_message.empty() ? "读取自定义提醒失败，请稍后重试。" : response_message,
            true);
        return;
    }
    if (existing.status == 2)
    {
        FinishToolRequest(context.completion, "已完成的一次性提醒不能编辑或重新启停。", true);
        return;
    }

    std::string content = existing.content;
    std::string first_run_at = existing.first_run_at;
    int schedule_type = existing.schedule_type;
    int interval_minutes = existing.interval_minutes;
    std::vector<CustomReminderTimeRange> ranges = existing.ranges;
    int target_status = existing.status;
    const char *operation_text = "编辑";
    if (context.operation == CustomReminderToolOperation::Update)
    {
        content = context.content;
        first_run_at = context.first_run_at;
        schedule_type = context.schedule_type;
        interval_minutes = context.interval_minutes;
        if (!ParseCustomReminderRanges(context.allowed_time_ranges, ranges))
        {
            FinishToolRequest(context.completion, "自定义提醒允许时段格式无效。", true);
            return;
        }
    }
    else
    {
        target_status = context.operation == CustomReminderToolOperation::Enable ? 1 : 0;
        operation_text = target_status == 1 ? "启用" : "停用";
        if (existing.status == target_status)
        {
            FinishToolRequest(
                context.completion,
                "自定义提醒已经处于" + std::string(target_status == 1 ? "启用" : "停用")
                    + "状态，ID：" + existing.id + "。",
                false);
            return;
        }
    }

    const std::string request_body = BuildCustomReminderRequestJson(
        content,
        schedule_type,
        target_status,
        first_run_at,
        interval_minutes,
        ranges);
    if (request_body.empty())
    {
        FinishToolRequest(context.completion, "设备内存不足，无法生成自定义提醒修改请求。", true);
        return;
    }
    const std::string path = std::string(kCustomRemindersPath) + "/" + context.reminder_id;
    const HttpResponse response = SendJsonRequest(
        "PUT", path.c_str(), access_token, request_body, kCustomReminderResponseMaxBytes);
    cJSON *root = nullptr;
    cJSON *data = nullptr;
    CustomReminderDetail saved;
    response_message.clear();
    const bool parsed = response.transport_succeeded
        && response.status_code == 200
        && ParseSuccessData(response.body, &root, &data, response_message)
        && ParseCustomReminderDetail(data, saved)
        && saved.id == context.reminder_id;
    cJSON_Delete(root);
    if (!parsed)
    {
        ESP_LOGW(kTag, "%s自定义提醒失败，HTTP 状态码=%d", operation_text, response.status_code);
        FinishToolRequest(
            context.completion,
            response_message.empty()
                ? std::string(operation_text) + "自定义提醒失败，请稍后重试。"
                : response_message,
            true);
        return;
    }
    ESP_LOGI(kTag, "语音%s自定义提醒成功", operation_text);
    FinishToolRequest(
        context.completion,
        "自定义提醒已" + std::string(operation_text) + "，ID：" + saved.id
            + "，内容：" + TruncateUtf8(saved.content, kCustomReminderQueryItemMaxBytes) + "。",
        false);
}

/**
 * @brief 在屏保待办提醒缓存需要更新时把请求加入常驻后端 Worker。
 * @param force_refresh true 忽略最近成功时间；false 遵守 30 分钟本地新鲜周期。
 */
void BackendService::StartPendingReminderSync(bool force_refresh)
{
    if (notification_playback_active_.load())
    {
        if (force_refresh || pending_reminder_retry_due_.load()
            || pending_reminder_refresh_requested_.load())
        {
            pending_reminder_refresh_requested_.store(true);
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
    bool refresh_requested = false;
    {
        std::lock_guard<std::mutex> lock(pending_reminder_mutex_);
        if (pending_reminder_task_handle_ != nullptr)
        {
            return;
        }
        refresh_requested = pending_reminder_refresh_requested_.exchange(false);
        const bool retry_due = pending_reminder_retry_due_.load();
        if (!force_refresh && pending_reminder_retry_pending_.load() && !retry_due
            && !refresh_requested)
        {
            return;
        }
        force_refresh = force_refresh || refresh_requested || retry_due;
        const int64_t now_us = esp_timer_get_time();
        if (!force_refresh && pending_reminder_snapshot_.valid
            && pending_reminder_last_success_us_ > 0
            && now_us - pending_reminder_last_success_us_ < kPendingReminderRefreshIntervalUs)
        {
            return;
        }
        pending_reminder_task_handle_ = backend_worker_task_handle_;
        pending_reminder_retry_pending_.store(false);
        pending_reminder_retry_due_.store(false);
        if (pending_reminder_retry_timer_ != nullptr)
        {
            esp_timer_stop(pending_reminder_retry_timer_);
        }
    }

    if (!EnqueueBackendJob(BackendJobType::PendingReminderSync))
    {
        {
            std::lock_guard<std::mutex> lock(pending_reminder_mutex_);
            pending_reminder_task_handle_ = nullptr;
            if (refresh_requested)
            {
                pending_reminder_refresh_requested_.store(true);
            }
        }
        ESP_LOGE(kTag, "后端 Worker 队列已满，无法安排待办提醒同步");
        ShowPendingReminderStatusIfUnavailable("提醒任务启动失败");
        SchedulePendingReminderRetry();
    }
}

/**
 * @brief 获取当前用户未到期且当日到期的自定义提醒并更新运行内存缓存。
 * @details 复用自定义提醒列表接口，只保留 status==1（已启用）且 next_run_at（缺省时用
 * first_run_at）落在北京时间“今天”的提醒，最多渲染前 3 条。
 */
void BackendService::RunPendingReminderSync()
{
    if (notification_playback_active_.load())
    {
        pending_reminder_refresh_requested_.store(true);
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
        ShowPendingReminderStatusIfUnavailable(kUnboundPendingReminderPrompt);
        return;
    }

    const std::string path = std::string(kCustomRemindersPath)
        + "?page_index=1&page_size=" + std::to_string(kMaximumScreensaverPendingReminderCount);
    const HttpResponse response = SendJsonRequest(
        "GET", path.c_str(), access_token, "", kCustomReminderResponseMaxBytes);
    if (!IsCurrentDeviceCredential(access_token))
    {
        return;
    }
    cJSON *root = nullptr;
    cJSON *data = nullptr;
    std::string response_message;
    bool parsed = response.transport_succeeded
        && response.status_code == 200
        && ParseSuccessData(response.body, &root, &data, response_message);
    PendingReminderSnapshot snapshot;
    cJSON *records = nullptr;
    if (parsed)
    {
        records = cJSON_GetObjectItemCaseSensitive(data, "records");
        parsed = cJSON_IsArray(records)
            && cJSON_GetArraySize(records) <= static_cast<int>(kMaximumScreensaverPendingReminderCount);
    }
    if (parsed)
    {
        const int record_count = cJSON_GetArraySize(records);
        snapshot.contents.reserve(static_cast<size_t>(record_count));
        int visible_count = 0;
        for (int index = 0; index < record_count
             && visible_count < static_cast<int>(kMaximumScreensaverPendingReminderCount); ++index)
        {
            cJSON *record = cJSON_GetArrayItem(records, index);
            CustomReminderDetail detail;
            if (!ParseCustomReminderDetail(record, detail) || detail.status != 1)
            {
                continue;
            }

            // 判定"即将到来"：优先 next_run_at，为空时退回 first_run_at；
            // 只要不是已过期（今天或未来）就纳入屏保轮播。
            std::string run_at;
            if (!ReadBoundedString(record, "next_run_at", kCustomReminderTimeMaxBytes, run_at)
                && !ReadBoundedString(
                    record, "first_run_at", kCustomReminderTimeMaxBytes, run_at))
            {
                run_at = detail.first_run_at;
            }
            if (run_at.empty() || !IsReminderUpcomingBeijing(run_at))
            {
                continue;
            }

            std::string reminder_line;
            if (!FormatReminderTimeLine(run_at, reminder_line))
            {
                reminder_line = "提醒";
            }
            snapshot.contents.push_back(
                detail.content + "\n[" + reminder_line + "]");
            ++visible_count;
        }
    }
    cJSON_Delete(root);

    if (!parsed)
    {
        ESP_LOGW(kTag, "待办提醒同步失败，HTTP 状态码=%d", response.status_code);
        ShowPendingReminderStatusIfUnavailable(
            response.status_code == 401 || response.status_code == 403
                ? "设备认证已失效"
                : "提醒同步失败");
        SchedulePendingReminderRetry();
        return;
    }

    snapshot.valid = true;
    {
        std::lock_guard<std::mutex> binding_lock(binding_mutex_);
        if (device_access_token_ != access_token)
        {
            return;
        }
        std::lock_guard<std::mutex> reminder_lock(pending_reminder_mutex_);
        pending_reminder_snapshot_ = snapshot;
        pending_reminder_last_success_us_ = esp_timer_get_time();
    }
    ResetPendingReminderRetry();
    if (screensaver_active_.load())
    {
        ShowPendingReminderSnapshot(snapshot);
    }
    ESP_LOGI(kTag, "待办提醒同步成功，显示数量=%u",
             static_cast<unsigned>(snapshot.contents.size()));
}

/**
 * @brief 把有效待办提醒快照写入当前 LCD 表盘。
 * @param snapshot 已完成字段和数量校验的运行内存快照。
 */
void BackendService::ShowPendingReminderSnapshot(const PendingReminderSnapshot &snapshot)
{
    if (!screensaver_active_.load() || !snapshot.valid)
    {
        return;
    }
    Display *display = Board::GetInstance().GetDisplay();
    if (display != nullptr)
    {
        display->SetScreensaverPendingReminders(snapshot.contents);
    }
}

/**
 * @brief 没有历史待办提醒可保留时，向屏保待办区域写入固定状态文本。
 * @param message 加载、网络、认证或错误状态。
 */
void BackendService::ShowPendingReminderStatusIfUnavailable(const std::string &message)
{
    if (!screensaver_active_.load())
    {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(pending_reminder_mutex_);
        if (pending_reminder_snapshot_.valid)
        {
            return;
        }
    }
    Display *display = Board::GetInstance().GetDisplay();
    if (display != nullptr)
    {
        display->SetScreensaverPendingReminders({message});
    }
}

/**
 * @brief 在屏保显示期间使用最近缓存并按需同步天气和待办提醒。
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
    PendingReminderSnapshot reminder_snapshot;
    {
        std::lock_guard<std::mutex> lock(pending_reminder_mutex_);
        reminder_snapshot = pending_reminder_snapshot_;
    }
    if (reminder_snapshot.valid)
    {
        ShowPendingReminderSnapshot(reminder_snapshot);
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
            ShowPendingReminderStatusIfUnavailable(kUnboundPendingReminderPrompt);
        }
        else if (!network_connected_.load())
        {
            ShowPendingReminderStatusIfUnavailable("等待网络连接");
        }
        else
        {
            ShowPendingReminderStatusIfUnavailable("提醒加载中");
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
    StartPendingReminderSync(false);
}

/**
 * @brief esp_timer 周期回调，每 30 分钟为屏保待办提醒执行一次兜底同步检查。
 * @param context 指向 BackendService 单例。
 */
void BackendService::PendingReminderTimerCallback(void *context)
{
    auto *service = static_cast<BackendService *>(context);
    service->StartPendingReminderSync(false);
}

/**
 * @brief 待办提醒退避重试定时器回调，标记到期后尝试强制刷新一次。
 * @param context 指向 BackendService 单例。
 */
void BackendService::PendingReminderRetryTimerCallback(void *context)
{
    auto *service = static_cast<BackendService *>(context);
    service->pending_reminder_retry_due_.store(true);
    service->StartPendingReminderSync(true);
}

/**
 * @brief 按固定退避序列安排下一次待办提醒刷新，达到 30 分钟后保持该间隔。
 */
void BackendService::SchedulePendingReminderRetry()
{
    uint32_t delay_minutes = 0;
    esp_err_t timer_error = ESP_ERR_INVALID_STATE;
    {
        std::lock_guard<std::mutex> lock(pending_reminder_mutex_);
        if (pending_reminder_retry_timer_ == nullptr)
        {
            return;
        }
        const size_t interval_index = std::min(
            pending_reminder_retry_index_, kPendingReminderRetryIntervalsMinutes.size() - 1);
        delay_minutes = kPendingReminderRetryIntervalsMinutes[interval_index];
        if (pending_reminder_retry_index_ + 1 < kPendingReminderRetryIntervalsMinutes.size())
        {
            ++pending_reminder_retry_index_;
        }
        esp_timer_stop(pending_reminder_retry_timer_);
        pending_reminder_retry_pending_.store(true);
        pending_reminder_retry_due_.store(false);
        timer_error = esp_timer_start_once(
            pending_reminder_retry_timer_,
            static_cast<uint64_t>(delay_minutes) * 60ULL * 1000ULL * 1000ULL);
        if (timer_error != ESP_OK)
        {
            pending_reminder_retry_pending_.store(false);
        }
    }
    if (timer_error == ESP_OK)
    {
        ESP_LOGW(kTag, "待办提醒将在%u分钟后重试同步",
                 static_cast<unsigned>(delay_minutes));
    }
    else
    {
        ESP_LOGE(kTag, "待办提醒重试定时器启动失败，原因=%s",
                 esp_err_to_name(timer_error));
    }
}

/**
 * @brief 待办提醒同步成功后停止退避定时器，并让下一轮失败重新从 1 分钟开始。
 */
void BackendService::ResetPendingReminderRetry()
{
    std::lock_guard<std::mutex> lock(pending_reminder_mutex_);
    pending_reminder_retry_index_ = 0;
    pending_reminder_retry_pending_.store(false);
    pending_reminder_retry_due_.store(false);
    if (pending_reminder_retry_timer_ != nullptr)
    {
        esp_timer_stop(pending_reminder_retry_timer_);
    }
}
