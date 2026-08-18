#ifndef TUNTUN_BACKEND_UTILS_H
#define TUNTUN_BACKEND_UTILS_H

/**
 * @file backend_utils.h
 * @brief 囤囤AI后端实现共享的常量、HTTP、JSON 与输入校验工具。
 */

#include "tuntun/backend/backend_service.h"

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

namespace tuntun::backend_internal
{

    /**
     * @brief 业务后端模块使用的日志标签。
     */
    constexpr const char *kTag = "TuntunBackend";

    /**
     * @brief 串行调度囤囤AI后端的 TLS 连接，并让通知音频优先于普通业务请求。
     * @details 资源受限芯片在证书校验阶段需要较大的连续内存，因此仍然只允许一个 HTTP/TLS
     * 连接活动；通知音频等待网络时会优先获得连接，普通请求继续排队而不并发握手。
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
    };

    inline BackendHttpGate backend_http_gate;

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
     * @brief 每次囤囤AI后端请求携带当前固件版本的请求头名称。
     */
    constexpr const char *kFirmwareVersionHeader = "X-Firmware-Version";

    /**
     * @brief 后端设备绑定接口相对于 CONFIG_TUNTUN_API_URL 的固定路径。
     */
    constexpr const char *kBindingRequestPath = "/api/device/binding/request";
    constexpr const char *kBindingStatusPath = "/api/device/binding/status";
    constexpr const char *kBindingCompletePath = "/api/device/binding/complete";

    /**
     * @brief 绑定码页面固定显示的后台输入提示和访问域名。
     * @note 绑定后台对所有外部硬件开放；接入自有后端时可改成你的平台名称与后台地址。
     */
    constexpr const char *kBindingPageMessage =
        "请在囤囤AI后台输入\nweb.tuntun.life";

    /**
     * @brief 使用设备访问 Token 获取当前屏保天气的固定接口路径。
     */
    constexpr const char *kWeatherPath = "/api/device/weather";
    constexpr const char *kWeatherSettingsPath = "/api/device/weather/settings";

    /** @brief 使用设备访问 Token 管理自定义提醒的固定接口路径。 */
    constexpr const char *kCustomRemindersPath = "/api/device/custom-reminders";

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
     * @brief 绑定状态轮询周期和 HTTP 超时，单位均为毫秒。
     */
    constexpr int kBindingPollIntervalMs = 3000;
    constexpr int kBindingRetryIntervalMs = 5000;
    constexpr int kHttpTimeoutMs = 10000;
    constexpr int kWeatherHttpTimeoutMs = 45000;

    /**
     * @brief 天气缓存新鲜周期和后台检查周期。
     * @details 固件每 10 分钟只检查一次缓存年龄，成功天气在 30 分钟内不会重复请求；真正的
     * HTTPS 请求统一由 BackendService 常驻 Worker 串行处理。
     */
    constexpr int64_t kWeatherRefreshIntervalUs = 30LL * 60LL * 1000LL * 1000LL;
    constexpr int64_t kWeatherCheckIntervalUs = 10LL * 60LL * 1000LL * 1000LL;

    /**
     * @brief 屏保待办区域缓存周期和后台检查周期。
     * @details 屏保持续显示时每 30 分钟兜底刷新一次；后端增删改会通过 MQTT 提示设备立即刷新，
     * 请求统一排入常驻 BackendService Worker。
     */
    constexpr int64_t kPendingReminderRefreshIntervalUs = 30LL * 60LL * 1000LL * 1000LL;
    constexpr int64_t kPendingReminderCheckIntervalUs = 30LL * 60LL * 1000LL * 1000LL;
    constexpr std::array<uint32_t, 8> kPendingReminderRetryIntervalsMinutes = {
        1, 2, 5, 10, 20, 30, 30, 30};

