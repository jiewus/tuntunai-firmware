/**
 * @file backend_service.cc
 * @brief 囤囤管家设备绑定、屏保天气、动态 MCP 和未接入业务的占位实现。
 */

#include "backend_service.h"

#include <cJSON.h>
#include <esp_app_desc.h>
#include <esp_log.h>
#include <http.h>

#include <array>
#include <cmath>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <unordered_set>
#include <utility>

#include "application.h"
#include "boards/common/board.h"
#include "display/display.h"
#include "mcp_server.h"
#include "settings.h"
#include "system_info.h"

namespace
{

    /**
     * @brief 业务后端模块使用的日志标签。
     */
    constexpr const char *kTag = "TuntunBackend";

    /**
     * @brief 保存绑定会话和最终设备凭据的 NVS 命名空间。
     * @details 名称及其全部键名均不超过 ESP-IDF NVS 的 15 字符限制。
     */
    constexpr const char *kSettingsNamespace = "tuntun_api";
    constexpr const char *kBindingCodeKey = "bind_code";
    constexpr const char *kBindingTokenKey = "bind_token";
    constexpr const char *kDeviceIdKey = "device_id";
    constexpr const char *kDeviceTokenKey = "device_token";

    /**
     * @brief 后端设备绑定接口相对于 CONFIG_TUNTUN_API_URL 的固定路径。
     */
    constexpr const char *kBindingRequestPath = "/api/device/binding/request";
    constexpr const char *kBindingStatusPath = "/api/device/binding/status";
    constexpr const char *kBindingCompletePath = "/api/device/binding/complete";

    /**
     * @brief 使用设备访问 Token 获取当前屏保天气的固定接口路径。
     */
    constexpr const char *kWeatherPath = "/api/device/weather";
    constexpr const char *kWeatherSettingsPath = "/api/device/weather/settings";

    /**
     * @brief 使用设备访问 Token 同步和执行动态 MCP 工具的固定接口路径。
     */
    constexpr const char *kMcpManifestPath = "/api/mcp-tools/manifest";
    constexpr const char *kMcpExecutePath = "/api/mcp-tools/execute";

    /**
     * @brief 设备型号在后端 DeviceModelEnum 中对应 Movecall Moji2 ESP32-C5 的数值。
     */
    constexpr int kDeviceModelMovecallMoji2Esp32C5 = 1;

    /**
     * @brief 绑定状态轮询周期和单次 HTTP 超时，单位均为毫秒。
     */
    constexpr int kBindingPollIntervalMs = 3000;
    constexpr int kBindingRetryIntervalMs = 5000;
    constexpr int kHttpTimeoutMs = 10000;

    /**
     * @brief 天气缓存新鲜周期、后台检查周期和临时 Worker 资源配置。
     * @details 固件每 5 分钟只检查一次缓存年龄，成功天气在 30 分钟内不会重复请求。天气 Worker
     *          仅在真正需要 HTTPS 同步时存在，完成后立即释放 8KB 任务栈。
     */
    constexpr int64_t kWeatherRefreshIntervalUs = 30LL * 60LL * 1000LL * 1000LL;
    constexpr int64_t kWeatherCheckIntervalUs = 5LL * 60LL * 1000LL * 1000LL;
    constexpr uint32_t kWeatherTaskStackSize = 8192;
    constexpr UBaseType_t kWeatherTaskPriority = 3;

    /**
     * @brief 动态 MCP 清单检查周期和临时 Worker 的资源配置。
     * @details 清单同步与工具执行都涉及 HTTPS 和 cJSON，因此使用独立 8KB 任务栈。第一版同时只
     *          运行一个清单同步任务和一个动态工具执行任务，避免并发请求挤压设备可用内存。
     */
    constexpr int64_t kMcpManifestCheckIntervalUs = 30LL * 1000LL * 1000LL;
    constexpr uint32_t kMcpTaskStackSize = 8192;
    constexpr UBaseType_t kMcpTaskPriority = 3;

    /**
     * @brief 天气响应和语音位置输入的固件边界，防止异常文本和温度进入任务或圆屏布局。
     * @details 圆屏位置名称限制为 96 字节；语音输入按后端 100 个 Unicode 字符的上限预留
     *          最多 300 个 UTF-8 字节。
     */
    constexpr size_t kWeatherLocationMaxBytes = 96;
    constexpr size_t kWeatherLocationInputMaxBytes = 300;
    constexpr size_t kWeatherDescriptionMaxBytes = 48;
    constexpr int kWeatherMinimumTemperature = -100;
    constexpr int kWeatherMaximumTemperature = 100;

    /**
     * @brief 单次后端 JSON 响应允许占用的最大字节数和分段读取缓冲区大小。
     * @details 绑定接口响应均远小于 4KB。设置硬上限可以避免异常网关或错误页面返回大正文时
     *          持续占用 ESP32-C5 堆内存；超过上限的响应会被当作传输失败处理。
     */
    constexpr size_t kMaxApiResponseBytes = 4096;
    constexpr size_t kMcpManifestResponseMaxBytes = 12288;
    constexpr size_t kHttpReadChunkBytes = 512;

    /**
     * @brief 第一版动态 MCP 清单和统一执行结果的固件安全边界。
     */
    constexpr size_t kMaximumDynamicToolCount = 10;
    constexpr size_t kDynamicToolNameMaxBytes = 64;
    constexpr size_t kDynamicToolDescriptionMaxBytes = 512;
    constexpr size_t kDynamicToolResultTextMaxBytes = 2048;
    constexpr const char *kDynamicToolSchemaVersion = "1.0";
    constexpr const char *kDynamicToolInputSchema =
        "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}";
    constexpr const char *kDynamicToolFallbackError = "服务执行失败，请稍后重试。";

