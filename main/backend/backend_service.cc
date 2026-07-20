/**
 * @file backend_service.cc
 * @brief 囤囤AI设备绑定、屏保天气、备忘录和动态 MCP 实现。
 */

#include "backend_service.h"

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

#include "application.h"
#include "assets/lang_config.h"
#include "audio/demuxer/ogg_demuxer.h"
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
     * @brief 串行调度囤囤AI后端的 TLS 连接，并让通知音频优先于普通业务请求。
     * @details ESP32-C5 在证书校验阶段需要较大的连续内存，因此仍然只允许一个 HTTP/TLS
     *          连接活动；通知音频等待网络时会优先获得连接，普通请求继续排队而不并发握手。
     */
    class BackendHttpGate {
    public:
        /** @brief 获取普通 JSON 请求或通知音频请求的网络使用权。 */
        void Lock(bool notification_audio) {
            std::unique_lock<std::mutex> lock(mutex_);
            if (notification_audio) {
                ++waiting_audio_count_;
                condition_.wait(lock, [this]() { return !active_; });
                --waiting_audio_count_;
            } else {
                condition_.wait(lock, [this]() {
                    return !active_ && waiting_audio_count_ == 0;
                });
            }
            active_ = true;
        }

        /** @brief 释放当前 HTTP/TLS 网络使用权并唤醒排队请求。 */
        void Unlock() {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                active_ = false;
            }
            condition_.notify_all();
        }

    private:
        std::mutex mutex_;
        std::condition_variable condition_;
        bool active_ = false;
        size_t waiting_audio_count_ = 0;
    } backend_http_gate;

    /**
     * @brief BackendHttpGate 的异常安全使用守卫。
     */
    class BackendHttpLease {
    public:
        explicit BackendHttpLease(bool notification_audio) {
            backend_http_gate.Lock(notification_audio);
        }
        ~BackendHttpLease() {
            backend_http_gate.Unlock();
        }
        BackendHttpLease(const BackendHttpLease&) = delete;
        BackendHttpLease& operator=(const BackendHttpLease&) = delete;
    };

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
     * @brief 使用设备访问 Token 创建、查询和统计备忘录的固定接口路径。
     */
    constexpr const char *kMemosPath = "/api/device/memos";
    constexpr const char *kMemoStatisticsPath = "/api/device/memos/statistics";
    constexpr const char *kMemoScreensaverPath =
        "/api/device/memos?page_index=1&page_size=5&screen_only=true";

    /**
     * @brief 使用设备访问 Token 同步和执行动态 MCP 工具的固定接口路径。
     */
    constexpr const char *kMcpManifestPath = "/api/mcp-tools/manifest";
    constexpr const char *kMcpExecutePath = "/api/mcp-tools/execute";

    /**
     * @brief 主动通知、业务 EMQX 配置和设备确认接口路径。
     */
    constexpr const char *kNotificationPendingPath = "/api/device/notifications/pending";
    constexpr const char *kNotificationMqttConfigPath = "/api/device/mqtt/config";

    /**
     * @brief 设备型号在后端 DeviceModelEnum 中对应 Tuntun Moji2 ESP32-C5 的数值。
     */
    constexpr int kDeviceModelTuntunMoji2Esp32C5 = 1;

    /**
     * @brief 绑定状态轮询周期和 HTTP 超时，单位均为毫秒。
     */
    constexpr int kBindingPollIntervalMs = 3000;
    constexpr int kBindingRetryIntervalMs = 5000;
    constexpr int kHttpTimeoutMs = 10000;
    constexpr int kWeatherHttpTimeoutMs = 45000;

    /**
     * @brief 天气缓存新鲜周期和后台检查周期。
     * @details 固件每 10 分钟只检查一次缓存年龄，成功天气在 30 分钟内不会重复请求；真正的
     *          HTTPS 请求统一由 BackendService 常驻 Worker 串行处理。
     */
    constexpr int64_t kWeatherRefreshIntervalUs = 30LL * 60LL * 1000LL * 1000LL;
    constexpr int64_t kWeatherCheckIntervalUs = 10LL * 60LL * 1000LL * 1000LL;

    /**
     * @brief 屏保备忘录缓存周期和后台检查周期。
     * @details 屏保持续显示时每 30 分钟兜底刷新一次；后端增删改会通过 MQTT 提示设备立即刷新，
     *          请求统一排入常驻 BackendService Worker。
     */
    constexpr int64_t kMemoRefreshIntervalUs = 30LL * 60LL * 1000LL * 1000LL;
    constexpr int64_t kMemoCheckIntervalUs = 30LL * 60LL * 1000LL * 1000LL;
    constexpr std::array<uint32_t, 8> kMemoRetryIntervalsMinutes = {
        1, 2, 5, 10, 20, 30, 30, 30};

    /**
     * @brief 动态 MCP 清单和工具执行的调度配置。
     * @details 清单同步与工具执行都涉及 HTTPS 和 cJSON，统一由常驻 BackendService Worker
     *          串行运行，避免反复申请 8KB 任务栈。
     */

    /**
     * @brief 主动通知 HTTPS 补偿、音频下载和任务资源限制。
     */
    constexpr int64_t kNotificationCheckIntervalUs = 5LL * 60LL * 1000LL * 1000LL;
    /**
     * @brief 通知确认任务的专用栈大小，单位为字节；通知同步本身使用常驻 Worker。
     */
    constexpr uint32_t kNotificationTaskStackSize = 10240;
    constexpr UBaseType_t kNotificationTaskPriority = 2;
    constexpr uint32_t kNotificationPlaybackTaskStackSize = 16384;
    constexpr size_t kNotificationResponseMaxBytes = 12288;
    constexpr size_t kNotificationAudioMaxBytes = 512 * 1024;
    constexpr size_t kNotificationTextMaxBytes = 1500;
    constexpr uint32_t kNotificationConfirmationTimeoutMs = 30000;
    constexpr uint32_t kNotificationReconnectMaximumSeconds = 300;

    /**
     * @brief 创建非实时后端业务任务，并把任务栈放入板载 PSRAM。
     * @param task_entry FreeRTOS 任务入口函数。
     * @param task_name 用于诊断的任务名称。
     * @param stack_size_bytes 任务栈大小，单位为字节。
     * @param context 传递给任务入口的上下文指针。
     * @param priority 任务优先级。
     * @param task_handle 接收创建成功后的任务句柄。
     * @return 创建成功时返回 pdPASS，否则返回 FreeRTOS 创建错误码。
     */
    BaseType_t CreatePsramBackendTask(
        TaskFunction_t task_entry,
        const char *task_name,
        configSTACK_DEPTH_TYPE stack_size_bytes,
        void *context,
        UBaseType_t priority,
        TaskHandle_t *task_handle)
    {
#if CONFIG_SPIRAM
        return xTaskCreateWithCaps(
            task_entry,
            task_name,
            stack_size_bytes,
            context,
            priority,
            task_handle,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
        return xTaskCreate(
            task_entry,
            task_name,
            stack_size_bytes,
            context,
            priority,
            task_handle);
#endif
    }

    /**
     * @brief 删除由 CreatePsramBackendTask 创建的当前任务并释放 PSRAM 任务栈。
     */
    void DeleteCurrentPsramTask()
    {
#if CONFIG_SPIRAM
        vTaskDeleteWithCaps(nullptr);
#else
        vTaskDelete(nullptr);
#endif
    }

    /**
     * @brief 后端 NotificationModeEnum 和设备确认动作稳定数值。
     */
    constexpr int kNotificationModeDirect = 1;
    constexpr int kNotificationModeConfirm = 2;
    constexpr int kNotificationSourceScheduledMcp = 1;
    constexpr int kNotificationSourceWeather = 2;
    constexpr int kNotificationSourceMemo = 3;
    constexpr int kNotificationAckReceived = 1;
    constexpr int kNotificationAckDeferred = 2;
    constexpr int kNotificationAckPlaying = 3;
    constexpr int kNotificationAckCompleted = 4;
    constexpr int kNotificationAckFailed = 5;
    constexpr int kNotificationAckDismissed = 6;

    /**
     * @brief 去除后台动态工具名称的固定命名空间，得到适合屏幕和语音介绍的名称。
     * @param tool_name 后端签发的完整工具名称。
     * @return 不包含 custom. 前缀的工具名称。
     */
    std::string GetVisibleDynamicToolName(const std::string &tool_name)
    {
        constexpr const char *prefix = "custom.";
        constexpr size_t prefix_length = 7;
        if (tool_name.compare(0, prefix_length, prefix) == 0)
        {
            return tool_name.substr(prefix_length);
        }
        return tool_name;
    }

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
    constexpr size_t kMemoResponseMaxBytes = 12288;
    constexpr size_t kHttpReadChunkBytes = 512;

    /**
     * @brief 备忘录正文、提醒时间和语音查询结果的固件边界。
     * @details 后端正文上限为 500 个 Unicode 字符，按中文 UTF-8 最坏情况预留 1500 字节。
     */
    constexpr size_t kMemoContentMaxBytes = 1500;
    constexpr size_t kMemoReminderTimeMaxBytes = 48;
    constexpr size_t kMemoQueryItemMaxBytes = 360;
    constexpr size_t kMemoQueryResultMaxBytes = 3000;
    constexpr size_t kMaximumScreensaverMemoCount = 5;

    /**
     * @brief 第一版动态 MCP 清单和统一执行结果的固件安全边界。
     */
    constexpr size_t kMaximumDynamicToolCount = 10;
    constexpr size_t kDynamicToolNameMaxBytes = 64;
    constexpr size_t kDynamicToolDisplayNameMaxBytes = 400;
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
     * @param timeout_ms 本次请求允许等待的最长毫秒数。
     * @return 传输状态、HTTP 状态码和响应正文。
     * @details 本方法不记录请求或响应正文，避免绑定会话 Token 和设备 Token 进入日志。
     */
    HttpResponse SendJsonRequest(const char *method, const char *path,
                                 const std::string &bearer_token,
                                 std::string request_body,
                                 size_t maximum_response_bytes = kMaxApiResponseBytes,
                                 int timeout_ms = kHttpTimeoutMs)
    {
        BackendHttpLease request_lock(false);
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
        http->SetTimeout(timeout_ms);
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
     * @brief 解析不要求 data 对象的统一成功响应。
     * @param body 完整响应正文。
     * @param message 输出服务端可安全展示的响应说明。
     * @return JSON 根节点有效且业务 code 为 0 时返回 true。
     * @details 删除接口成功时 data 为 null，因此不能使用要求 data 必须为对象的 ParseSuccessData。
     */
    bool ParseSuccessEnvelope(const std::string &body, std::string &message)
    {
        cJSON *root = cJSON_Parse(body.c_str());
        if (root == nullptr)
        {
            message = "平台响应格式无效";
            return false;
        }
        cJSON *message_item = cJSON_GetObjectItemCaseSensitive(root, "message");
        if (cJSON_IsString(message_item) && message_item->valuestring != nullptr)
        {
            message = message_item->valuestring;
        }
        cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
        const bool succeeded = cJSON_IsNumber(code) && code->valueint == 0;
        cJSON_Delete(root);
        return succeeded;
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
        cJSON_AddNumberToObject(root, "device_model", kDeviceModelTuntunMoji2Esp32C5);
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
     * @param announcement_time 每日播报时间；空字符串表示关闭播报。
     * @return 包含完整天气配置的紧凑 JSON；内存不足时返回空字符串。
     */
    std::string BuildWeatherLocationRequestJson(
        const std::string &location_name,
        bool use_ip_auto,
        const std::string &announcement_time)
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
        if (announcement_time.empty())
        {
            cJSON_AddNullToObject(root, "announcement_time");
        }
        else
        {
            cJSON_AddStringToObject(root, "announcement_time", announcement_time.c_str());
        }
        char *json_text = cJSON_PrintUnformatted(root);
        std::string result = json_text == nullptr ? std::string() : std::string(json_text);
        cJSON_free(json_text);
        cJSON_Delete(root);
        return result;
    }

    /**
     * @brief 校验后端 TimeOnly 使用的 HH:mm:ss 文本。
     */
    bool IsValidTimeText(const std::string &value)
    {
        if (value.size() != 8 || value[2] != ':' || value[5] != ':')
        {
            return false;
        }
        for (size_t index : {0U, 1U, 3U, 4U, 6U, 7U})
        {
            if (value[index] < '0' || value[index] > '9')
            {
                return false;
            }
        }
        const int hour = (value[0] - '0') * 10 + value[1] - '0';
        const int minute = (value[3] - '0') * 10 + value[4] - '0';
        const int second = (value[6] - '0') * 10 + value[7] - '0';
        return hour <= 23 && minute <= 59 && second <= 59;
    }

    /**
     * @brief 构建语音创建备忘录使用的固定强类型 JSON。
     * @param content 用户口述的备忘录正文。
     * @param remind_at RFC 3339 提醒时间；空字符串表示不设置提醒时间。
     * @return 包含 content 和 remind_at 的紧凑 JSON；内存不足时返回空字符串。
     */
    std::string BuildMemoCreateRequestJson(
        const std::string &content,
        const std::string &remind_at)
    {
        cJSON *root = cJSON_CreateObject();
        if (root == nullptr)
        {
            return {};
        }
        cJSON_AddStringToObject(root, "content", content.c_str());
        if (remind_at.empty())
        {
            cJSON_AddNullToObject(root, "remind_at");
        }
        else
        {
            cJSON_AddStringToObject(root, "remind_at", remind_at.c_str());
        }
        char *json_text = cJSON_PrintUnformatted(root);
        std::string result = json_text == nullptr ? std::string() : std::string(json_text);
        cJSON_free(json_text);
        cJSON_Delete(root);
        return result;
    }

    /**
     * @brief 构建完整修改备忘录使用的固定强类型 JSON。
     * @param content 修改后的完整正文。
     * @param remind_at 修改后的 RFC 3339 提醒时间；空字符串表示取消提醒时间。
     * @param status 修改后的状态，1 和 2 分别表示未完成和已完成。
     * @return 包含 content、remind_at 和 status 的紧凑 JSON；内存不足时返回空字符串。
     */
    std::string BuildMemoUpdateRequestJson(
        const std::string &content,
        const std::string &remind_at,
        int status)
    {
        cJSON *root = cJSON_CreateObject();
        if (root == nullptr)
        {
            return {};
        }
        cJSON_AddStringToObject(root, "content", content.c_str());
        if (remind_at.empty())
        {
            cJSON_AddNullToObject(root, "remind_at");
        }
        else
        {
            cJSON_AddStringToObject(root, "remind_at", remind_at.c_str());
        }
        cJSON_AddNumberToObject(root, "status", status);
        char *json_text = cJSON_PrintUnformatted(root);
        std::string result = json_text == nullptr ? std::string() : std::string(json_text);
        cJSON_free(json_text);
        cJSON_Delete(root);
        return result;
    }

    /**
     * @brief 构建设备主动通知确认请求 JSON。
     * @param delivery_id 当前设备投递 UUID。
     * @param ack_type 固定确认动作数值。
     * @return 紧凑 JSON；内存不足时返回空字符串。
     */
    std::string BuildNotificationAckJson(
        const std::string &delivery_id,
        int ack_type)
    {
        cJSON *root = cJSON_CreateObject();
        if (root == nullptr)
        {
            return {};
        }
        cJSON_AddStringToObject(root, "delivery_id", delivery_id.c_str());
        cJSON_AddNumberToObject(root, "ack_type", ack_type);
        char *json_text = cJSON_PrintUnformatted(root);
        std::string result = json_text == nullptr ? std::string() : std::string(json_text);
        cJSON_free(json_text);
        cJSON_Delete(root);
        return result;
    }

    /**
     * @brief 在不拆分 UTF-8 字符的前提下限制返回给模型的文本字节数。
     * @param value 原始 UTF-8 文本。
     * @param maximum_bytes 允许保留的最大字节数。
     * @return 未超限时返回原文；超限时返回完整字符边界内的前缀并追加省略号。
     */
    std::string TruncateUtf8(const std::string &value, size_t maximum_bytes)
    {
        if (value.size() <= maximum_bytes)
        {
            return value;
        }
        size_t length = maximum_bytes;
        while (length > 0
               && (static_cast<unsigned char>(value[length]) & 0xC0U) == 0x80U)
        {
            --length;
        }
        return value.substr(0, length) + "…";
    }

    /**
     * @brief 校验后端备忘录 ID 可安全拼接到固定 API 路径。
     * @param memo_id 查询工具返回的后端唯一编号。
     * @return 长度为 1 至 50 且只包含 ASCII 字母、数字或连字符时返回 true。
     */
    bool IsSafeMemoId(const std::string &memo_id)
    {
        if (memo_id.empty() || memo_id.size() > 50)
        {
            return false;
        }
        return std::all_of(memo_id.begin(), memo_id.end(), [](char character)
        {
            return (character >= 'a' && character <= 'z')
                || (character >= 'A' && character <= 'Z')
                || (character >= '0' && character <= '9')
                || character == '-';
        });
    }

    /**
     * @brief 从 RFC 3339 文本的固定位置读取十进制日期或时间字段。
     * @param value 完整提醒时间文本。
     * @param offset 字段起始字节下标。
     * @param length 字段固定数字位数。
     * @param result 成功时接收十进制整数。
     * @return 指定范围全部为 ASCII 数字时返回 true。
     */
    bool ParseFixedDecimal(
        const std::string &value,
        size_t offset,
        size_t length,
        int &result)
    {
        if (offset + length > value.size())
        {
            return false;
        }
        result = 0;
        for (size_t index = 0; index < length; ++index)
        {
            const char character = value[offset + index];
            if (character < '0' || character > '9')
            {
                return false;
            }
            result = result * 10 + (character - '0');
        }
        return true;
    }

    /**
     * @brief 按当前本地日期格式化屏保备忘录的提醒时间行。
     * @param remind_at 后端返回的带 +08:00 偏移 RFC 3339 时间。
     * @param result 成功时接收 HH:mm、MM-dd 或 yyyy-MM-dd。
     * @return 时间结构和字段范围有效时返回 true。
     * @details 今天只显示时间；其他日期只显示日期，其中今年省略年份、非今年保留年份。
     *          系统时间尚未校准时无法判断“今天”和“今年”，此时保守显示完整日期。
     */
    bool FormatMemoReminderLine(
        const std::string &remind_at,
        std::string &result)
    {
        if (remind_at.size() < 16
            || remind_at[4] != '-'
            || remind_at[7] != '-'
            || (remind_at[10] != 'T' && remind_at[10] != ' ')
            || remind_at[13] != ':')
        {
            return false;
        }

        int year = 0;
        int month = 0;
        int day = 0;
        int hour = 0;
        int minute = 0;
        if (!ParseFixedDecimal(remind_at, 0, 4, year)
            || !ParseFixedDecimal(remind_at, 5, 2, month)
            || !ParseFixedDecimal(remind_at, 8, 2, day)
            || !ParseFixedDecimal(remind_at, 11, 2, hour)
            || !ParseFixedDecimal(remind_at, 14, 2, minute)
            || year < 2000 || year > 9999
            || month < 1 || month > 12
            || day < 1 || day > 31
            || hour < 0 || hour > 23
            || minute < 0 || minute > 59)
        {
            return false;
        }

        const time_t now = time(nullptr);
        struct tm local_time = {};
        const bool current_time_valid = now >= 1609459200
            && localtime_r(&now, &local_time) != nullptr;
        char formatted[20] = {};
        if (current_time_valid
            && year == local_time.tm_year + 1900
            && month == local_time.tm_mon + 1
            && day == local_time.tm_mday)
        {
            std::snprintf(formatted, sizeof(formatted), "%02d:%02d", hour, minute);
        }
        else if (current_time_valid && year == local_time.tm_year + 1900)
        {
            std::snprintf(formatted, sizeof(formatted), "%02d-%02d", month, day);
        }
        else
        {
            std::snprintf(formatted, sizeof(formatted), "%04d-%02d-%02d", year, month, day);
        }
        result = formatted;
        return true;
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
        StartMemoSync(false);
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
    if (notification_reconnect_timer_ != nullptr)
    {
        esp_timer_stop(notification_reconnect_timer_);
    }
    notification_reconnect_delay_seconds_.store(kNotificationReconnectInitialSeconds);
    std::unique_ptr<Mqtt> mqtt;
    {
        std::lock_guard<std::mutex> lock(notification_mutex_);
        active_notification_mqtt_.store(nullptr);
        mqtt = std::move(notification_mqtt_);
    }
    if (mqtt != nullptr)
    {
        mqtt->Disconnect();
    }
    ShowWeatherStatusIfUnavailable("等待网络连接");
    ShowMemoStatusIfUnavailable("等待网络连接");
}

/**
 * @brief 语音 MCP 会话结束不取消设备级绑定流程。
 */
void BackendService::OnMcpDisconnected()
{
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
        ShowMemoStatusIfUnavailable("请先绑定囤囤AI");
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
            ShowMemoStatusIfUnavailable("请先绑定囤囤AI");
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
            ShowWeatherStatusIfUnavailable("请先绑定囤囤AI");
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
 * @brief 在天气缓存确实需要更新时把请求加入常驻后端 Worker。
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

    bool mqtt_refresh_requested = false;
    {
        std::lock_guard<std::mutex> lock(weather_mutex_);
        if (weather_task_handle_ != nullptr)
        {
            return;
        }
        mqtt_refresh_requested = weather_refresh_requested_.exchange(false);
        force_refresh = force_refresh || mqtt_refresh_requested;
        const int64_t now_us = esp_timer_get_time();
        if (!force_refresh && weather_snapshot_.valid && weather_last_success_us_ > 0 && now_us - weather_last_success_us_ < kWeatherRefreshIntervalUs)
        {
            return;
        }

        weather_task_handle_ = backend_worker_task_handle_;
    }

    if (!EnqueueBackendJob(BackendJobType::WeatherSync))
    {
        {
            std::lock_guard<std::mutex> lock(weather_mutex_);
            weather_task_handle_ = nullptr;
            if (mqtt_refresh_requested)
            {
                weather_refresh_requested_.store(true);
            }
        }
        ESP_LOGE(kTag, "后端 Worker 队列已满，无法安排天气同步");
        ShowWeatherStatusIfUnavailable("天气任务启动失败");
    }
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
        ShowWeatherStatusIfUnavailable("请先绑定囤囤AI");
        return;
    }

    const HttpResponse response = SendJsonRequest(
        "GET", kWeatherPath, access_token, "", kMaxApiResponseBytes,
        kWeatherHttpTimeoutMs);
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
 * @brief 把通过语音设置固定城市或 IP 自动定位的请求加入常驻后端 Worker。
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
        completion("设备尚未绑定囤囤AI，无法保存天气城市。", true);
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
    {
        std::lock_guard<std::mutex> lock(weather_mutex_);
        if (weather_location_task_handle_ != nullptr ||
            weather_announcement_task_handle_ != nullptr)
        {
            task_already_running = true;
        }
        else
        {
            weather_location_task_handle_ = backend_worker_task_handle_;
        }
    }

    if (task_already_running)
    {
        auto callback = std::move(context->completion);
        delete context;
        callback("设备正在保存另一项天气设置，请稍后再试。", true);
        return;
    }

    if (!EnqueueBackendJob(BackendJobType::WeatherLocation, context))
    {
        {
            std::lock_guard<std::mutex> lock(weather_mutex_);
            weather_location_task_handle_ = nullptr;
        }
        auto callback = std::move(context->completion);
        delete context;
        callback("设备后台任务队列已满，无法设置天气城市。", true);
    }
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
    if (!network_connected_.load())
    {
        FinishToolRequest(completion, "设备网络尚未连接，暂时无法设置天气城市。", true);
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

    const HttpResponse settings_response = SendJsonRequest(
        "GET", kWeatherSettingsPath, access_token, "");
    cJSON *settings_root = nullptr;
    cJSON *settings_data = nullptr;
    std::string settings_message;
    bool settings_parsed = settings_response.transport_succeeded &&
        settings_response.status_code == 200 &&
        ParseSuccessData(
            settings_response.body,
            &settings_root,
            &settings_data,
            settings_message);
    std::string announcement_time;
    if (settings_parsed)
    {
        cJSON *announcement_item = cJSON_GetObjectItemCaseSensitive(
            settings_data, "announcement_time");
        if (cJSON_IsString(announcement_item) &&
            announcement_item->valuestring != nullptr)
        {
            announcement_time = announcement_item->valuestring;
            settings_parsed = IsValidTimeText(announcement_time);
        }
        else
        {
            settings_parsed = cJSON_IsNull(announcement_item);
        }
    }
    cJSON_Delete(settings_root);
    if (!settings_parsed)
    {
        FinishToolRequest(
            completion,
            settings_message.empty() ? "无法读取当前天气配置，请稍后重试。" : settings_message,
            true);
        return;
    }

    const std::string request_json = BuildWeatherLocationRequestJson(
        location_name,
        use_ip_auto,
        announcement_time);
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
 * @brief 创建每日天气播报设置任务。
 */
void BackendService::StartWeatherAnnouncementTask(
    bool enabled,
    const std::string &announcement_time,
    WeatherLocationCompletion completion)
{
    if (!network_connected_.load())
    {
        completion("设备网络尚未连接，暂时无法设置天气播报。", true);
        return;
    }
    if (enabled && !IsValidTimeText(announcement_time))
    {
        completion("天气播报时间格式无效，请提供具体的小时和分钟。", true);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(binding_mutex_);
        if (device_access_token_.empty())
        {
            completion("设备尚未绑定囤囤AI，无法设置天气播报。", true);
            return;
        }
    }

    auto *context = new (std::nothrow) WeatherAnnouncementTaskContext{
        this,
        enabled,
        announcement_time,
        std::move(completion)};
    if (context == nullptr)
    {
        completion("设备内存不足，无法设置天气播报。", true);
        return;
    }

    bool task_already_running = false;
    {
        std::lock_guard<std::mutex> lock(weather_mutex_);
        if (weather_location_task_handle_ != nullptr ||
            weather_announcement_task_handle_ != nullptr)
        {
            task_already_running = true;
        }
        else
        {
            weather_announcement_task_handle_ = backend_worker_task_handle_;
        }
    }
    if (task_already_running)
    {
        auto callback = std::move(context->completion);
        delete context;
        callback("设备正在保存另一项天气设置，请稍后再试。", true);
        return;
    }

    if (!EnqueueBackendJob(BackendJobType::WeatherAnnouncement, context))
    {
        {
            std::lock_guard<std::mutex> lock(weather_mutex_);
            weather_announcement_task_handle_ = nullptr;
        }
        auto callback = std::move(context->completion);
        delete context;
        callback("设备后台任务队列已满，无法设置天气播报。", true);
    }
}

/**
 * @brief 保留当前位置配置并更新每日天气播报。
 */
void BackendService::RunWeatherAnnouncementTask(
    bool enabled,
    const std::string &announcement_time,
    WeatherLocationCompletion &completion)
{
    if (!network_connected_.load())
    {
        FinishToolRequest(completion, "设备网络尚未连接，暂时无法设置天气播报。", true);
        return;
    }
    std::string access_token;
    {
        std::lock_guard<std::mutex> lock(binding_mutex_);
        access_token = device_access_token_;
    }
    const HttpResponse settings_response = SendJsonRequest(
        "GET", kWeatherSettingsPath, access_token, "");
    cJSON *root = nullptr;
    cJSON *data = nullptr;
    std::string response_message;
    bool parsed = settings_response.transport_succeeded &&
        settings_response.status_code == 200 &&
        ParseSuccessData(settings_response.body, &root, &data, response_message);
    bool use_ip_auto = false;
    std::string location_name;
    if (parsed)
    {
        cJSON *location_mode = cJSON_GetObjectItemCaseSensitive(data, "location_mode");
        parsed = cJSON_IsNumber(location_mode) &&
            (location_mode->valueint == 1 || location_mode->valueint == 2);
        if (parsed)
        {
            use_ip_auto = location_mode->valueint == 2;
            if (!use_ip_auto)
            {
                parsed = ReadBoundedString(
                    data,
                    "location_name",
                    kWeatherLocationInputMaxBytes,
                    location_name);
            }
        }
    }
    cJSON_Delete(root);
    if (!parsed)
    {
        FinishToolRequest(
            completion,
            response_message.empty() ? "无法读取当前天气配置，请稍后重试。" : response_message,
            true);
        return;
    }

    const std::string request_json = BuildWeatherLocationRequestJson(
        location_name,
        use_ip_auto,
        enabled ? announcement_time : "");
    const HttpResponse update_response = SendJsonRequest(
        "PUT", kWeatherSettingsPath, access_token, request_json);
    cJSON *update_root = nullptr;
    cJSON *update_data = nullptr;
    response_message.clear();
    const bool updated = update_response.transport_succeeded &&
        update_response.status_code == 200 &&
        ParseSuccessData(
            update_response.body,
            &update_root,
            &update_data,
            response_message);
    cJSON_Delete(update_root);
    if (!updated)
    {
        FinishToolRequest(
            completion,
            response_message.empty() ? "天气播报设置失败，请稍后重试。" : response_message,
            true);
        return;
    }

    FinishToolRequest(
        completion,
        enabled
            ? "每日天气播报已设置为" + announcement_time.substr(0, 5) + "。"
            : "每日天气播报已关闭。",
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
        ShowBindingPage(active_code, "请在囤囤AI网页端输入绑定码");
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
                service->StartMemoSync(true);
            }
        });
    }
    // 绑定成功后会写入 NVS；Flash 缓存关闭期间任务栈必须保留在片内 SRAM。
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
        ShowBindingPage(binding_code, "请在囤囤AI网页端输入绑定码");
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
        ShowBindingPage(binding_code, "请在囤囤AI网页端输入绑定码");
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
    if (DequeueNotificationHint(hint))
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
    const HttpResponse response = SendJsonRequest(
        "GET",
        kNotificationPendingPath,
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
    if (!cJSON_IsNumber(code) || code->valueint != 0 || !cJSON_IsArray(data))
    {
        cJSON_Delete(root);
        ESP_LOGW(kTag, "主动通知响应结构无效");
        return;
    }
    cJSON *item = cJSON_GetArrayItem(data, 0);
    if (!cJSON_IsObject(item))
    {
        cJSON_Delete(root);
        return;
    }

    PendingNotification notification;
    cJSON *source_type = cJSON_GetObjectItemCaseSensitive(item, "source_type");
    cJSON *notification_mode = cJSON_GetObjectItemCaseSensitive(item, "notification_mode");
    cJSON *has_audio = cJSON_GetObjectItemCaseSensitive(item, "has_audio");
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
                       source_type->valueint <= kNotificationSourceMemo &&
                       cJSON_IsNumber(notification_mode) &&
                       (notification_mode->valueint == kNotificationModeDirect ||
                        notification_mode->valueint == kNotificationModeConfirm) &&
                       cJSON_IsBool(has_audio);
    if (!valid)
    {
        cJSON_Delete(root);
        ESP_LOGW(kTag, "主动通知字段校验失败");
        return;
    }
    notification.notification_mode = notification_mode->valueint;
    notification.has_audio = cJSON_IsTrue(has_audio);
    if (source_type->valueint == kNotificationSourceScheduledMcp)
    {
        cJSON *tool_name = cJSON_GetObjectItemCaseSensitive(item, "mcp_tool_name");
        notification.source_title = cJSON_IsString(tool_name) && tool_name->valuestring != nullptr
            ? GetVisibleDynamicToolName(tool_name->valuestring)
            : "MCP 通知";
    }
    else
    {
        notification.source_title = source_type->valueint == kNotificationSourceWeather
            ? "天气播报"
            : "备忘录提醒";
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
    cJSON *port = cJSON_GetObjectItemCaseSensitive(data, "port");
    cJSON *use_tls = cJSON_GetObjectItemCaseSensitive(data, "use_tls");
    const bool valid = ReadBoundedString(data, "host", 255, host) &&
                       ReadBoundedString(data, "client_id", 128, client_id) &&
                       ReadBoundedString(data, "username", 256, username) &&
                       ReadBoundedString(data, "password", 256, password) &&
                       ReadBoundedString(data, "topic", 255, topic) &&
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
    if (CreatePsramBackendTask(
            NotificationPlaybackTaskEntry,
            "notify_play",
            kNotificationPlaybackTaskStackSize,
            this,
            kNotificationTaskPriority,
            &notification_playback_task_handle_) != pdPASS)
    {
        notification_playback_task_handle_ = nullptr;
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
            auto packet = std::make_unique<AudioStreamPacket>();
            packet->sample_rate = sample_rate;
            packet->frame_duration = frame_duration;
            packet->payload.assign(data, data + size);
            if (Application::GetInstance().GetAudioService().PushPacketToDecodeQueue(
                    std::move(packet), true))
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
    std::array<char, kHttpReadChunkBytes> read_buffer{};
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
        const std::string audio_path =
            "/api/device/notifications/" + notification.notification_id + "/audio";
        Application::GetInstance().GetAudioService().ResetDecoder();
        played = StreamNotificationAudio(audio_path, access_token);
        if (played && !notification_playback_interrupted_.load())
        {
            Application::GetInstance().GetAudioService().WaitForPlaybackQueueEmpty();
        }
        else if (!notification_playback_interrupted_.load())
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
        if (display != nullptr && app.GetDeviceState() == kDeviceStateIdle)
        {
            display->SetScreensaverMode(true);
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
