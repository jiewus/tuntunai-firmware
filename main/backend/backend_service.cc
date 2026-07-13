/**
 * @file backend_service.cc
 * @brief 吞吞生活后端认证、串行 HTTP Worker、屏保缓存与异步 MCP 工具实现。
 */

#include "backend_service.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>

#include <esp_app_desc.h>
#include <esp_log.h>
#include <esp_random.h>

#include "application.h"
#include "boards/common/board.h"
#include "display/display.h"
#include "settings.h"
#include "system_info.h"

namespace {

constexpr const char* kTag = "TuntunBackend";
constexpr const char* kApiBaseUrl = "https://api.tuntun.life";
constexpr const char* kActivationCode = "TuntunLife-2026_Act#ivate@Code";
constexpr const char* kDeviceModel = "movecall-moji2-esp32c5";
constexpr const char* kSettingsNamespace = "tuntun_api";
constexpr const char* kDeviceIdKey = "device_id";
constexpr const char* kAccessTokenKey = "access_token";
constexpr int kHttpTimeoutMs = 10000;
constexpr size_t kActivationResponseLimit = 4096;
constexpr size_t kWeatherResponseLimit = 4096;
constexpr size_t kMemoResponseLimit = 12 * 1024;
constexpr UBaseType_t kWorkerPriority = tskIDLE_PRIORITY;
constexpr uint32_t kWorkerStackSize = 8 * 1024;
constexpr UBaseType_t kQueueLength = 8;
constexpr TickType_t kWeatherRefreshInterval = pdMS_TO_TICKS(30 * 60 * 1000);
constexpr TickType_t kMemoRefreshInterval = pdMS_TO_TICKS(5 * 60 * 1000);

/**
 * @brief 从 JSON 对象安全读取字符串字段。
 * @param object 字段所属 JSON 对象。
 * @param name 字段名。
 * @return 字段存在且类型为字符串时返回其副本，否则返回空字符串。
 */
std::string GetJsonString(const cJSON* object, const char* name) {
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(value) && value->valuestring != nullptr
        ? std::string(value->valuestring)
        : std::string();
}

/**
 * @brief 创建可安全返回给 MCP 的统一错误 JSON。
 * @param code 后端业务码、HTTP 状态码或设备本地错误码。
 * @param message 不包含令牌和备忘正文的简短错误说明。
 * @return 紧凑 JSON 字符串。
 */
std::string BuildErrorJson(int code, const std::string& message) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "code", code);
    cJSON_AddStringToObject(root, "message", message.c_str());
    char* serialized = cJSON_PrintUnformatted(root);
    std::string result = serialized != nullptr ? serialized : "{\"code\":500,\"message\":\"结果序列化失败\"}";
    if (serialized != nullptr) {
        cJSON_free(serialized);
    }
    cJSON_Delete(root);
    return result;
}

/**
 * @brief 把可选 ISO 8601 时间写入 JSON；空字符串写为 null。
 * @param root 目标 JSON 对象。
 * @param name 字段名。
 * @param value MCP 参数中的时间文本。
 */
void AddOptionalTime(cJSON* root, const char* name, const std::string& value) {
    if (value.empty()) {
        cJSON_AddNullToObject(root, name);
    } else {
        cJSON_AddStringToObject(root, name, value.c_str());
    }
}

/**
 * @brief 将字符串中的 ASCII 英文字母转换为小写，中文和其他 UTF-8 字节保持不变。
 * @param value MCP 工具传入的位置模式或位置文本。
 * @return 适合比较 automatic、auto、fixed 等英文关键字的字符串副本。
 */
std::string ToAsciiLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return character >= 'A' && character <= 'Z'
            ? static_cast<char>(character + ('a' - 'A'))
            : static_cast<char>(character);
    });
    return value;
}

/**
 * @brief 判断天气位置工具参数是否表达了公网 IP 自动定位意图。
 * @param mode 大模型显式提供的位置模式，推荐值为 automatic。
 * @param location_text 大模型误用固定位置工具时可能传入的自然语言位置文本。
 * @return 参数明确要求 automatic、auto、IP 定位或自动识别当前位置时返回 true。
 * @details 该兼容判断用于处理大模型把“改成 IP 自动定位”错误路由到
 *          self.weather.set_location 的情况，不会把普通省市区文本改成自动模式。
 */
bool IsAutomaticWeatherLocationIntent(
    const std::string& mode,
    const std::string& location_text) {
    const std::string normalized_mode = ToAsciiLower(mode);
    const std::string normalized_text = ToAsciiLower(location_text);
    if (normalized_mode == "automatic" || normalized_mode == "auto") {
        return true;
    }

    return normalized_text == "automatic"
        || normalized_text == "auto"
        || normalized_text == "ip"
        || normalized_text.find("ip自动定位") != std::string::npos
        || normalized_text.find("ip定位") != std::string::npos
        || normalized_text.find("根据ip") != std::string::npos
        || normalized_text.find("自动定位") != std::string::npos
        || normalized_text.find("自动识别") != std::string::npos
        || normalized_text.find("当前位置") != std::string::npos;
}