    /**
     * @brief 后端 DeviceBindingStatusEnum 的稳定数值。
     */
    constexpr int kBindingStatusPending = 1;
    constexpr int kBindingStatusBound = 2;
    constexpr int kBindingStatusCompleted = 3;
    constexpr int kBindingStatusExpired = 4;
    constexpr int kBindingStatusFailed = 5;

    /**
     * @brief 屏保天气位置区域显示的临时说明。
     */
    constexpr const char *kWeatherTemperaturePlaceholder = "--℃";
    constexpr const char *kWeatherDescriptionPlaceholder = "--";
    constexpr const char *kWeatherRangePlaceholder = "--/--";
    constexpr const char *kMemoPlaceholder = "备忘录服务开发中";

    /**
     * @brief 保存一次 HTTP 请求的传输结果。
     */
    struct HttpResponse
    {
        /**
         * @brief true 表示请求已经收到状态码，并在调用方指定限制内完整读取响应正文。
         */
        bool transport_succeeded = false;
        /**
         * @brief 服务端返回的 HTTP 状态码；0 表示尚未取得有效状态码。
         */
        int status_code = 0;
        /**
         * @brief 在固定上限内读取的 UTF-8 JSON 响应正文。
         */
        std::string body;
    };

    /**
     * @brief 去除 API 根地址末尾的斜杠并拼接固定接口路径。
     * @param path 以斜杠开头的接口路径。
     * @return 可直接交给 HTTP 客户端的完整 HTTPS 地址。
     */
    std::string BuildApiUrl(const char *path)
    {
        std::string base_url = CONFIG_TUNTUN_API_URL;
        while (!base_url.empty() && base_url.back() == '/')
        {
            base_url.pop_back();
        }
        return base_url + path;
    }

    /**
     * @brief 执行一条有界 JSON HTTP 请求。
     * @param method HTTP 方法。
     * @param path 业务接口路径。
     * @param bearer_token 可选的 Bearer Token；空字符串表示不发送 Authorization。
     * @param request_body JSON 请求正文；GET 请求传空字符串。
     * @param maximum_response_bytes 本次响应正文允许占用的最大字节数。
     * @return 传输状态、HTTP 状态码和响应正文。
     * @details 本方法不记录请求或响应正文，避免绑定会话 Token 和设备 Token 进入日志。
     */
    HttpResponse SendJsonRequest(const char *method, const char *path,
                                 const std::string &bearer_token,
                                 std::string request_body,
                                 size_t maximum_response_bytes = kMaxApiResponseBytes)
    {
        HttpResponse response;
        auto *network = Board::GetInstance().GetNetwork();
        if (network == nullptr)
        {
            return response;
        }

        auto http = network->CreateHttp();
        if (http == nullptr)
        {
            return response;
        }
        http->SetTimeout(kHttpTimeoutMs);
        http->SetHeader("Accept", "application/json");
        http->SetHeader("Content-Type", "application/json");
        http->SetHeader("User-Agent", SystemInfo::GetUserAgent());
        if (!bearer_token.empty())
        {
            http->SetHeader("Authorization", "Bearer " + bearer_token);
        }
        if (!request_body.empty())
        {
            http->SetContent(std::move(request_body));
        }

        if (!http->Open(method, BuildApiUrl(path)))
        {
            ESP_LOGW(kTag, "后端 HTTP 传输失败，路径=%s，错误码=0x%x",
                     path, http->GetLastError());
            return response;
        }

        response.status_code = http->GetStatusCode();
        if (response.status_code <= 0)
        {
            http->Close();
            return response;
        }
        const size_t declared_body_length = http->GetBodyLength();
        if (declared_body_length > maximum_response_bytes)
        {
            ESP_LOGW(kTag, "后端 HTTP 响应过大，路径=%s，字节数=%u",
                     path, static_cast<unsigned>(declared_body_length));
            http->Close();
            return response;
        }

        std::array<char, kHttpReadChunkBytes> read_buffer{};
        response.body.reserve(declared_body_length);
        while (true)
        {
            const int read_size = http->Read(read_buffer.data(), read_buffer.size());
            if (read_size < 0)
            {
                ESP_LOGW(kTag, "后端 HTTP 响应读取失败，路径=%s", path);
                response.body.clear();
                http->Close();
                return response;
            }
            if (read_size == 0)
            {
                break;
            }
            if (response.body.size() + static_cast<size_t>(read_size) > maximum_response_bytes)
            {
                ESP_LOGW(kTag, "后端 HTTP 响应超过限制，限制字节数=%u，路径=%s",
                         static_cast<unsigned>(maximum_response_bytes), path);
                response.body.clear();
                http->Close();
                return response;
            }
            response.body.append(read_buffer.data(), static_cast<size_t>(read_size));
        }
        http->Close();
        response.transport_succeeded = response.status_code > 0;
        return response;
    }

