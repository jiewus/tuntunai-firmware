/**
 * @file backend_dynamic_mcp.cc
 * @brief 囤囤AI动态 MCP 清单同步和工具执行实现。
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
 * @brief 在网络和设备凭据可用时启动单次动态 MCP 清单同步。
 */
void BackendService::StartMcpManifestSync()
{
    if (notification_playback_active_.load())
    {
        std::lock_guard<std::mutex> lock(dynamic_mcp_mutex_);
        mcp_manifest_refresh_requested_ = true;
        return;
    }
    if (!network_connected_.load())
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

    {
        std::lock_guard<std::mutex> lock(dynamic_mcp_mutex_);
        if (mcp_manifest_task_handle_ != nullptr)
        {
            return;
        }
        mcp_manifest_task_handle_ = backend_worker_task_handle_;
    }
    if (!EnqueueBackendJob(BackendJobType::McpManifest))
    {
        std::lock_guard<std::mutex> lock(dynamic_mcp_mutex_);
        mcp_manifest_task_handle_ = nullptr;
        ESP_LOGE(kTag, "后端 Worker 队列已满，无法安排 MCP 清单同步");
    }
}

/**
 * @brief 获取、校验并安装当前设备的动态 MCP 权威清单。
 */
void BackendService::RunMcpManifestSync()
{
    if (notification_playback_active_.load())
    {
        std::lock_guard<std::mutex> lock(dynamic_mcp_mutex_);
        mcp_manifest_refresh_requested_ = true;
        return;
    }
    if (!network_connected_.load())
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
        return;
    }

    const HttpResponse response = SendJsonRequest(
        "GET",
        kMcpManifestPath,
        access_token,
        "",
        kMcpManifestResponseMaxBytes);
    if (response.status_code == 401 || response.status_code == 403)
    {
        ESP_LOGW(kTag, "MCP 清单认证失败，HTTP 状态码=%d", response.status_code);
        ClearDynamicTools();
        return;
    }

    cJSON *root = nullptr;
    cJSON *data = nullptr;
    std::string response_message;
    const bool envelope_valid = response.transport_succeeded
        && response.status_code == 200
        && ParseSuccessData(response.body, &root, &data, response_message);
    if (!envelope_valid)
    {
        cJSON_Delete(root);
        ESP_LOGW(kTag, "MCP 清单同步失败，HTTP 状态码=%d", response.status_code);
        return;
    }

    uint32_t manifest_revision = 0;
    cJSON *revision = cJSON_GetObjectItemCaseSensitive(data, "revision");
    cJSON *tools = cJSON_GetObjectItemCaseSensitive(data, "tools");
    if (!ReadUnsignedInteger(revision, true, manifest_revision)
        || !cJSON_IsArray(tools)
        || cJSON_GetArraySize(tools) > static_cast<int>(kMaximumDynamicToolCount))
    {
        cJSON_Delete(root);
        ESP_LOGW(kTag, "MCP 清单修订号或工具数量无效");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(dynamic_mcp_mutex_);
        if (mcp_manifest_loaded_ && mcp_manifest_revision_ == manifest_revision)
        {
            cJSON_Delete(root);
            ESP_LOGD(kTag, "MCP 清单没有变化，修订号=%lu",
                     static_cast<unsigned long>(manifest_revision));
            return;
        }
    }

    std::vector<McpDynamicToolDefinition> definitions;
    definitions.reserve(static_cast<size_t>(cJSON_GetArraySize(tools)));
    std::vector<DynamicToolSummary> summaries;
    summaries.reserve(static_cast<size_t>(cJSON_GetArraySize(tools)));
    std::unordered_set<std::string> names;
    bool manifest_valid = true;
    cJSON *tool = nullptr;
    cJSON_ArrayForEach(tool, tools)
    {
        std::string tool_name;
        std::string display_name;
        std::string description;
        uint32_t tool_revision = 0;
        cJSON *revision_item = cJSON_GetObjectItemCaseSensitive(tool, "tool_revision");
        cJSON *parameters = cJSON_GetObjectItemCaseSensitive(tool, "parameters");
        cJSON *schema_version = cJSON_GetObjectItemCaseSensitive(tool, "result_schema_version");
        manifest_valid = cJSON_IsObject(tool)
            && ReadBoundedString(tool, "tool_name", kDynamicToolNameMaxBytes, tool_name)
            && IsValidDynamicToolName(tool_name)
            && ReadBoundedString(
                tool,
                "display_name",
                kDynamicToolDisplayNameMaxBytes,
                display_name)
            && ReadBoundedString(
                tool,
                "description",
                kDynamicToolDescriptionMaxBytes,
                description)
            && ReadUnsignedInteger(revision_item, false, tool_revision)
            && cJSON_IsArray(parameters)
            && cJSON_GetArraySize(parameters) == 0
            && cJSON_IsString(schema_version)
            && std::strcmp(schema_version->valuestring, kDynamicToolSchemaVersion) == 0
            && names.insert(tool_name).second;
        if (!manifest_valid)
        {
            break;
        }

        McpDynamicToolDefinition definition;
        definition.name = tool_name;
        definition.description = description;
        definition.properties = PropertyList();
        definition.input_schema_json = kDynamicToolInputSchema;
        definition.callback = [this, tool_name, tool_revision](
                                  const PropertyList &properties,
                                  McpToolCompletion completion) mutable
        {
            (void)properties;
            StartDynamicToolExecution(
                tool_name,
                tool_revision,
                [completion = std::move(completion)](
                    const std::string &message,
                    bool is_error) mutable
                {
                    completion(McpToolResult{message, is_error});
                });
        };
        definitions.push_back(std::move(definition));
        summaries.push_back(DynamicToolSummary{tool_name, display_name, description});
    }
    cJSON_Delete(root);

    if (!manifest_valid)
    {
        ESP_LOGW(kTag, "MCP 清单包含无效或重复的工具定义");
        return;
    }

    Application::GetInstance().Schedule(
        [this, manifest_revision, definitions = std::move(definitions),
         summaries = std::move(summaries)]() mutable
        {
            if (!McpServer::GetInstance().ReplaceDynamicTools(std::move(definitions)))
            {
                ESP_LOGE(kTag, "MCP 清单安装失败，修订号=%lu",
                         static_cast<unsigned long>(manifest_revision));
                return;
            }
            {
                std::lock_guard<std::mutex> lock(dynamic_mcp_mutex_);
                mcp_manifest_revision_ = manifest_revision;
                mcp_manifest_loaded_ = true;
                dynamic_tool_summaries_ = std::move(summaries);
            }
            ESP_LOGI(kTag, "MCP 清单同步成功，修订号=%lu",
                     static_cast<unsigned long>(manifest_revision));
        });
}