/**
 * @brief 判断组合天气位置工具是否只要求关闭自动定位并固定当前已保存位置。
 * @param mode 大模型显式提供的位置模式。
 * @param location_text 用户指定的固定省市区文本；非空时应优先执行位置设置。
 * @param selected_location_code 用户从歧义候选中确认的高德行政区编码。
 * @return 模式为 fixed 或 manual，且没有提交新位置和候选编码时返回 true。
 */
bool IsFixCurrentWeatherLocationIntent(
    const std::string& mode,
    const std::string& location_text,
    const std::string& selected_location_code) {
    const std::string normalized_mode = ToAsciiLower(mode);
    return (normalized_mode == "fixed" || normalized_mode == "manual")
        && location_text.empty()
        && selected_location_code.empty();
}

}  // namespace

/**
 * @brief 获取全局唯一的后端业务服务。
 * @return 静态 BackendService 实例。
 */
BackendService& BackendService::GetInstance() {
    static BackendService instance;
    return instance;
}

/**
 * @brief 创建固定容量作业队列和低优先级后台任务。
 * @details 队列元素只保存 Job 指针，实际 std::string 和回调位于堆对象中，避免 FreeRTOS
 *          对非平凡 C++ 对象执行按字节复制。Worker 是唯一释放成功入队 Job 的位置。
 */
void BackendService::Start() {
    if (worker_task_ != nullptr) {
        return;
    }

    /*
     * 公共 HttpClient 在 DEBUG 级别会输出完整请求头，其中可能包含 Authorization。
     * 将该组件标签固定到 INFO，确保即使全局日志级别被调高，也不会泄露后端令牌。
     */
    esp_log_level_set("HttpClient", ESP_LOG_INFO);

    queue_ = xQueueCreate(kQueueLength, sizeof(Job*));
    if (queue_ == nullptr) {
        ESP_LOGE(kTag, "Failed to create backend queue");
        return;
    }

    BaseType_t created = xTaskCreate(
        WorkerEntry,
        "tuntun_backend",
        kWorkerStackSize,
        this,
        kWorkerPriority,
        &worker_task_);
    if (created != pdPASS) {
        ESP_LOGE(kTag, "Failed to create backend worker");
        vQueueDelete(queue_);
        queue_ = nullptr;
    }
}

/**
 * @brief 将静态 FreeRTOS 任务入口转发到对象成员循环。
 * @param argument BackendService 实例地址。
 */
void BackendService::WorkerEntry(void* argument) {
    auto* service = static_cast<BackendService*>(argument);
    service->WorkerLoop();
    service->worker_task_ = nullptr;
    vTaskDelete(nullptr);
}

/**
 * @brief 串行处理显式作业，并每秒检查一次屏保周期刷新。
 * @details Tick 比较使用有符号差值，能够正确处理 FreeRTOS tick 计数回绕。
 */
void BackendService::WorkerLoop() {
    while (true) {
        Job* raw_job = nullptr;
        if (xQueueReceive(queue_, &raw_job, pdMS_TO_TICKS(1000)) == pdTRUE && raw_job != nullptr) {
            std::unique_ptr<Job> job(raw_job);
            ProcessJob(*job);
        }

        if (!screensaver_active_.load() || !network_connected_.load()) {
            continue;
        }

        const TickType_t now = xTaskGetTickCount();
        if (next_weather_refresh_ != 0
            && static_cast<int32_t>(now - next_weather_refresh_) >= 0
            && !weather_queued_.exchange(true)) {
            auto* weather_job = new Job();
            weather_job->type = JobType::WeatherRefresh;
            weather_job->screensaver_generation = screensaver_generation_.load();
            if (!Enqueue(weather_job, true)) {
                weather_queued_.store(false);
                delete weather_job;
                next_weather_refresh_ = now + pdMS_TO_TICKS(1000);
            } else {
                next_weather_refresh_ = now + kWeatherRefreshInterval;
            }
        }

        if (next_memo_refresh_ != 0
            && static_cast<int32_t>(now - next_memo_refresh_) >= 0
            && !memo_queued_.exchange(true)) {
            auto* memo_job = new Job();
            memo_job->type = JobType::MemoRefresh;
            memo_job->screensaver_generation = screensaver_generation_.load();
            if (!Enqueue(memo_job, true)) {
                memo_queued_.store(false);
                delete memo_job;
                next_memo_refresh_ = now + pdMS_TO_TICKS(1000);
            } else {
                next_memo_refresh_ = now + kMemoRefreshInterval;
            }
        }
    }
}

/**
 * @brief 把作业指针无等待写入固定队列。
 * @param job 尚未转移所有权的动态作业。
 * @param allow_coalesce true 表示调用方允许对屏保读刷新采用无等待入队策略。
 * @return 作业成功写入队列时返回 true；队列已满或尚未创建时返回 false。
 */
bool BackendService::Enqueue(Job* job, bool allow_coalesce) {
    if (queue_ == nullptr || job == nullptr) {
        return false;
    }
    if (xQueueSend(queue_, &job, 0) == pdTRUE) {
        return true;
    }

    if (allow_coalesce
        && (job->type == JobType::WeatherRefresh || job->type == JobType::MemoRefresh)) {
        return false;
    }
    return false;
}