    /**
     * @brief 解析统一 ApiResponse 并取得 data 对象。
     * @param body 完整响应正文。
     * @param root 输出需要由调用方 cJSON_Delete 的根对象。
     * @param data 输出根对象内部的 data 对象，生命周期从属于 root。
     * @param message 输出服务端可安全展示的响应说明。
     * @return JSON 有效、业务 code 为0且 data 为对象时返回 true。
     */
    bool ParseSuccessData(const std::string &body, cJSON **root, cJSON **data,
                          std::string &message)
    {
        *root = cJSON_Parse(body.c_str());
        if (*root == nullptr)
        {
            message = "平台响应格式无效";
            return false;
        }

        cJSON *message_item = cJSON_GetObjectItemCaseSensitive(*root, "message");
        if (cJSON_IsString(message_item))
        {
            message = message_item->valuestring;
        }
        cJSON *code = cJSON_GetObjectItemCaseSensitive(*root, "code");
        *data = cJSON_GetObjectItemCaseSensitive(*root, "data");
        return cJSON_IsNumber(code) && code->valueint == 0 && cJSON_IsObject(*data);
    }

    /**
     * @brief 从 JSON 对象读取一个非空且长度受限的 UTF-8 字符串。
     * @param object 包含目标字段的 JSON 对象。
     * @param name 需要读取的精确字段名。
     * @param maximum_bytes 包含内容允许占用的最大字节数，不包含结尾空字符。
     * @param value 成功时接收字段副本。
     * @return 字段存在、类型正确、非空且没有超过字节上限时返回 true。
     */
    bool ReadBoundedString(cJSON *object, const char *name, size_t maximum_bytes,
                           std::string &value)
    {
        cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
        if (!cJSON_IsString(item) || item->valuestring == nullptr)
        {
            return false;
        }
        const size_t length = std::strlen(item->valuestring);
        if (length == 0 || length > maximum_bytes)
        {
            return false;
        }
        value.assign(item->valuestring, length);
        return true;
    }

    /**
     * @brief 从 JSON 对象读取一个处于固件安全范围内的摄氏温度整数。
     * @param object 包含目标字段的 JSON 对象。
     * @param name 需要读取的精确字段名。
     * @param value 成功时接收温度值。
     * @return 字段为数值且落在 -100℃ 至 100℃ 时返回 true。
     */
    bool ReadTemperature(cJSON *object, const char *name, int &value)
    {
        cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
        if (!cJSON_IsNumber(item) || item->valuedouble < kWeatherMinimumTemperature || item->valuedouble > kWeatherMaximumTemperature)
        {
            return false;
        }
        value = item->valueint;
        return true;
    }

    /**
     * @brief 构建设备申请绑定码所需的固定强类型 JSON。
     * @return 包含硬件标识、设备型号、客户端标识和固件版本的紧凑 JSON。
     */
    std::string BuildBindingRequestJson()
    {
        auto &board = Board::GetInstance();
        const esp_app_desc_t *app_description = esp_app_get_description();
        cJSON *root = cJSON_CreateObject();
        if (root == nullptr)
        {
            return {};
        }
        cJSON_AddStringToObject(root, "hardware_id", SystemInfo::GetMacAddress().c_str());
        cJSON_AddNumberToObject(root, "device_model", kDeviceModelMovecallMoji2Esp32C5);
        cJSON_AddStringToObject(root, "client_id", board.GetUuid().c_str());
        cJSON_AddStringToObject(root, "firmware_version", app_description->version);
        char *json_text = cJSON_PrintUnformatted(root);
        std::string result = json_text == nullptr ? std::string() : std::string(json_text);
        cJSON_free(json_text);
        cJSON_Delete(root);
        return result;
    }

    /**
     * @brief 构建固定城市或 IP 自动定位模式的天气设置请求 JSON。
     * @param location_name 固定模式下用户提供的城市或区县名称；IP 模式下为空。
     * @param use_ip_auto true 使用公网 IP 自动定位；false 使用固定城市。
     * @return 包含位置模式和位置名称的紧凑 JSON；内存不足时返回空字符串。
     */
    std::string BuildWeatherLocationRequestJson(
        const std::string &location_name,
        bool use_ip_auto)
    {
        cJSON *root = cJSON_CreateObject();
        if (root == nullptr)
        {
            return {};
        }
        cJSON_AddNumberToObject(root, "location_mode", use_ip_auto ? 2 : 1);
        if (use_ip_auto)
        {
            cJSON_AddNullToObject(root, "location_name");
        }
        else
        {
            cJSON_AddStringToObject(root, "location_name", location_name.c_str());
        }
        char *json_text = cJSON_PrintUnformatted(root);
        std::string result = json_text == nullptr ? std::string() : std::string(json_text);
        cJSON_free(json_text);
        cJSON_Delete(root);
        return result;
    }