/**
 * @brief 创建代理执行动态 MCP 工具的单次后台任务。
 * @param tool_name 清单中的完整工具名称。
 * @param tool_revision 清单中的工具版本号。
 * @param completion 执行完成后回复原始 MCP 请求的一次性回调。
 */
void BackendService::StartDynamicToolExecution(
    const std::string &tool_name,
    uint32_t tool_revision,
    DynamicToolCompletion completion)
{
    if (!network_connected_.load())
    {
        completion("设备网络尚未连接，暂时无法执行该服务。", true);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(binding_mutex_);
        if (device_access_token_.empty())
        {
            completion("设备尚未绑定囤囤AI，无法执行该服务。", true);
            return;
        }
    }

    const char *rejected_message = nullptr;
    DynamicToolCompletion rejected_completion = completion;
    {
        std::lock_guard<std::mutex> lock(dynamic_mcp_mutex_);
        if (dynamic_tool_task_handle_ != nullptr)
        {
            rejected_message = "设备正在执行另一个自定义服务，请稍后再试。";
        }
        else
        {
            dynamic_tool_task_handle_ = backend_worker_task_handle_;
        }
    }
    if (rejected_message != nullptr)
    {
        rejected_completion(rejected_message, true);
        return;
    }

    auto *task_context = new (std::nothrow) DynamicToolTaskContext{
        tool_name,
        tool_revision,
        std::move(completion)};
    if (task_context == nullptr)
    {
        std::lock_guard<std::mutex> lock(dynamic_mcp_mutex_);
        dynamic_tool_task_handle_ = nullptr;
        rejected_completion("设备内存不足，无法执行该服务。", true);
        return;
    }
    bool queue_failed = false;
    if (!EnqueueBackendJob(BackendJobType::DynamicTool, task_context))
    {
        queue_failed = true;
        delete task_context;
        std::lock_guard<std::mutex> lock(dynamic_mcp_mutex_);
        dynamic_tool_task_handle_ = nullptr;
    }
    if (queue_failed)
    {
        rejected_completion("设备后台任务队列已满，暂时无法执行该服务。", true);
    }
}

/**
 * @brief 调用平台执行接口并校验 ServiceExecutionResult v1。
 * @param tool_name 本次请求的完整工具名称。
 * @param tool_revision 本次请求固定的工具版本号。
 * @param completion 执行完成后回复原始 MCP 请求的一次性回调。
 */