    /**
     * @brief 动态 MCP 清单和工具执行的调度配置。
     * @details 清单同步与工具执行都涉及 HTTPS 和 cJSON，统一由常驻 BackendService Worker
     * 串行运行，避免反复申请 8KB 任务栈。
     */

    /**
     * @brief 主动通知 HTTPS 补偿、音频下载和任务资源限制。
     */
    constexpr int64_t kNotificationCheckIntervalUs = 5LL * 60LL * 1000LL * 1000LL;
    /** @brief 设备业务 EMQX 心跳周期，固定为两分钟。 */
    constexpr int64_t kHeartbeatIntervalUs = 2LL * 60LL * 1000LL * 1000LL;
    /**
     * @brief 通知确认任务的专用栈大小，单位为字节；通知同步本身使用常驻 Worker。
     */
    constexpr uint32_t kNotificationTaskStackSize = 10240;
    constexpr UBaseType_t kNotificationTaskPriority = 2;
    constexpr uint32_t kNotificationPlaybackTaskStackSize = 16384;
    constexpr size_t kNotificationResponseMaxBytes = 12288;
    constexpr size_t kNotificationAudioMaxBytes = 512 * 1024;
    /** @brief 通知 Ogg 流的单次网络读取大小，减少短读和解封装调用次数。 */
    constexpr size_t kNotificationAudioReadChunkBytes = 4096;
    /** @brief 通知音频开始解码前的目标缓存时长，单位为毫秒。 */
    constexpr size_t kNotificationAudioPrebufferDurationMs = 800;
    constexpr size_t kNotificationTextMaxBytes = 1500;
    constexpr int kNotificationMaximumAudioSegments = 32;
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
    inline BaseType_t CreatePsramBackendTask(
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
    inline void DeleteCurrentPsramTask()
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
    constexpr int kNotificationSourceCustomReminder = 4;
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
    inline std::string GetVisibleDynamicToolName(const std::string &tool_name)
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
     * 最多 300 个 UTF-8 字节。
     */
    constexpr size_t kWeatherLocationMaxBytes = 96;
    constexpr size_t kWeatherLocationInputMaxBytes = 300;
    constexpr size_t kWeatherDescriptionMaxBytes = 48;
    constexpr int kWeatherMinimumTemperature = -100;
    constexpr int kWeatherMaximumTemperature = 100;

    /**
     * @brief 单次后端 JSON 响应允许占用的最大字节数和分段读取缓冲区大小。
     * @details 绑定接口响应均远小于 4KB。设置硬上限可以避免异常网关或错误页面返回大正文时
     * 持续占用设备堆内存；超过上限的响应会被当作传输失败处理。
     */
    constexpr size_t kMaxApiResponseBytes = 4096;
    constexpr size_t kMcpManifestResponseMaxBytes = 12288;
    constexpr size_t kHttpReadChunkBytes = 512;

    /**
     * @brief 自定义提醒内容的固件边界。
     * @details 后端正文上限为 500 个 Unicode 字符，按中文 UTF-8 最坏情况预留 1500 字节。
     */
    constexpr size_t kPendingReminderTextMaxBytes = 1500;
    constexpr size_t kPendingReminderTimeMaxBytes = 48;
    constexpr size_t kPendingReminderQueryItemMaxBytes = 360;
    constexpr size_t kPendingReminderQueryResultMaxBytes = 3000;
    constexpr size_t kMaximumScreensaverPendingReminderCount = 10;
    constexpr size_t kMaximumScreensaverPendingReminderLines = 3;

    /** @brief 自定义提醒请求、响应和语音查询结果的固件安全边界。 */
    constexpr size_t kCustomReminderResponseMaxBytes = 12288;
    constexpr size_t kCustomReminderContentMaxBytes = 1500;
    constexpr size_t kCustomReminderTimeMaxBytes = 48;
    constexpr size_t kCustomReminderRangesMaxBytes = 160;
    constexpr size_t kCustomReminderQueryItemMaxBytes = 300;
    constexpr size_t kCustomReminderQueryResultMaxBytes = 3000;
    constexpr size_t kMaximumCustomReminderQueryCount = 5;
    constexpr size_t kMaximumCustomReminderRangeCount = 8;
    constexpr int kMaximumCustomReminderIntervalMinutes = 365 * 24 * 60;

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
    /** @brief 屏保待办区域在设备未绑定时显示的操作引导。 */
    constexpr const char *kUnboundPendingReminderPrompt =
        "请先绑定囤囤AI\n唤醒我，和我说：“绑定设备”吧";

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
    inline std::string BuildApiUrl(const char *path)
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
    inline HttpResponse SendJsonRequest(const char *method, const char *path,
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
        http->SetHeader(kFirmwareVersionHeader, esp_app_get_description()->version);
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
        BackendService::GetInstance().HandleApiAuthenticationFailure(
            response.status_code,
            bearer_token);
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
    inline bool ParseSuccessData(const std::string &body, cJSON **root, cJSON **data,
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
    inline bool ParseSuccessEnvelope(const std::string &body, std::string &message)
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
    inline bool ReadBoundedString(cJSON *object, const char *name, size_t maximum_bytes,
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
    inline bool ReadTemperature(cJSON *object, const char *name, int &value)
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
    inline std::string BuildBindingRequestJson()
    {
        auto &board = Board::GetInstance();
        const esp_app_desc_t *app_description = esp_app_get_description();
        cJSON *root = cJSON_CreateObject();
        if (root == nullptr)
        {
            return {};
        }
        cJSON_AddStringToObject(root, "hardware_id", SystemInfo::GetMacAddress().c_str());
        cJSON_AddNumberToObject(root, "device_model", BOARD_DEVICE_MODEL);
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
    inline std::string BuildWeatherLocationRequestJson(
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
    inline bool IsValidTimeText(const std::string &value)
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

    /** @brief 前置声明后续定义的安全资源编号校验函数。 */
    inline bool IsSafeResourceId(const std::string &resource_id);
    /** @brief 前置声明后续定义的无符号整数读取函数。 */
    inline bool ReadUnsignedInteger(cJSON *item, bool allow_zero, uint32_t &value);

    /** @brief 保存一段已经规范化的自定义提醒每日允许时间。 */
    struct CustomReminderTimeRange
    {
        /** @brief 每日开始时间，格式为 HH:mm:ss。 */
        std::string start_time;
        /** @brief 每日结束时间，格式为 HH:mm:ss。 */
        std::string end_time;
    };

    /** @brief 保存后端返回且已经完成边界校验的一条自定义提醒。 */
    struct CustomReminderDetail
    {
        /** @brief 自定义提醒唯一编号。 */
        std::string id;
        /** @brief 提醒正文。 */
        std::string content;
        /** @brief 带时区偏移的首次执行时间。 */
        std::string first_run_at;
        /** @brief 1 为一次性提醒，2 为间隔循环提醒。 */
        int schedule_type = 0;
        /** @brief 0 为停用，1 为启用，2 为已完成。 */
        int status = 0;
        /** @brief 循环提醒间隔分钟数；一次性提醒为 0。 */
        int interval_minutes = 0;
        /** @brief 循环提醒每日允许时间段。 */
        std::vector<CustomReminderTimeRange> ranges;
    };

    /**
     * @brief 去除字符串首尾的 ASCII 空白。
     * @param value 需要规范化的工具参数片段。
     * @return 不含首尾空格、制表符和换行符的副本。
     */
    inline std::string TrimAsciiWhitespace(const std::string &value)
    {
        const size_t begin = value.find_first_not_of(" \t\r\n");
        if (begin == std::string::npos)
        {
            return {};
        }
        const size_t end = value.find_last_not_of(" \t\r\n");
        return value.substr(begin, end - begin + 1);
    }

    /**
     * @brief 把 HH:mm 或 HH:mm:ss 规范化为后端 TimeOnly 使用的 HH:mm:ss。
     * @param value 工具传入的单个时间文本。
     * @param normalized 成功时接收规范化结果。
     * @return 格式和时间范围均有效时返回 true。
     */
    inline bool NormalizeCustomReminderTime(const std::string &value, std::string &normalized)
    {
        if (value.size() == 5 && value[2] == ':')
        {
            normalized = value + ":00";
        }
        else
        {
            normalized = value;
        }
        return IsValidTimeText(normalized);
    }

    /**
     * @brief 解析逗号分隔的每日允许时间段。
     * @param value 空字符串表示全天；其他值格式为 HH:mm-HH:mm，并可包含多个逗号分隔项。
     * @param ranges 成功时接收按开始时间排序且互不重叠的结构化时间段。
     * @return 全部时段格式有效、开始早于结束且互不重叠时返回 true。
     */
    inline bool ParseCustomReminderRanges(
        const std::string &value,
        std::vector<CustomReminderTimeRange> &ranges)
    {
        ranges.clear();
        if (TrimAsciiWhitespace(value).empty())
        {
            return true;
        }

        size_t offset = 0;
        while (offset <= value.size())
        {
            const size_t separator = value.find(',', offset);
            const std::string part = TrimAsciiWhitespace(value.substr(
                offset,
                separator == std::string::npos ? std::string::npos : separator - offset));
            const size_t dash = part.find('-');
            if (dash == std::string::npos || part.find('-', dash + 1) != std::string::npos)
            {
                return false;
            }

            CustomReminderTimeRange range;
            if (!NormalizeCustomReminderTime(
                    TrimAsciiWhitespace(part.substr(0, dash)), range.start_time)
                || !NormalizeCustomReminderTime(
                    TrimAsciiWhitespace(part.substr(dash + 1)), range.end_time)
                || range.start_time >= range.end_time)
            {
                return false;
            }
            ranges.push_back(std::move(range));
            if (ranges.size() > kMaximumCustomReminderRangeCount)
            {
                return false;
            }
            if (separator == std::string::npos)
            {
                break;
            }
            offset = separator + 1;
        }

        std::sort(ranges.begin(), ranges.end(), [](const auto &left, const auto &right)
        {
            return left.start_time < right.start_time;
        });
        for (size_t index = 1; index < ranges.size(); ++index)
        {
            if (ranges[index].start_time < ranges[index - 1].end_time)
            {
                return false;
            }
        }
        return true;
    }

    /**
     * @brief 构建创建或完整修改自定义提醒使用的强类型 JSON。
     * @param content 提醒正文。
     * @param schedule_type 1 为一次性提醒，2 为间隔循环提醒。
     * @param status 0 为停用，1 为启用。
     * @param first_run_at 带时区偏移的 RFC 3339 首次执行时间。
     * @param interval_minutes 循环间隔分钟数；一次性提醒传 0。
     * @param ranges 已规范化的每日允许时间段。
     * @return 符合后端保存请求结构的紧凑 JSON；内存不足时返回空字符串。
     */
    inline std::string BuildCustomReminderRequestJson(
        const std::string &content,
        int schedule_type,
        int status,
        const std::string &first_run_at,
        int interval_minutes,
        const std::vector<CustomReminderTimeRange> &ranges)
    {
        cJSON *root = cJSON_CreateObject();
        if (root == nullptr)
        {
            return {};
        }
        cJSON_AddStringToObject(root, "content", content.c_str());
        cJSON_AddNumberToObject(root, "schedule_type", schedule_type);
        cJSON_AddNumberToObject(root, "status", status);
        cJSON_AddStringToObject(root, "first_run_at", first_run_at.c_str());
        if (schedule_type == 1)
        {
            cJSON_AddNullToObject(root, "interval_minutes");
        }
        else
        {
            cJSON_AddNumberToObject(root, "interval_minutes", interval_minutes);
        }

        cJSON *range_array = cJSON_AddArrayToObject(root, "allowed_time_ranges");
        if (range_array == nullptr)
        {
            cJSON_Delete(root);
            return {};
        }
        for (const auto &range : ranges)
        {
            cJSON *item = cJSON_CreateObject();
            if (item == nullptr)
            {
                cJSON_Delete(root);
                return {};
            }
            cJSON_AddStringToObject(item, "start_time", range.start_time.c_str());
            cJSON_AddStringToObject(item, "end_time", range.end_time.c_str());
            cJSON_AddItemToArray(range_array, item);
        }

        char *json_text = cJSON_PrintUnformatted(root);
        std::string result = json_text == nullptr ? std::string() : std::string(json_text);
        cJSON_free(json_text);
        cJSON_Delete(root);
        return result;
    }

    /**
     * @brief 解析并校验后端返回的一条自定义提醒详情。
     * @param data 统一响应中的 data 对象。
     * @param detail 成功时接收提醒详情。
     * @return 必要字段、枚举、间隔和时间段全部有效时返回 true。
     */
    inline bool ParseCustomReminderDetail(cJSON *data, CustomReminderDetail &detail)
    {
        uint32_t schedule_type = 0;
        uint32_t status = 0;
        if (!cJSON_IsObject(data)
            || !ReadBoundedString(data, "id", 50, detail.id)
            || !IsSafeResourceId(detail.id)
            || !ReadBoundedString(
                data, "content", kCustomReminderContentMaxBytes, detail.content)
            || !ReadBoundedString(
                data, "first_run_at", kCustomReminderTimeMaxBytes, detail.first_run_at)
            || !ReadUnsignedInteger(
                cJSON_GetObjectItemCaseSensitive(data, "schedule_type"), false, schedule_type)
            || schedule_type > 2
            || !ReadUnsignedInteger(
                cJSON_GetObjectItemCaseSensitive(data, "status"), true, status)
            || status > 2)
        {
            return false;
        }
        detail.schedule_type = static_cast<int>(schedule_type);
        detail.status = static_cast<int>(status);
        detail.interval_minutes = 0;
        detail.ranges.clear();

        cJSON *interval = cJSON_GetObjectItemCaseSensitive(data, "interval_minutes");
        uint32_t interval_value = 0;
        if (detail.schedule_type == 1)
        {
            if (!cJSON_IsNull(interval))
            {
                return false;
            }
        }
        else if (!ReadUnsignedInteger(interval, false, interval_value)
                 || interval_value > kMaximumCustomReminderIntervalMinutes)
        {
            return false;
        }
        else
        {
            detail.interval_minutes = static_cast<int>(interval_value);
        }

        cJSON *range_array = cJSON_GetObjectItemCaseSensitive(data, "allowed_time_ranges");
        if (!cJSON_IsArray(range_array)
            || cJSON_GetArraySize(range_array) > static_cast<int>(kMaximumCustomReminderRangeCount))
        {
            return false;
        }
        cJSON *item = nullptr;
        cJSON_ArrayForEach(item, range_array)
        {
            CustomReminderTimeRange range;
            if (!ReadBoundedString(item, "start_time", 8, range.start_time)
                || !ReadBoundedString(item, "end_time", 8, range.end_time)
                || !IsValidTimeText(range.start_time)
                || !IsValidTimeText(range.end_time)
                || range.start_time >= range.end_time)
            {
                return false;
            }
            detail.ranges.push_back(std::move(range));
        }
        return true;
    }

    /**
     * @brief 构建设备主动通知确认请求 JSON。
     * @param delivery_id 当前设备投递 UUID。
     * @param ack_type 固定确认动作数值。
     * @return 紧凑 JSON；内存不足时返回空字符串。
     */
    inline std::string BuildNotificationAckJson(
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
    inline std::string TruncateUtf8(const std::string &value, size_t maximum_bytes)
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
     * @brief 校验后端资源编号可安全拼接到固定 API 路径。
     * @param resource_id 查询工具返回的后端唯一编号。
     * @return 长度为 1 至 50 且只包含 ASCII 字母、数字或连字符时返回 true。
     */
    inline bool IsSafeResourceId(const std::string &resource_id)
    {
        if (resource_id.empty() || resource_id.size() > 50)
        {
            return false;
        }
        return std::all_of(resource_id.begin(), resource_id.end(), [](char character)
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
    inline bool ParseFixedDecimal(
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
     * @brief 按当前本地日期格式化屏保提醒行的提醒时间。
     * @param remind_at 后端返回的带 +08:00 偏移 RFC 3339 时间。
     * @param result 成功时接收 HH:mm、MM-dd 或 yyyy-MM-dd。
     * @return 时间结构和字段范围有效时返回 true。
     * @details 今天只显示时间；其他日期只显示日期，其中今年省略年份、非今年保留年份。
     * 系统时间尚未校准时无法判断“今天”和“今年”，此时保守显示完整日期。
     */
    inline bool FormatReminderTimeLine(
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
     * @brief 判断一条带 +08:00 偏移的提醒时间是否尚未过期（今天或未来）。
     * @param run_at 带时区偏移的 RFC 3339 首次执行时间或下次执行时间。
     * @return 时间可解析且日期不早于北京时间今天时返回 true；已过期或格式无效返回 false。
     * @details 设备已按中国时区校准，本地 today 边界即 +08:00；按本地日历日比较即可。
     * 系统时间尚未校准时无法判断"今天"，保守返回 true 以保证提醒仍能显示。
     */
    inline bool IsReminderUpcomingBeijing(const std::string &run_at)
    {
        if (run_at.size() < 10 || run_at[4] != '-' || run_at[7] != '-')
        {
            return false;
        }
        int year = 0;
        int month = 0;
        int day = 0;
        if (!ParseFixedDecimal(run_at, 0, 4, year)
            || !ParseFixedDecimal(run_at, 5, 2, month)
            || !ParseFixedDecimal(run_at, 8, 2, day)
            || year < 2000 || year > 9999
            || month < 1 || month > 12
            || day < 1 || day > 31)
        {
            return false;
        }
        const time_t now = time(nullptr);
        struct tm local_time = {};
        if (now < 1609459200 || localtime_r(&now, &local_time) == nullptr)
        {
            return true;
        }
        const int today = (local_time.tm_year + 1900) * 10000
            + (local_time.tm_mon + 1) * 100 + local_time.tm_mday;
        const int remind_day = year * 10000 + month * 100 + day;
        return remind_day >= today;
    }

    /**
     * @brief 检查 JSON 数值节点是否为有限非负整数且未超过 uint32_t 上限。
     * @param item 需要检查的 JSON 节点。
     * @param allow_zero true 允许0；false 要求至少为1。
     * @param value 校验成功时接收转换后的整数。
     * @return 节点为有限非负整数且没有超过 uint32_t 上限时返回 true。
     */
    inline bool ReadUnsignedInteger(cJSON *item, bool allow_zero, uint32_t &value)
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
    inline bool IsValidDynamicToolName(const std::string &name)
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
    inline std::string BuildDynamicToolRequestJson(
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
     * @brief 至多调用一次异步 MCP 结果回调，并在调用前清空原回调。
     * @param completion 设备绑定或天气城市设置需要消费的一次性回调；为空时不执行操作。
     * @param message 返回给大模型的中文结果文本。
     * @param is_error true 表示对应的设备绑定或天气城市操作失败。
     * @details 先转移并清空回调可防止回调自身抛出异常时，任务异常边界再次回复同一条
     * JSON-RPC 请求。
     */
    inline void FinishToolRequest(
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

} // namespace tuntun::backend_internal

#endif // TUNTUN_BACKEND_UTILS_H