/**
 * @brief 处理一个 Worker 独占作业并在所有分支中完成状态收尾。
 * @param job 当前队列作业。
 */
void BackendService::ProcessJob(Job& job) {
    if (job.type == JobType::Activate) {
        EnsureActivated();
        return;
    }

    if (job.type == JobType::WeatherRefresh) {
        Job request = job;
        request.endpoint = "/api/weather/get";
        request.body = "{}";
        request.response_limit = kWeatherResponseLimit;
        HttpResult result = ExecuteWithRetry(request);
        weather_queued_.store(false);
        if (result.success) {
            ApplyWeatherResponse(result.response_body, job.screensaver_generation);
        }
        if (screensaver_active_.load()
            && job.screensaver_generation != screensaver_generation_.load()) {
            next_weather_refresh_ = xTaskGetTickCount();
        }
        return;
    }

    if (job.type == JobType::MemoRefresh) {
        Job request = job;
        request.endpoint = "/api/memo/screensaver/get";
        request.body = "{}";
        request.response_limit = kMemoResponseLimit;
        HttpResult result = ExecuteWithRetry(request);
        memo_queued_.store(false);
        if (result.success) {
            ApplyMemoResponse(result.response_body, job.screensaver_generation);
        }
        if (screensaver_active_.load()
            && job.screensaver_generation != screensaver_generation_.load()) {
            next_memo_refresh_ = xTaskGetTickCount();
        }
        return;
    }

    HttpResult result = ExecuteWithRetry(job);
    if (result.success && screensaver_active_.load()) {
        if (job.endpoint.rfind("/api/weather/", 0) == 0
            && !weather_queued_.exchange(true)) {
            auto* refresh = new Job();
            refresh->type = JobType::WeatherRefresh;
            refresh->screensaver_generation = screensaver_generation_.load();
            if (!Enqueue(refresh, true)) {
                weather_queued_.store(false);
                delete refresh;
                next_weather_refresh_ = xTaskGetTickCount() + pdMS_TO_TICKS(1000);
            }
        } else if (job.write_operation
                   && job.endpoint.rfind("/api/memo/", 0) == 0
                   && !memo_queued_.exchange(true)) {
            auto* refresh = new Job();
            refresh->type = JobType::MemoRefresh;
            refresh->screensaver_generation = screensaver_generation_.load();
            if (!Enqueue(refresh, true)) {
                memo_queued_.store(false);
                delete refresh;
                next_memo_refresh_ = xTaskGetTickCount() + pdMS_TO_TICKS(1000);
            }
        }
    }
    if (job.completion) {
        if (result.success) {
            job.completion({result.response_body, false});
        } else {
            const int code = result.business_code != 0
                ? result.business_code
                : (result.http_status != 0 ? result.http_status : 503);
            const std::string message = result.message.empty() ? "后端服务暂时不可用" : result.message;
            job.completion({BuildErrorJson(code, message), true});
        }
    }
}

/**
 * @brief 在网络可用后排队一次自动激活检查。
 */
void BackendService::OnNetworkConnected() {
    network_connected_.store(true);
    if (screensaver_active_.load()) {
        const TickType_t now = xTaskGetTickCount();
        next_weather_refresh_ = now;
        next_memo_refresh_ = now;
    }
    auto* job = new Job();
    job->type = JobType::Activate;
    if (!Enqueue(job, false)) {
        delete job;
        ESP_LOGW(kTag, "Backend queue is full while scheduling activation");
    }
}

/**
 * @brief 标记网络断开，供读请求重试策略及时停止。
 */
void BackendService::OnNetworkDisconnected() {
    network_connected_.store(false);
    mcp_connected_.store(false);
}

/**
 * @brief 标记小智 MCP 协议已经释放，使后台读作业停止后续重试。
 */
void BackendService::OnMcpDisconnected() {
    mcp_connected_.store(false);
}

/**
 * @brief 切换屏保刷新生命周期，并在进入时按天气、备忘录顺序立即入队。
 * @param active 屏保是否当前可见。
 */
void BackendService::OnScreensaverChanged(bool active) {
    screensaver_active_.store(active);
    const uint32_t generation = screensaver_generation_.fetch_add(1) + 1;
    if (!active) {
        next_weather_refresh_ = 0;
        next_memo_refresh_ = 0;
        return;
    }

    const TickType_t now = xTaskGetTickCount();
    next_weather_refresh_ = now + kWeatherRefreshInterval;
    next_memo_refresh_ = now + kMemoRefreshInterval;

    if (!weather_queued_.exchange(true)) {
        auto* weather_job = new Job();
        weather_job->type = JobType::WeatherRefresh;
        weather_job->screensaver_generation = generation;
        if (!Enqueue(weather_job, true)) {
            weather_queued_.store(false);
            delete weather_job;
            next_weather_refresh_ = now + pdMS_TO_TICKS(1000);
        }
    }
    if (!memo_queued_.exchange(true)) {
        auto* memo_job = new Job();
        memo_job->type = JobType::MemoRefresh;
        memo_job->screensaver_generation = generation;
        if (!Enqueue(memo_job, true)) {
            memo_queued_.store(false);
            delete memo_job;
            next_memo_refresh_ = now + pdMS_TO_TICKS(1000);
        }
    }
}