void BackendService::RunDynamicToolExecution(
    const std::string &tool_name,
    uint32_t tool_revision,
    DynamicToolCompletion &completion)
{
    if (!network_connected_.load())
    {
        FinishToolRequest(completion, "设备网络尚未连接，暂时无法执行该服务。", true);
        return;
    }
    std::string access_token;
    {
        std::lock_guard<std::mutex> lock(binding_mutex_);
        access_token = device_access_token_;
    }
    if (access_token.empty())
    {
        FinishToolRequest(completion, "设备认证信息不存在，请重新绑定设备。", true);
        return;
    }

    const std::string request_json = BuildDynamicToolRequestJson(tool_name, tool_revision);
    if (request_json.empty())
    {
        FinishToolRequest(completion, "设备内存不足，无法生成服务请求。", true);
        return;
    }

    const HttpResponse response = SendJsonRequest(
        "POST",
        kMcpExecutePath,
        access_token,
        request_json);
    if (response.status_code == 409)
    {
        std::lock_guard<std::mutex> lock(dynamic_mcp_mutex_);
        mcp_manifest_refresh_requested_ = true;
    }

    cJSON *root = nullptr;
    cJSON *data = nullptr;
    std::string response_message;
    const bool envelope_valid = response.transport_succeeded
        && response.status_code == 200
        && ParseSuccessData(response.body, &root, &data, response_message);
    if (!envelope_valid)
    {
        cJSON_Delete(root);
        ESP_LOGW(kTag, "动态 MCP 工具执行请求失败，HTTP 状态码=%d",
                 response.status_code);
        const bool authentication_failed = response.status_code == 401
            || response.status_code == 403;
        if (authentication_failed)
        {
            ClearDynamicTools();
        }
        const std::string message = authentication_failed
            ? "设备认证已失效，请重新绑定设备。"
            : (response_message.empty()
                   || response_message.size() > kDynamicToolDescriptionMaxBytes
                   ? kDynamicToolFallbackError
                   : response_message);
        FinishToolRequest(completion, message, true);
        return;
    }

    cJSON *schema_version = cJSON_GetObjectItemCaseSensitive(data, "schema_version");
    cJSON *result_tool_name = cJSON_GetObjectItemCaseSensitive(data, "tool_name");
    cJSON *status = cJSON_GetObjectItemCaseSensitive(data, "status");
    cJSON *content = cJSON_GetObjectItemCaseSensitive(data, "content");
    uint32_t result_status = 0;
    const size_t content_length = cJSON_IsString(content) && content->valuestring != nullptr
        ? std::strlen(content->valuestring)
        : 0;
    const bool result_valid = cJSON_IsString(schema_version)
        && std::strcmp(schema_version->valuestring, kDynamicToolSchemaVersion) == 0
        && cJSON_IsString(result_tool_name)
        && tool_name == result_tool_name->valuestring
        && ReadUnsignedInteger(status, false, result_status)
        && (result_status == 1 || result_status == 2)
        && cJSON_IsString(content)
        && content_length <= kDynamicToolResultTextMaxBytes
        && (result_status == 2 || content_length > 0);
    if (!result_valid)
    {
        cJSON_Delete(root);
        ESP_LOGW(kTag, "动态 MCP 工具返回了无效结果");
        FinishToolRequest(completion, "平台返回的服务结果格式无效。", true);
        return;
    }

    const bool execution_failed = result_status == 2;
    const std::string result_text = content_length == 0
        ? std::string(kDynamicToolFallbackError)
        : std::string(content->valuestring, content_length);
    cJSON_Delete(root);
    FinishToolRequest(completion, result_text, execution_failed);
}

/**
 * @brief 在 Application 主任务中清空后端动态工具和清单修订状态。
 */
void BackendService::ClearDynamicTools()
{
    Application::GetInstance().Schedule([this]()
    {
        {
            std::lock_guard<std::mutex> lock(dynamic_mcp_mutex_);
            if (!mcp_manifest_loaded_)
            {
                return;
            }
        }
        if (!McpServer::GetInstance().ReplaceDynamicTools({}))
        {
            return;
        }
        std::lock_guard<std::mutex> lock(dynamic_mcp_mutex_);
        mcp_manifest_revision_ = 0;
        mcp_manifest_loaded_ = false;
        dynamic_tool_summaries_.clear();
        ESP_LOGI(kTag, "认证失败后已清空动态 MCP 工具");
    });
}