    /**
     * @brief 检查 cJSON 数值是否能无损转换为 uint32_t。
     * @param item 需要检查的 JSON 节点。
     * @param allow_zero true 允许0；false 要求至少为1。
     * @param value 校验成功时接收转换后的整数。
     * @return 节点为有限非负整数且没有超过 uint32_t 上限时返回 true。
     */
    bool ReadUnsignedInteger(cJSON *item, bool allow_zero, uint32_t &value)
    {
        if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble)
            || std::floor(item->valuedouble) != item->valuedouble
            || item->valuedouble < (allow_zero ? 0.0 : 1.0)
            || item->valuedouble > static_cast<double>(std::numeric_limits<uint32_t>::max()))
        {
            return false;
        }
        value = static_cast<uint32_t>(item->valuedouble);
        return true;
    }

    /**
     * @brief 校验动态工具名称符合 custom. 前缀和固件允许的 ASCII 字符集。
     * @param name 后端清单中的完整工具名称。
     * @return 名称长度为1至64字节、以 custom. 开头且只含小写字母、数字及分隔符时返回 true。
     */
    bool IsValidDynamicToolName(const std::string &name)
    {
        constexpr const char *prefix = "custom.";
        if (name.size() <= std::strlen(prefix) || name.size() > kDynamicToolNameMaxBytes
            || name.compare(0, std::strlen(prefix), prefix) != 0)
        {
            return false;
        }
        for (const char character : name)
        {
            const bool valid = (character >= 'a' && character <= 'z')
                || (character >= '0' && character <= '9')
                || character == '_' || character == '-' || character == '.';
            if (!valid)
            {
                return false;
            }
        }
        return true;
    }

    /**
     * @brief 构建第一版动态 MCP 工具执行请求。
     * @param tool_name 清单中的完整工具名称。
     * @param tool_revision 清单中的固定工具版本号。
     * @return 包含空 arguments 数组的紧凑 JSON；内存不足时返回空字符串。
     */
    std::string BuildDynamicToolRequestJson(
        const std::string &tool_name,
        uint32_t tool_revision)
    {
        cJSON *root = cJSON_CreateObject();
        if (root == nullptr)
        {
            return {};
        }
        cJSON_AddStringToObject(root, "tool_name", tool_name.c_str());
        cJSON_AddNumberToObject(root, "tool_revision", tool_revision);
        cJSON_AddItemToObject(root, "arguments", cJSON_CreateArray());
        char *json_text = cJSON_PrintUnformatted(root);
        std::string result = json_text == nullptr ? std::string() : std::string(json_text);
        cJSON_free(json_text);
        cJSON_Delete(root);
        return result;
    }

    /**
     * @brief 在圆屏上显示绑定流程状态，并确保用户可见背光已经恢复。
     * @param binding_code 非空时显示大号数字绑定码；空字符串表示只显示状态。
     * @param message 显示在绑定码下方的中文操作说明或结果。
     */
    void ShowBindingPage(const std::string &binding_code, const std::string &message)
    {
        auto &board = Board::GetInstance();
        board.WakeUpScreen(true);
        Display *display = board.GetDisplay();
        if (display != nullptr)
        {
            display->ShowDeviceBinding(binding_code, message);
        }
    }

    /**
     * @brief 隐藏绑定页面并恢复页面下方的正常界面。
     */
    void HideBindingPage()
    {
        Display *display = Board::GetInstance().GetDisplay();
        if (display != nullptr)
        {
            display->HideDeviceBinding();
        }
    }

    /**
     * @brief 至多调用一次异步 MCP 结果回调，并在调用前清空原回调。
     * @param completion 设备绑定或天气城市设置需要消费的一次性回调；为空时不执行操作。
     * @param message 返回给大模型的中文结果文本。
     * @param is_error true 表示对应的设备绑定或天气城市操作失败。
     * @details 先转移并清空回调可防止回调自身抛出异常时，任务异常边界再次回复同一条
     *          JSON-RPC 请求。
     */
    void FinishToolRequest(
        std::function<void(const std::string &message, bool is_error)> &completion,
        const std::string &message, bool is_error)
    {
        if (!completion)
        {
            return;
        }
        auto callback = std::move(completion);
        completion = {};
        callback(message, is_error);
    }

} // namespace

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
 * @brief 从 NVS 恢复后端绑定状态。
 */
void BackendService::Start()
{
    if (started_)
    {
        return;
    }
    started_ = true;
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

    const esp_timer_create_args_t mcp_manifest_timer_args = {
        .callback = McpManifestTimerCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "mcp_manifest",
        .skip_unhandled_events = true,
    };
    timer_error = esp_timer_create(&mcp_manifest_timer_args, &mcp_manifest_timer_);
    if (timer_error == ESP_OK)
    {
        timer_error = esp_timer_start_periodic(
            mcp_manifest_timer_,
            kMcpManifestCheckIntervalUs);
    }
    if (timer_error != ESP_OK)
    {
        ESP_LOGE(kTag, "MCP 清单定时器启动失败，原因=%s",
                 esp_err_to_name(timer_error));
        if (mcp_manifest_timer_ != nullptr)
        {
            esp_timer_delete(mcp_manifest_timer_);
            mcp_manifest_timer_ = nullptr;
        }
    }
    ESP_LOGI(kTag, "囤囤管家后端服务已初始化，接口地址=%s，设备凭据=%s",
             CONFIG_TUNTUN_API_URL, device_access_token_.empty() ? "缺失" : "可用");
}

/**
 * @brief 注册设备绑定状态处理、解绑说明和天气设置 MCP 工具。
 * @param server 设备 MCP 服务器。
 */