/**
 * @brief 判断本地凭据是否完整，不完整时执行激活。
 * @return 凭据可用或激活成功时返回 true。
 */
bool BackendService::EnsureActivated() {
    Settings settings(kSettingsNamespace, false);
    const std::string device_id = settings.GetString(kDeviceIdKey);
    const std::string access_token = settings.GetString(kAccessTokenKey);
    if (!device_id.empty() && !access_token.empty()) {
        return true;
    }
    return Activate();
}

/**
 * @brief 调用公开激活接口，最多执行初次请求和三次退避重试。
 * @return 响应业务码为 0 且包含 device_id、access_token 时返回 true。
 */
bool BackendService::Activate() {
    if (!network_connected_.load()) {
        return false;
    }

    cJSON* body = cJSON_CreateObject();
    const std::string mac = SystemInfo::GetMacAddress();
    const std::string client_id = Board::GetInstance().GetUuid();
    const esp_app_desc_t* app = esp_app_get_description();
    cJSON_AddStringToObject(body, "serial_number", mac.c_str());
    cJSON_AddStringToObject(body, "activation_code", kActivationCode);
    cJSON_AddStringToObject(body, "mac_address", mac.c_str());
    cJSON_AddStringToObject(body, "client_id", client_id.c_str());
    cJSON_AddStringToObject(body, "model", kDeviceModel);
    cJSON_AddStringToObject(body, "firmware_version", app->version);
    const std::string request_body = SerializeAndDelete(body);

    constexpr std::array<int, 3> retry_delays_seconds = {5, 15, 30};
    HttpResult result;
    for (size_t attempt = 0; attempt <= retry_delays_seconds.size(); ++attempt) {
        result = Post("/api/device/activate", request_body, kActivationResponseLimit, "", false);
        if (result.success) {
            cJSON* root = cJSON_Parse(result.response_body.c_str());
            if (root == nullptr) {
                result.success = false;
                result.retryable = false;
                result.message = "激活响应不是有效 JSON";
                break;
            }
            const cJSON* data = cJSON_GetObjectItemCaseSensitive(root, "data");
            const std::string device_id = GetJsonString(data, "device_id");
            const std::string access_token = GetJsonString(data, "access_token");
            if (!device_id.empty() && !access_token.empty()) {
                Settings settings(kSettingsNamespace, true);
                settings.SetString(kDeviceIdKey, device_id);
                settings.SetString(kAccessTokenKey, access_token);
                cJSON_Delete(root);
                activation_failure_notified_.store(false);
                ESP_LOGI(kTag, "Backend activation succeeded");
                return true;
            }
            cJSON_Delete(root);
            result.success = false;
            result.retryable = false;
            result.message = "激活响应缺少设备凭据";
        }

        if (!result.retryable || attempt == retry_delays_seconds.size()) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(retry_delays_seconds[attempt] * 1000));
    }

    ESP_LOGW(kTag, "Backend activation failed, code=%d", result.business_code);
    if (!activation_failure_notified_.exchange(true)) {
        Application::GetInstance().Schedule([]() {
            Board::GetInstance().GetDisplay()->ShowNotification("业务服务激活失败，语音功能仍可使用", 5000);
        });
    }
    return false;
}

/**
 * @brief 清除普通 NVS 中保存的后端凭据。
 */
void BackendService::ClearCredentials() {
    Settings settings(kSettingsNamespace, true);
    settings.EraseKey(kDeviceIdKey);
    settings.EraseKey(kAccessTokenKey);
}

/**
 * @brief 创建独立 HTTP 对象并完成一次受限 POST 调用。
 * @param endpoint 相对 API 路径。
 * @param body UTF-8 JSON 请求体。
 * @param response_limit 最大响应字节数。
 * @param idempotency_key 可选幂等 UUID。
 * @param authenticated 是否添加 Bearer 和设备身份头。
 * @return 解析后的统一内部结果。
 */
