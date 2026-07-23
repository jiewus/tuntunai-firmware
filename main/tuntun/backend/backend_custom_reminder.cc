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
    if (requires_id && !IsSafeMemoId(context->reminder_id))
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