void BackendService::RegisterMcpTools(McpServer &server)
{
    // 绑定工具在设备 MCP 服务器上注册为异步工具，确保在网络请求完成前不会阻塞语音会话。
    server.AddAsyncTool(
        "self.tuntun.bind_device",
        "Check whether this device is already bound. If it is bound, report that state directly. "
        "Otherwise generate a TuntunLife platform device binding code and show it on the screen. "
        "Call this tool only when the user explicitly asks to bind or connect this device to the "
        "TuntunLife platform. The user must finish binding in the TuntunLife web application.",
        PropertyList(),
        [this](const PropertyList &properties, McpToolCompletion completion)
        {
            (void)properties;
            StartBindingTask(true,
                             [completion = std::move(completion)](const std::string &message,
                                                                  bool is_error) mutable
                             {
                                 completion(McpToolResult{message, is_error});
                             });
        });

    // 解绑工具在设备 MCP 服务器上注册为同步工具，确保在语音会话中立即返回说明文本。
    server.AddTool(
        "self.tuntun.unbind_device",
        "Explain how to unbind this device from the TuntunLife platform. Call this tool when the "
        "user explicitly asks to unbind or remove this device. Device-side voice unbinding is not "
        "allowed; the user must sign in to the TuntunLife web application and unbind it there.",
        PropertyList(),
        [](const PropertyList &properties) -> ReturnValue
        {
            (void)properties;
            return std::string(
                "为保证设备归属安全，设备端不支持语音解绑。请登录囤囤管家后台，在设备管理中完成解绑。");
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
}

/**
 * @brief 标记网络可用，并恢复未完成绑定会话的轮询。
 */
void BackendService::OnNetworkConnected()
{
    network_connected_.store(true);
    bool has_pending_session = false;
    {
        std::lock_guard<std::mutex> lock(binding_mutex_);
        has_pending_session = !binding_session_token_.empty() && !binding_code_.empty();
    }
    if (has_pending_session)
    {
        StartBindingTask(false);
    }
    if (screensaver_active_.load())
    {
        StartWeatherSync(false);
    }
    StartMcpManifestSync();
}

/**
 * @brief 标记网络不可用，活动绑定任务会暂停请求并等待恢复。
 */
void BackendService::OnNetworkDisconnected()
{
    network_connected_.store(false);
    ShowWeatherStatusIfUnavailable("等待网络连接");
}

/**
 * @brief 语音 MCP 会话结束不取消设备级绑定流程。
 */
void BackendService::OnMcpDisconnected()
{
}

/**
 * @brief 在网络和设备凭据可用时启动单次动态 MCP 清单同步。
 */
void BackendService::StartMcpManifestSync()
{
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

    std::lock_guard<std::mutex> lock(dynamic_mcp_mutex_);
    if (mcp_manifest_task_handle_ != nullptr)
    {
        return;
    }
    const BaseType_t created = xTaskCreate(
        McpManifestTaskEntry,
        "mcp_manifest",
        kMcpTaskStackSize,
        this,
        kMcpTaskPriority,
        &mcp_manifest_task_handle_);
    if (created != pdPASS)
    {
        mcp_manifest_task_handle_ = nullptr;
        ESP_LOGE(kTag, "内存不足，无法启动 MCP 清单同步任务");
    }
}

/**
 * @brief FreeRTOS 动态 MCP 清单同步任务入口，确保任务标志始终释放。
 * @param context 指向 BackendService 单例。
 */
void BackendService::McpManifestTaskEntry(void *context)
{
    auto *service = static_cast<BackendService *>(context);
    try
    {
        service->RunMcpManifestSync();
    }
    catch (const std::exception &exception)
    {
        ESP_LOGE(kTag, "MCP 清单同步任务异常，原因=%s", exception.what());
    }
    catch (...)
    {
        ESP_LOGE(kTag, "MCP 清单同步任务发生未知异常");
    }
    {
        std::lock_guard<std::mutex> lock(service->dynamic_mcp_mutex_);
        service->mcp_manifest_task_handle_ = nullptr;
    }
    vTaskDelete(nullptr);
}

/**
 * @brief esp_timer 周期回调，创建动态 MCP 清单同步任务。
 * @param context 指向 BackendService 单例。
 */
void BackendService::McpManifestTimerCallback(void *context)
{
    auto *service = static_cast<BackendService *>(context);
    service->StartMcpManifestSync();
}

/**
 * @brief 获取、校验并安装当前设备的动态 MCP 权威清单。
 */
void BackendService::RunMcpManifestSync()
{
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
    std::unordered_set<std::string> names;
    bool manifest_valid = true;
    cJSON *tool = nullptr;
    cJSON_ArrayForEach(tool, tools)
    {
        std::string tool_name;
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
    }
    cJSON_Delete(root);

    if (!manifest_valid)
    {
        ESP_LOGW(kTag, "MCP 清单包含无效或重复的工具定义");
        return;
    }

    Application::GetInstance().Schedule(
        [this, manifest_revision, definitions = std::move(definitions)]() mutable
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
            completion("设备尚未绑定囤囤管家，无法执行该服务。", true);
            return;
        }
    }

    DynamicToolCompletion rejected_completion;
    const char *rejected_message = nullptr;
    {
        std::lock_guard<std::mutex> lock(dynamic_mcp_mutex_);
        if (dynamic_tool_task_handle_ != nullptr)
        {
            rejected_completion = std::move(completion);
            rejected_message = "设备正在执行另一个自定义服务，请稍后再试。";
        }
        else
        {
            auto *context = new (std::nothrow) DynamicToolTaskContext{
                this,
                tool_name,
                tool_revision,
                std::move(completion)};
            if (context == nullptr)
            {
                rejected_completion = std::move(completion);
                rejected_message = "设备内存不足，无法执行该服务。";
            }
            else
            {
                const BaseType_t created = xTaskCreate(
                    DynamicToolTaskEntry,
                    "mcp_execute",
                    kMcpTaskStackSize,
                    context,
                    kMcpTaskPriority,
                    &dynamic_tool_task_handle_);
                if (created == pdPASS)
                {
                    return;
                }
                rejected_completion = std::move(context->completion);
                delete context;
                dynamic_tool_task_handle_ = nullptr;
                rejected_message = "设备内存不足，无法执行该服务。";
            }
        }
    }
    if (rejected_completion)
    {
        rejected_completion(rejected_message, true);
    }
}

/**
 * @brief FreeRTOS 动态工具执行入口，确保回调和任务标志始终得到处理。
 * @param context DynamicToolTaskContext 指针。
 */
void BackendService::DynamicToolTaskEntry(void *context)
{
    std::unique_ptr<DynamicToolTaskContext> task_context(
        static_cast<DynamicToolTaskContext *>(context));
    BackendService *service = task_context->service;
    try
    {
        service->RunDynamicToolExecution(
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
        std::lock_guard<std::mutex> lock(service->dynamic_mcp_mutex_);
        service->dynamic_tool_task_handle_ = nullptr;
        refresh_manifest = service->mcp_manifest_refresh_requested_;
        service->mcp_manifest_refresh_requested_ = false;
    }
    if (refresh_manifest)
    {
        service->StartMcpManifestSync();
    }
    vTaskDelete(nullptr);
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
        ESP_LOGI(kTag, "认证失败后已清空动态 MCP 工具");
    });
}