BackendService::HttpResult BackendService::Post(
    const std::string& endpoint,
    const std::string& body,
    size_t response_limit,
    const std::string& idempotency_key,
    bool authenticated) {
    HttpResult result;
    if (!network_connected_.load()) {
        result.retryable = true;
        result.message = "网络未连接";
        return result;
    }

    auto http = Board::GetInstance().GetNetwork()->CreateHttp();
    if (!http) {
        result.retryable = true;
        result.message = "无法创建 HTTP 客户端";
        return result;
    }

    const esp_app_desc_t* app = esp_app_get_description();
    http->SetTimeout(kHttpTimeoutMs);
    http->SetKeepAlive(false);
    http->SetHeader("Content-Type", "application/json");
    http->SetHeader("Accept", "application/json");
    http->SetHeader("X-Request-Id", GenerateUuid());
    http->SetHeader("X-Client-Id", Board::GetInstance().GetUuid());
    http->SetHeader("X-Firmware-Version", app->version);
    if (!idempotency_key.empty()) {
        http->SetHeader("Idempotency-Key", idempotency_key);
    }

    if (authenticated) {
        Settings settings(kSettingsNamespace, false);
        const std::string device_id = settings.GetString(kDeviceIdKey);
        const std::string access_token = settings.GetString(kAccessTokenKey);
        if (device_id.empty() || access_token.empty()) {
            result.business_code = 2001;
            result.message = "设备尚未激活";
            return result;
        }
        http->SetHeader("Authorization", "Bearer " + access_token);
        http->SetHeader("X-Device-Id", device_id);
    }

    http->SetContent(std::string(body));
    if (!http->Open("POST", std::string(kApiBaseUrl) + endpoint)) {
        result.retryable = true;
        result.message = "后端连接失败";
        return result;
    }

    result.http_status = http->GetStatusCode();
    const size_t declared_length = http->GetBodyLength();
    if (declared_length > response_limit) {
        http->Close();
        result.message = "后端响应超过设备允许大小";
        return result;
    }

    std::array<char, 512> buffer{};
    while (true) {
        const int read = http->Read(buffer.data(), buffer.size());
        if (read < 0) {
            result.retryable = true;
            result.message = "读取后端响应失败";
            http->Close();
            return result;
        }
        if (read == 0) {
            break;
        }
        if (result.response_body.size() + static_cast<size_t>(read) > response_limit) {
            result.response_body.clear();
            result.message = "后端响应超过设备允许大小";
            http->Close();
            return result;
        }
        result.response_body.append(buffer.data(), static_cast<size_t>(read));
    }
    http->Close();

    if (result.http_status >= 500) {
        result.retryable = true;
        result.message = "后端服务暂时异常";
        return result;
    }
    if (result.http_status != 200) {
        result.message = "后端返回 HTTP " + std::to_string(result.http_status);
        return result;
    }

    cJSON* root = cJSON_Parse(result.response_body.c_str());
    if (root == nullptr) {
        result.business_code = 500;
        result.message = "后端响应格式无效";
        return result;
    }
    const cJSON* code = cJSON_GetObjectItemCaseSensitive(root, "code");
    const cJSON* message = cJSON_GetObjectItemCaseSensitive(root, "message");
    if (!cJSON_IsNumber(code)) {
        cJSON_Delete(root);
        result.business_code = 500;
        result.message = "后端响应格式无效";
        return result;
    }
    result.business_code = code->valueint;
    if (cJSON_IsString(message) && message->valuestring != nullptr) {
        result.message = message->valuestring;
    }
    result.success = result.business_code == 0;
    cJSON_Delete(root);

    ESP_LOGI(kTag, "POST %s http=%d code=%d", endpoint.c_str(), result.http_status, result.business_code);
    return result;
}

/**
 * @brief 对认证业务请求应用自动激活、未授权重签和网络错误退避。
 * @param job 当前业务作业。
 * @return 首次成功或最终失败结果。
 */
BackendService::HttpResult BackendService::ExecuteWithRetry(const Job& job) {
    HttpResult result;
    if (!EnsureActivated()) {
        result.business_code = 2001;
        result.message = "设备业务服务尚未激活";
        return result;
    }

    constexpr std::array<int, 3> retry_delays_seconds = {10, 30, 60};
    bool reactivated = false;
    for (size_t attempt = 0; attempt <= retry_delays_seconds.size(); ++attempt) {
        result = Post(job.endpoint, job.body, job.response_limit,
                      job.idempotency_key, true);
        if (result.success) {
            return result;
        }

        if (result.business_code == 2001 && !reactivated) {
            ClearCredentials();
            reactivated = true;
            if (Activate()) {
                continue;
            }
            return result;
        }

        if (!result.retryable || attempt == retry_delays_seconds.size()) {
            return result;
        }
        if (!job.write_operation
            && ((!network_connected_.load())
                || (job.mcp_request && !mcp_connected_.load()))) {
            result.message = "连接已断开，读取请求已停止重试";
            return result;
        }
        vTaskDelay(pdMS_TO_TICKS(retry_delays_seconds[attempt] * 1000));
    }
    return result;
}

/**
 * @brief 更新天气 RAM 缓存，并仅在同一世代屏保仍可见时刷新 UI。
 * @param response_body 天气接口统一响应。
 * @param generation 请求入队时记录的屏保世代。
 */
void BackendService::ApplyWeatherResponse(
    const std::string& response_body,
    uint32_t generation) {
    cJSON* root = cJSON_Parse(response_body.c_str());
    if (root == nullptr) {
        ESP_LOGW(kTag, "Weather response is not valid JSON");
        return;
    }
    const cJSON* data = cJSON_GetObjectItemCaseSensitive(root, "data");
    const cJSON* temperature = cJSON_GetObjectItemCaseSensitive(data, "temperature");
    const cJSON* high = cJSON_GetObjectItemCaseSensitive(data, "high_temperature");
    const cJSON* low = cJSON_GetObjectItemCaseSensitive(data, "low_temperature");
    const std::string location = GetJsonString(data, "location");
    const std::string weather = GetJsonString(data, "weather");
    if (!cJSON_IsNumber(temperature) || !cJSON_IsNumber(high)
        || !cJSON_IsNumber(low) || location.empty() || weather.empty()) {
        cJSON_Delete(root);
        ESP_LOGW(kTag, "Weather response data is incomplete");
        return;
    }

    BackendWeatherData snapshot;
    snapshot.location = location;
    snapshot.temperature = temperature->valueint;
    snapshot.high_temperature = high->valueint;
    snapshot.low_temperature = low->valueint;
    snapshot.weather = weather;
    snapshot.valid = true;
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        weather_cache_ = snapshot;
    }
    cJSON_Delete(root);

    if (screensaver_active_.load() && screensaver_generation_.load() == generation) {
        Application::GetInstance().Schedule([snapshot]() {
            if (Board::GetInstance().IsScreensaverActive()) {
                Board::GetInstance().GetDisplay()->SetScreensaverWeather(
                    snapshot.location,
                    snapshot.temperature,
                    snapshot.weather,
                    snapshot.low_temperature,
                    snapshot.high_temperature);
            }
        });
    }
}

/**
 * @brief 更新最多 5 条备忘录正文缓存，并按屏保世代决定是否更新 UI。
 * @param response_body 屏保备忘录接口统一响应。
 * @param generation 请求入队时记录的屏保世代。
 */
void BackendService::ApplyMemoResponse(
    const std::string& response_body,
    uint32_t generation) {
    cJSON* root = cJSON_Parse(response_body.c_str());
    if (root == nullptr) {
        ESP_LOGW(kTag, "Memo response is not valid JSON");
        return;
    }
    const cJSON* data = cJSON_GetObjectItemCaseSensitive(root, "data");
    const cJSON* memos = cJSON_GetObjectItemCaseSensitive(data, "memos");
    if (!cJSON_IsArray(memos)) {
        cJSON_Delete(root);
        ESP_LOGW(kTag, "Memo response data is incomplete");
        return;
    }

    std::vector<std::string> snapshot;
    const int count = std::min(cJSON_GetArraySize(memos), 5);
    snapshot.reserve(static_cast<size_t>(count));
    for (int index = 0; index < count; ++index) {
        const cJSON* memo = cJSON_GetArrayItem(memos, index);
        std::string content = GetJsonString(memo, "content");
        if (!content.empty()) {
            snapshot.push_back(std::move(content));
        }
    }
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        memo_cache_ = snapshot;
    }
    cJSON_Delete(root);

    if (screensaver_active_.load() && screensaver_generation_.load() == generation) {
        Application::GetInstance().Schedule([snapshot = std::move(snapshot)]() {
            if (Board::GetInstance().IsScreensaverActive()) {
                Board::GetInstance().GetDisplay()->SetScreensaverMemos(snapshot);
            }
        });
    }
}

/**
 * @brief 将通用 MCP API 请求放入串行 Worker。
 * @param endpoint 后端相对路径。
 * @param body JSON 请求体。
 * @param write_operation 是否为需要尽力完成的写操作。
 * @param completion MCP 完成回调。
 * @param idempotency_key 创建操作在重试期间复用的幂等 UUID。
 */
void BackendService::QueueApiCall(
    const std::string& endpoint,
    std::string body,
    bool write_operation,
    McpToolCompletion completion,
    std::string idempotency_key) {
    auto* job = new Job();
    job->type = JobType::ApiCall;
    job->endpoint = endpoint;
    job->body = std::move(body);
    job->response_limit = endpoint.rfind("/api/weather/", 0) == 0
        ? kWeatherResponseLimit
        : kMemoResponseLimit;
    job->write_operation = write_operation;
    job->mcp_request = true;
    mcp_connected_.store(true);
    job->completion = std::move(completion);
    job->idempotency_key = std::move(idempotency_key);
    if (!Enqueue(job, false)) {
        McpToolCompletion failed = std::move(job->completion);
        delete job;
        if (failed) {
            failed({BuildErrorJson(503, "设备业务队列已满，请稍后重试"), true});
        }
    }
}

/**
 * @brief 注册天气位置及备忘录异步工具，并把每个工具映射到独立 Controller 接口。
 * @param server 设备 MCP 服务器。
 */