/**
 * @brief 在屏保显示期间使用最近缓存并按需同步天气，同时保留备忘录占位内容。
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
    display->SetScreensaverMemos({kMemoPlaceholder});

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
            ShowWeatherStatusIfUnavailable("请先绑定囤囤管家");
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
}

/**
 * @brief 在天气缓存确实需要更新时创建一个临时 FreeRTOS Worker。
 * @param force_refresh true 忽略最近成功时间；false 遵守 30 分钟本地新鲜周期。
 */
void BackendService::StartWeatherSync(bool force_refresh)
{
    if (!screensaver_active_.load() || !network_connected_.load())
    {
        return;
    }

    bool has_device_credential = false;
    {
        std::lock_guard<std::mutex> lock(binding_mutex_);
        has_device_credential = !device_access_token_.empty();
    }
    if (!has_device_credential)
    {
        return;
    }

    bool task_start_failed = false;
    {
        std::lock_guard<std::mutex> lock(weather_mutex_);
        if (weather_task_handle_ != nullptr)
        {
            return;
        }
        const int64_t now_us = esp_timer_get_time();
        if (!force_refresh && weather_snapshot_.valid && weather_last_success_us_ > 0 && now_us - weather_last_success_us_ < kWeatherRefreshIntervalUs)
        {
            return;
        }

        weather_task_handle_ = nullptr;
        const BaseType_t created = xTaskCreate(
            WeatherTaskEntry, "weather_sync", kWeatherTaskStackSize, this,
            kWeatherTaskPriority, &weather_task_handle_);
        if (created != pdPASS)
        {
            weather_task_handle_ = nullptr;
            task_start_failed = true;
        }
    }

    if (task_start_failed)
    {
        ESP_LOGE(kTag, "内存不足，无法启动天气同步任务");
        ShowWeatherStatusIfUnavailable("天气任务启动失败");
    }
}

/**
 * @brief FreeRTOS 天气 Worker 入口，捕获异常并始终释放任务占用标志。
 * @param context 指向 BackendService 单例。
 */
void BackendService::WeatherTaskEntry(void *context)
{
    auto *service = static_cast<BackendService *>(context);
    try
    {
        service->RunWeatherSync();
    }
    catch (const std::exception &exception)
    {
        ESP_LOGE(kTag, "天气同步任务异常，原因=%s", exception.what());
        service->ShowWeatherStatusIfUnavailable("天气同步失败");
    }
    catch (...)
    {
        ESP_LOGE(kTag, "天气同步任务发生未知异常");
        service->ShowWeatherStatusIfUnavailable("天气同步失败");
    }
    {
        std::lock_guard<std::mutex> lock(service->weather_mutex_);
        service->weather_task_handle_ = nullptr;
    }
    vTaskDelete(nullptr);
}

/**
 * @brief esp_timer 周期回调，只在屏保可见时检查天气缓存是否过期。
 * @param context 指向 BackendService 单例。
 */
void BackendService::WeatherTimerCallback(void *context)
{
    auto *service = static_cast<BackendService *>(context);
    service->StartWeatherSync(false);
}

/**
 * @brief 通过设备 Token 获取一组天气数据并更新运行内存缓存。
 */
void BackendService::RunWeatherSync()
{
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
        ShowWeatherStatusIfUnavailable("请先绑定囤囤管家");
        return;
    }

    const HttpResponse response = SendJsonRequest(
        "GET", kWeatherPath, access_token, "");
    cJSON *root = nullptr;
    cJSON *data = nullptr;
    std::string response_message;
    bool parsed = response.transport_succeeded && response.status_code == 200 && ParseSuccessData(response.body, &root, &data, response_message);

    WeatherSnapshot snapshot;
    if (parsed)
    {
        parsed = ReadBoundedString(
                     data, "location_name", kWeatherLocationMaxBytes,
                     snapshot.location_name) &&
                 ReadTemperature(data, "temperature", snapshot.temperature) && ReadBoundedString(data, "weather", kWeatherDescriptionMaxBytes, snapshot.weather) && ReadTemperature(data, "low_temperature", snapshot.low_temperature) && ReadTemperature(data, "high_temperature", snapshot.high_temperature) && snapshot.low_temperature <= snapshot.high_temperature;
    }
    cJSON_Delete(root);

    if (!parsed)
    {
        ESP_LOGW(kTag, "天气同步失败，HTTP 状态码=%d", response.status_code);
        ShowWeatherStatusIfUnavailable(
            response.status_code == 401 || response.status_code == 403
                ? "设备认证已失效"
                : "天气同步失败");
        return;
    }

    snapshot.valid = true;
    {
        std::lock_guard<std::mutex> lock(weather_mutex_);
        weather_snapshot_ = snapshot;
        weather_last_success_us_ = esp_timer_get_time();
    }
    if (screensaver_active_.load())
    {
        ShowWeatherSnapshot(snapshot);
    }
    ESP_LOGI(kTag, "天气同步成功");
}