void BackendService::RegisterMcpTools(McpServer& server) {
    server.AddAsyncTool(
        "self.weather.set_location",
        "配置屏保天气位置。设置省市区时传 location_text，成功后自动切换 fixed；用户要求按 IP 自动定位或自动识别当前位置时必须传 mode=automatic；用户只要求关闭自动定位时传 mode=fixed 且位置留空。若固定位置返回 confirmed=false，必须继续确认候选。",
        PropertyList({
            Property("mode", kPropertyTypeString, std::string("")),
            Property("location_text", kPropertyTypeString, std::string("")),
            Property("selected_location_code", kPropertyTypeString, std::string(""))
        }),
        [this](const PropertyList& properties, McpToolCompletion completion) {
            const std::string mode = properties["mode"].value<std::string>();
            const std::string location_text = properties["location_text"].value<std::string>();
            const std::string selected_location_code =
                properties["selected_location_code"].value<std::string>();

            if (IsAutomaticWeatherLocationIntent(mode, location_text)) {
                cJSON* body = cJSON_CreateObject();
                cJSON_AddStringToObject(body, "mode", "automatic");
                ESP_LOGI(kTag, "Weather location request routed to automatic mode");
                QueueApiCall("/api/weather/location/mode/set", SerializeAndDelete(body), true,
                             std::move(completion));
                return;
            }

            if (IsFixCurrentWeatherLocationIntent(mode, location_text, selected_location_code)) {
                cJSON* body = cJSON_CreateObject();
                cJSON_AddStringToObject(body, "mode", "fixed");
                ESP_LOGI(kTag, "Weather location request routed to fixed mode");
                QueueApiCall("/api/weather/location/mode/set", SerializeAndDelete(body), true,
                             std::move(completion));
                return;
            }

            cJSON* body = cJSON_CreateObject();
            cJSON_AddStringToObject(body, "location_text", location_text.c_str());
            cJSON_AddStringToObject(body, "selected_location_code", selected_location_code.c_str());
            QueueApiCall("/api/weather/location/set", SerializeAndDelete(body), true, std::move(completion));
        });

    server.AddAsyncTool(
        "self.weather.set_location_mode",
        "专门切换天气位置模式。用户提到 IP 自动定位、自动识别、根据当前位置显示天气时必须调用本工具并传 automatic；关闭自动定位且不修改城市时传 fixed。",
        PropertyList({
            Property("mode", kPropertyTypeString)
        }),
        [this](const PropertyList& properties, McpToolCompletion completion) {
            cJSON* body = cJSON_CreateObject();
            cJSON_AddStringToObject(body, "mode", properties["mode"].value<std::string>().c_str());
            QueueApiCall("/api/weather/location/mode/set", SerializeAndDelete(body), true,
                         std::move(completion));
        });

    server.AddAsyncTool(
        "self.weather.get_location",
        "查询当前天气使用固定模式还是自动识别模式，并返回当前生效的位置和 city 或 district 定位精度。",
        PropertyList(),
        [this](const PropertyList& properties, McpToolCompletion completion) {
            (void)properties;
            QueueApiCall("/api/weather/location/get", "{}", false, std::move(completion));
        });

    server.AddAsyncTool(
        "tuntun.memo.create",
        "创建备忘录。remind_at 使用带偏移的 ISO 8601 时间；没有提醒时间时传空字符串。time_zone 使用 IANA 时区，默认 Asia/Shanghai。",
        PropertyList({
            Property("content", kPropertyTypeString),
            Property("remind_at", kPropertyTypeString, std::string("")),
            Property("time_zone", kPropertyTypeString, std::string("Asia/Shanghai"))
        }),
        [this](const PropertyList& properties, McpToolCompletion completion) {
            cJSON* body = cJSON_CreateObject();
            cJSON_AddStringToObject(body, "content", properties["content"].value<std::string>().c_str());
            AddOptionalTime(body, "remind_at", properties["remind_at"].value<std::string>());
            cJSON_AddStringToObject(body, "time_zone", properties["time_zone"].value<std::string>().c_str());
            QueueApiCall("/api/memo/create", SerializeAndDelete(body), true,
                         std::move(completion), GenerateUuid());
        });

    server.AddAsyncTool(
        "tuntun.memo.list",
        "分页查询备忘录。scope 可用 today、upcoming、all；status 为 -1 表示全部、0 表示未完成、1 表示已完成。默认返回 5 条，最多 10 条。",
        PropertyList({
            Property("scope", kPropertyTypeString, std::string("upcoming")),
            Property("status", kPropertyTypeInteger, -1, -1, 1),
            Property("keyword", kPropertyTypeString, std::string("")),
            Property("page", kPropertyTypeInteger, 1, 1, 1000),
            Property("page_size", kPropertyTypeInteger, 5, 1, 10)
        }),
        [this](const PropertyList& properties, McpToolCompletion completion) {
            cJSON* body = cJSON_CreateObject();
            cJSON_AddStringToObject(body, "scope", properties["scope"].value<std::string>().c_str());
            const int status = properties["status"].value<int>();
            if (status < 0) {
                cJSON_AddNullToObject(body, "status");
            } else {
                cJSON_AddNumberToObject(body, "status", status);
            }
            cJSON_AddStringToObject(body, "keyword", properties["keyword"].value<std::string>().c_str());
            cJSON_AddNumberToObject(body, "pidx", properties["page"].value<int>());
            cJSON_AddNumberToObject(body, "psize", properties["page_size"].value<int>());
            QueueApiCall("/api/memo/list", SerializeAndDelete(body), false, std::move(completion));
        });

    server.AddAsyncTool(
        "tuntun.memo.statistics",
        "统计备忘录数量或是否存在。scope 可用 all、pending、unexpired、today、tomorrow、this_week。",
        PropertyList({Property("scope", kPropertyTypeString, std::string("pending"))}),
        [this](const PropertyList& properties, McpToolCompletion completion) {
            cJSON* body = cJSON_CreateObject();
            cJSON_AddStringToObject(body, "scope", properties["scope"].value<std::string>().c_str());
            QueueApiCall("/api/memo/statistics", SerializeAndDelete(body), false, std::move(completion));
        });

    server.AddAsyncTool(
        "tuntun.memo.match",
        "按语音关键词匹配备忘录候选。修改、完成、恢复或删除前，无法确定 memo_id 时先调用本工具并让用户确认。",
        PropertyList({
            Property("keyword", kPropertyTypeString),
            Property("status", kPropertyTypeInteger, 0, 0, 1)
        }),
        [this](const PropertyList& properties, McpToolCompletion completion) {
            cJSON* body = cJSON_CreateObject();
            cJSON_AddStringToObject(body, "keyword", properties["keyword"].value<std::string>().c_str());
            cJSON_AddNumberToObject(body, "status", properties["status"].value<int>());
            QueueApiCall("/api/memo/match", SerializeAndDelete(body), false, std::move(completion));
        });

    server.AddAsyncTool(
        "tuntun.memo.update",
        "修改已确认的备忘录。必须使用查询或匹配结果中的 memo_id 和 version；仅修改需要变化的字段。",
        PropertyList({
            Property("memo_id", kPropertyTypeString),
            Property("version", kPropertyTypeInteger, 0, 2147483647),
            Property("content", kPropertyTypeString, std::string("")),
            Property("remind_at", kPropertyTypeString, std::string("")),
            Property("time_zone", kPropertyTypeString, std::string("Asia/Shanghai")),
            Property("clear_remind_at", kPropertyTypeBoolean, false)
        }),
        [this](const PropertyList& properties, McpToolCompletion completion) {
            cJSON* body = cJSON_CreateObject();
            cJSON_AddStringToObject(body, "memo_id", properties["memo_id"].value<std::string>().c_str());
            cJSON_AddNumberToObject(body, "version", properties["version"].value<int>());
            cJSON_AddStringToObject(body, "content", properties["content"].value<std::string>().c_str());
            AddOptionalTime(body, "remind_at", properties["remind_at"].value<std::string>());
            cJSON_AddStringToObject(body, "time_zone", properties["time_zone"].value<std::string>().c_str());
            cJSON_AddBoolToObject(body, "clear_remind_at", properties["clear_remind_at"].value<bool>());
            QueueApiCall("/api/memo/update", SerializeAndDelete(body), true, std::move(completion));
        });

    auto register_version_action = [this, &server](const char* tool_name,
                                                    const char* description,
                                                    const char* endpoint) {
        server.AddAsyncTool(
            tool_name,
            description,
            PropertyList({
                Property("memo_id", kPropertyTypeString),
                Property("version", kPropertyTypeInteger, 0, 2147483647)
            }),
            [this, endpoint](const PropertyList& properties, McpToolCompletion completion) {
                cJSON* body = cJSON_CreateObject();
                cJSON_AddStringToObject(body, "memo_id", properties["memo_id"].value<std::string>().c_str());
                cJSON_AddNumberToObject(body, "version", properties["version"].value<int>());
                QueueApiCall(endpoint, SerializeAndDelete(body), true, std::move(completion));
            });
    };

    register_version_action(
        "tuntun.memo.complete",
        "将已确认的未完成备忘录标记为完成，必须传入最新 memo_id 和 version。",
        "/api/memo/complete");
    register_version_action(
        "tuntun.memo.reopen",
        "将已确认的已完成备忘录恢复为未完成，必须传入最新 memo_id 和 version。",
        "/api/memo/reopen");
    register_version_action(
        "tuntun.memo.delete",
        "软删除已确认的备忘录。删除属于不可逆语音操作，调用前必须获得用户明确确认，并传入最新 memo_id 和 version。",
        "/api/memo/delete");
}

/**
 * @brief 使用芯片硬件随机数生成 UUID v4。
 * @return 标准 8-4-4-4-12 小写十六进制字符串。
 */
std::string BackendService::GenerateUuid() {
    uint8_t bytes[16];
    esp_fill_random(bytes, sizeof(bytes));
    bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0F) | 0x40);
    bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3F) | 0x80);
    char text[37];
    snprintf(text, sizeof(text),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
             bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
    return text;
}

/**
 * @brief 序列化并释放调用方移交的 cJSON 根对象。
 * @param root 需要序列化的 JSON 根对象，可以为空。
 * @return 紧凑 JSON；分配失败时返回空对象文本。
 */
std::string BackendService::SerializeAndDelete(cJSON* root) {
    if (root == nullptr) {
        return "{}";
    }
    char* serialized = cJSON_PrintUnformatted(root);
    std::string result = serialized != nullptr ? serialized : "{}";
    if (serialized != nullptr) {
        cJSON_free(serialized);
    }
    cJSON_Delete(root);
    return result;
}