/**
 * @brief 把有效天气快照写入圆屏表盘。
 * @param snapshot 已通过接口字段和温度范围校验的天气数据。
 */
void BackendService::ShowWeatherSnapshot(const WeatherSnapshot &snapshot)
{
    if (!screensaver_active_.load() || !snapshot.valid)
    {
        return;
    }
    Display *display = Board::GetInstance().GetDisplay();
    if (display != nullptr)
    {
        display->SetScreensaverWeather(
            snapshot.location_name,
            snapshot.temperature,
            snapshot.weather,
            snapshot.low_temperature,
            snapshot.high_temperature);
    }
}

/**
 * @brief 没有历史天气可保留时，向天气区域写入固定状态文本。
 * @param message 天气位置区域的加载、网络、认证或错误状态。
 */
void BackendService::ShowWeatherStatusIfUnavailable(const std::string &message)
{
    if (!screensaver_active_.load())
    {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(weather_mutex_);
        if (weather_snapshot_.valid)
        {
            return;
        }
    }

    Display *display = Board::GetInstance().GetDisplay();
    if (display != nullptr)
    {
        display->SetScreensaverWeatherText(
            message,
            kWeatherTemperaturePlaceholder,
            kWeatherDescriptionPlaceholder,
            kWeatherRangePlaceholder);
    }
}

/**
 * @brief 创建通过语音设置固定城市或 IP 自动定位的独立 FreeRTOS 任务。
 * @param location_name 固定模式下用户提供的城市或区县名称；IP 模式下为空。
 * @param use_ip_auto true 切换为设备公网 IP 自动定位；false 设置固定城市。
 * @param completion 保存结果的一次性回调。
 */
void BackendService::StartWeatherLocationTask(
    const std::string &location_name,
    bool use_ip_auto,
    WeatherLocationCompletion completion)
{
    if (!network_connected_.load())
    {
        completion("设备网络尚未连接，暂时无法设置天气城市。", true);
        return;
    }
    if (!use_ip_auto && (location_name.empty() || location_name.size() > kWeatherLocationInputMaxBytes || location_name.find_first_not_of(" \t\r\n") == std::string::npos))
    {
        completion("城市名称为空或过长，请提供明确的城市或区县名称。", true);
        return;
    }

    bool has_device_credential = false;
    {
        std::lock_guard<std::mutex> lock(binding_mutex_);
        has_device_credential = !device_access_token_.empty();
    }
    if (!has_device_credential)
    {
        completion("设备尚未绑定囤囤管家，无法保存天气城市。", true);
        return;
    }

    auto *context = new (std::nothrow) WeatherLocationTaskContext{
        this, location_name, use_ip_auto, std::move(completion)};
    if (context == nullptr)
    {
        completion("设备内存不足，无法设置天气城市。", true);
        return;
    }

    bool task_already_running = false;
    bool task_start_failed = false;
    {
        std::lock_guard<std::mutex> lock(weather_mutex_);
        if (weather_location_task_handle_ != nullptr)
        {
            task_already_running = true;
        }
        else
        {
            const BaseType_t created = xTaskCreate(
                WeatherLocationTaskEntry,
                "weather_location",
                kWeatherTaskStackSize,
                context,
                kWeatherTaskPriority,
                &weather_location_task_handle_);
            task_start_failed = created != pdPASS;
            if (task_start_failed)
            {
                weather_location_task_handle_ = nullptr;
            }
        }
    }

    if (!task_already_running && !task_start_failed)
    {
        return;
    }

    auto callback = std::move(context->completion);
    delete context;
    callback(task_already_running
                 ? "设备正在保存另一个天气城市，请稍后再试。"
                 : "设备内存不足，无法启动天气城市设置任务。",
             true);
}

/**
 * @brief FreeRTOS 天气城市设置任务入口，确保上下文和任务标志始终释放。
 * @param context WeatherLocationTaskContext 指针。
 */
void BackendService::WeatherLocationTaskEntry(void *context)
{
    std::unique_ptr<WeatherLocationTaskContext> task_context(
        static_cast<WeatherLocationTaskContext *>(context));
    BackendService *service = task_context->service;
    try
    {
        service->RunWeatherLocationTask(
            task_context->location_name,
            task_context->use_ip_auto,
            task_context->completion);
    }
    catch (const std::exception &exception)
    {
        ESP_LOGE(kTag, "天气位置设置任务异常，原因=%s", exception.what());
        FinishToolRequest(
            task_context->completion,
            "天气城市设置失败，请稍后重试。",
            true);
    }
    catch (...)
    {
        ESP_LOGE(kTag, "天气位置设置任务发生未知异常");
        FinishToolRequest(
            task_context->completion,
            "天气城市设置失败，请稍后重试。",
            true);
    }
    {
        std::lock_guard<std::mutex> lock(service->weather_mutex_);
        service->weather_location_task_handle_ = nullptr;
    }
    vTaskDelete(nullptr);
}

/**
 * @brief 调用设备天气设置接口，并在成功后清除当前固件中的旧天气快照。
 * @param location_name 固定模式下用户提供的城市或区县名称；IP 模式下为空。
 * @param use_ip_auto true 保存 IP 自动定位模式；false 保存固定城市模式。
 * @param completion 保存结果的一次性回调。
 */
void BackendService::RunWeatherLocationTask(
    const std::string &location_name,
    bool use_ip_auto,
    WeatherLocationCompletion &completion)
{
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

    const std::string request_json = BuildWeatherLocationRequestJson(
        location_name,
        use_ip_auto);
    if (request_json.empty())
    {
        FinishToolRequest(completion, "设备内存不足，无法生成天气设置请求。", true);
        return;
    }

    const HttpResponse response = SendJsonRequest(
        "PUT", kWeatherSettingsPath, access_token, request_json);
    cJSON *root = nullptr;
    cJSON *data = nullptr;
    std::string response_message;
    bool parsed = response.transport_succeeded && response.status_code == 200 && ParseSuccessData(response.body, &root, &data, response_message);
    std::string resolved_location_name;
    if (parsed)
    {
        cJSON *location_mode = cJSON_GetObjectItemCaseSensitive(data, "location_mode");
        parsed = cJSON_IsNumber(location_mode) && location_mode->valueint == (use_ip_auto ? 2 : 1);
        if (parsed && !use_ip_auto)
        {
            parsed = ReadBoundedString(
                data,
                "location_name",
                kWeatherLocationInputMaxBytes,
                resolved_location_name);
        }
    }
    cJSON_Delete(root);

    if (!parsed)
    {
        ESP_LOGW(kTag, "天气位置更新失败，HTTP 状态码=%d", response.status_code);
        const std::string error_message = response.status_code == 401 || response.status_code == 403
                                              ? "设备认证已失效，请重新绑定设备。"
                                              : (response_message.empty()
                                                     ? "天气城市设置失败，请稍后重试。"
                                                     : response_message);
        FinishToolRequest(completion, error_message, true);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(weather_mutex_);
        weather_snapshot_ = WeatherSnapshot{};
        weather_last_success_us_ = 0;
    }
    if (screensaver_active_.load())
    {
        ShowWeatherStatusIfUnavailable("天气加载中");
        StartWeatherSync(true);
    }
    FinishToolRequest(
        completion,
        use_ip_auto
            ? "天气位置已切换为根据设备公网 IP 自动识别。"
            : "天气城市已设置为" + resolved_location_name + "。",
        false);
}

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
        }
        if (device_is_bound)
        {
            if (completion)
            {
                completion(
                    "当前设备已经绑定囤囤管家。如需解绑，请登录囤囤管家后台操作。",
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
                    BindingTaskEntry, "tuntun_bind", 8192, context, 3,
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
        ShowBindingPage(active_code, "请在囤囤管家网页端输入绑定码");
    }
    if (completion)
    {
        completion(active_code.empty()
                       ? "设备正在生成绑定码，请稍候。"
                       : "绑定码已经显示在设备屏幕上，请在囤囤管家网页端完成绑定。",
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
        ShowBindingPage("", "绑定流程异常，请稍后重试");
        FinishToolRequest(task_context->completion,
                          "设备绑定流程异常，请稍后重试。", true);
        vTaskDelay(pdMS_TO_TICKS(3000));
        HideBindingPage();
    }
    catch (...)
    {
        ESP_LOGE(kTag, "设备绑定任务发生未知异常");
        ShowBindingPage("", "绑定流程异常，请稍后重试");
        FinishToolRequest(task_context->completion,
                          "设备绑定流程异常，请稍后重试。", true);
        vTaskDelay(pdMS_TO_TICKS(3000));
        HideBindingPage();
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
            }
        });
    }
    vTaskDelete(nullptr);
}

/**
 * @brief 申请、显示、轮询并完成一次完整设备绑定流程。
 * @param request_new_session true 先创建新绑定会话。
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
        ShowBindingPage(binding_code, "请在囤囤管家网页端输入绑定码");
        FinishToolRequest(
            completion,
            "绑定码已经显示在设备屏幕上，请在囤囤管家网页端完成绑定。",
            false);
    }
    else
    {
        if (binding_code.empty() || session_token.empty())
        {
            return;
        }
        ShowBindingPage(binding_code, "请在囤囤管家网页端输入绑定码");
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
                ClearPendingBinding();
                ShowBindingPage("", "绑定码已过期，请重新申请");
                vTaskDelay(pdMS_TO_TICKS(3000));
                HideBindingPage();
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
            ClearPendingBinding();
            ShowBindingPage("", "绑定成功");
            vTaskDelay(pdMS_TO_TICKS(3000));
            HideBindingPage();
            ESP_LOGI(kTag, "设备绑定完成");
            return;
        }
        if (binding_status == kBindingStatusCompleted)
        {
            ClearPendingBinding();
            ShowBindingPage("", "设备已完成绑定");
            vTaskDelay(pdMS_TO_TICKS(3000));
            HideBindingPage();
            return;
        }
        if (binding_status == kBindingStatusExpired || binding_status == kBindingStatusFailed)
        {
            ClearPendingBinding();
            ShowBindingPage("", binding_status == kBindingStatusExpired
                                    ? "绑定码已过期，请重新申请"
                                    : "绑定失败，请重新申请");
            vTaskDelay(pdMS_TO_TICKS(3000));
            HideBindingPage();
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
