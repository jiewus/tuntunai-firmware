/**
 * @file backend_service.cc
 * @brief 吞吞生活后端认证、串行 HTTP Worker、屏保缓存与异步 MCP 工具实现。
 */

#include "backend_service.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>

#include <esp_app_desc.h>
#include <esp_log.h>
#include <esp_random.h>
#include <mbedtls/sha256.h>

#include "application.h"
#include "audio/demuxer/ogg_demuxer.h"
#include "boards/common/board.h"
#include "display/display.h"
#include "assets/lang_config.h"
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
constexpr size_t kMqttConfigResponseLimit = 4096;
constexpr size_t kDeviceEventResponseLimit = 24 * 1024;
constexpr size_t kReminderResponseLimit = 24 * 1024;
constexpr size_t kReminderAudioMetadataResponseLimit = 4096;
constexpr size_t kDynamicManifestResponseLimit = 20 * 1024;
constexpr size_t kMaximumDynamicTools = 10;
constexpr size_t kMaximumDynamicSchemaBytes = 4096;
constexpr size_t kMaximumDynamicDescriptionBytes = 1024;
constexpr size_t kMaximumDynamicExecutionRequestBytes = 12 * 1024;
constexpr size_t kMaximumReminderPlans = 8;
constexpr size_t kMaximumReminderTextBytes = 512;
constexpr size_t kMaximumReminderAudioBytes = 4 * 1024 * 1024;
constexpr size_t kAudioReadBufferBytes = 1024;
constexpr UBaseType_t kWorkerPriority = tskIDLE_PRIORITY;
constexpr uint32_t kWorkerStackSize = 8 * 1024;
constexpr UBaseType_t kQueueLength = 12;
constexpr TickType_t kWeatherRefreshInterval = pdMS_TO_TICKS(30 * 60 * 1000);
constexpr TickType_t kMemoRefreshInterval = pdMS_TO_TICKS(5 * 60 * 1000);
constexpr TickType_t kEventSyncInterval = pdMS_TO_TICKS(30 * 60 * 1000);
constexpr TickType_t kReminderSyncInterval = pdMS_TO_TICKS(30 * 60 * 1000);

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
 * @brief 从 JSON 对象读取非负 64 位整数。
 * @param object 字段所属对象。
 * @param name 字段名称。
 * @param value 成功时写入转换后的整数。
 * @return 字段为有限、非负且未超过 uint64_t 的数字时返回 true。
 */
bool GetJsonUInt64(const cJSON* object, const char* name, uint64_t& value) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsNumber(item) || item->valuedouble < 0
        || item->valuedouble > static_cast<double>(std::numeric_limits<uint64_t>::max())) {
        return false;
    }
    value = static_cast<uint64_t>(item->valuedouble);
    return true;
}

/**
 * @brief 将公历日期换算为相对 1970-01-01 的天数。
 * @param year 完整公历年份。
 * @param month 1 至 12 月。
 * @param day 1 至当月最大日。
 * @return 可直接用于计算 Unix 时间戳的有符号天数。
 */
int64_t DaysFromCivil(int year, unsigned month, unsigned day) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned year_of_era = static_cast<unsigned>(year - era * 400);
    const int adjusted_month = static_cast<int>(month) + (month > 2 ? -3 : 9);
    const unsigned day_of_year = static_cast<unsigned>((153 * adjusted_month + 2) / 5)
        + day - 1;
    const unsigned day_of_era = year_of_era * 365 + year_of_era / 4
        - year_of_era / 100 + day_of_year;
    return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(day_of_era) - 719468;
}

/**
 * @brief 判断公历日期字段组合是否合法。
 * @param year 完整年份。
 * @param month 月份。
 * @param day 日期。
 * @return 日期存在时返回 true。
 */
bool IsValidCivilDate(int year, int month, int day) {
    static constexpr int kDaysPerMonth[] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };
    if (year < 1970 || year > 2100 || month < 1 || month > 12 || day < 1) {
        return false;
    }
    int maximum_day = kDaysPerMonth[month - 1];
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    if (month == 2 && leap) {
        maximum_day = 29;
    }
    return day <= maximum_day;
}

/**
 * @brief 校验动态工具和参数使用的稳定 ASCII 名称。
 * @param value 待校验名称。
 * @return 长度为1至64、以小写字母开头且仅含小写字母、数字、下划线、点和短横线时返回 true。
 */
bool IsStableDynamicName(const std::string& value) {
    if (value.empty() || value.size() > 64 || value.front() < 'a' || value.front() > 'z') {
        return false;
    }
    return std::all_of(value.begin() + 1, value.end(), [](unsigned char character) {
        return (character >= 'a' && character <= 'z')
            || (character >= '0' && character <= '9')
            || character == '_' || character == '.' || character == '-';
    });
}

/**
 * @brief 校验标准连字符 UUID 文本的结构。
 * @param value 期望为 8-4-4-4-12 格式的 ASCII UUID。
 * @return 长度、连字符位置和十六进制字符均有效时返回 true。
 */
bool IsUuidText(const std::string& value) {
    if (value.size() != 36) {
        return false;
    }
    for (size_t index = 0; index < value.size(); ++index) {
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (value[index] != '-') {
                return false;
            }
            continue;
        }
        const unsigned char character = static_cast<unsigned char>(value[index]);
        if (!std::isxdigit(character)) {
            return false;
        }
    }
    return true;
}

/**
 * @brief 判断字段名是否出现在 JSON Schema required 数组中。
 * @param required 必须为字符串数组。
 * @param name 需要查找的属性名。
 * @return 数组包含完全相同字符串时返回 true。
 */
bool IsRequiredSchemaProperty(const cJSON* required, const std::string& name) {
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, required) {
        if (cJSON_IsString(item) && item->valuestring != nullptr && name == item->valuestring) {
            return true;
        }
    }
    return false;
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
 * @brief 串行处理显式作业，并检查屏保刷新、MQTT 补偿和本地提醒截止时间。
 * @details Tick 比较使用有符号差值，能够正确处理 FreeRTOS tick 计数回绕。
 */
void BackendService::WorkerLoop() {
    while (true) {
        Job* raw_job = nullptr;
        if (xQueueReceive(queue_, &raw_job, pdMS_TO_TICKS(500)) == pdTRUE && raw_job != nullptr) {
            std::unique_ptr<Job> job(raw_job);
            ProcessJob(*job);
        }

        if (mqtt_stop_requested_.exchange(false)) {
            business_mqtt_.Stop();
        }
        if (mqtt_restart_requested_.load() && network_connected_.load()
            && Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
            mqtt_restart_requested_.store(false);
            RefreshMqttConfig();
            next_event_sync_ = 0;
        }

        bool mqtt_requested_sync = business_mqtt_.ConsumeConnectedSignal();
        BackendEventHint hint;
        while (business_mqtt_.TryPop(hint)) {
            mqtt_requested_sync = true;
            ESP_LOGI(kTag, "MQTT event hint type=%d revision=%llu",
                     static_cast<int>(hint.type),
                     static_cast<unsigned long long>(hint.revision));
        }

        const TickType_t now = xTaskGetTickCount();
        if (network_connected_.load()
            && (mqtt_requested_sync || next_event_sync_ == 0
                || static_cast<int32_t>(now - next_event_sync_) >= 0)) {
            SyncDeviceEvents();
            next_event_sync_ = now + kEventSyncInterval;
        }
        if (network_connected_.load()
            && (next_reminder_sync_ == 0
                || static_cast<int32_t>(now - next_reminder_sync_) >= 0)) {
            SyncReminders();
            next_reminder_sync_ = now + kReminderSyncInterval;
        }

        ProcessDueReminder();

        if (!screensaver_active_.load() || !network_connected_.load()) {
            continue;
        }

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
        if (EnsureActivated()) {
            SyncDynamicMcpManifest();
            RefreshMqttConfig();
            SyncDeviceEvents();
            SyncReminders();
            business_mqtt_.ConsumeConnectedSignal();
            const TickType_t now = xTaskGetTickCount();
            next_event_sync_ = now + kEventSyncInterval;
            next_reminder_sync_ = now + kReminderSyncInterval;
        }
        return;
    }

    if (job.type == JobType::MqttConfigRefresh) {
        RefreshMqttConfig();
        return;
    }

    if (job.type == JobType::DeviceEventSync) {
        SyncDeviceEvents();
        next_event_sync_ = xTaskGetTickCount() + kEventSyncInterval;
        return;
    }

    if (job.type == JobType::ReminderSync) {
        SyncReminders();
        next_reminder_sync_ = xTaskGetTickCount() + kReminderSyncInterval;
        return;
    }

    if (job.type == JobType::WeatherRefresh) {
        /* 屏保读请求失败后等待下次周期，不使用长退避阻塞主动提醒截止时间。 */
        HttpResult result = PostAuthenticated(
            "/api/weather/get",
            "{}",
            kWeatherResponseLimit);
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
        /* 屏保读请求失败后等待下次周期，不使用长退避阻塞主动提醒截止时间。 */
        HttpResult result = PostAuthenticated(
            "/api/memo/screensaver/get",
            "{}",
            kMemoResponseLimit);
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
    mqtt_stop_requested_.store(false);
    next_event_sync_ = 0;
    next_reminder_sync_ = 0;
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
    mqtt_stop_requested_.store(true);
    reminder_playback_cancelled_.store(true);
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
 * @brief 通过原子标志中断当前提醒的网络读取和音频入队。
 * @return 调用时存在活动提醒返回 true。
 */
bool BackendService::CancelActiveReminderPlayback() {
    const bool active = reminder_playback_active_.load();
    if (active) {
        reminder_playback_cancelled_.store(true);
    }
    return active;
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
 * @brief 对周期业务请求应用一次未授权凭据重签，不执行网络错误退避。
 * @param endpoint 业务接口相对路径。
 * @param body JSON 请求体。
 * @param response_limit 最大响应体字节数。
 * @return 首次调用或重新激活后重放的结果。
 */
BackendService::HttpResult BackendService::PostAuthenticated(
    const std::string& endpoint,
    const std::string& body,
    size_t response_limit) {
    if (!EnsureActivated()) {
        HttpResult result;
        result.business_code = 2001;
        result.message = "设备业务服务尚未激活";
        return result;
    }

    HttpResult result = Post(endpoint, body, response_limit, "", true);
    if (result.business_code == 2001) {
        ClearCredentials();
        if (Activate()) {
            result = Post(endpoint, body, response_limit, "", true);
        }
    }
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
 * @brief 拉取短期业务 MQTT 凭据，并建立与小智协议隔离的订阅连接。
 * @return 配置被正确处理时返回 true。
 */
bool BackendService::RefreshMqttConfig() {
    if (!EnsureActivated()) {
        return false;
    }
    HttpResult result = PostAuthenticated(
        "/api/mqtt/device/config/get",
        "{}",
        kMqttConfigResponseLimit);
    if (!result.success) {
        ESP_LOGW(kTag, "Failed to refresh business MQTT config, code=%d", result.business_code);
        return false;
    }

    cJSON* root = cJSON_Parse(result.response_body.c_str());
    const cJSON* data = root == nullptr
        ? nullptr
        : cJSON_GetObjectItemCaseSensitive(root, "data");
    const cJSON* enabled = cJSON_GetObjectItemCaseSensitive(data, "enabled");
    if (!cJSON_IsBool(enabled)) {
        cJSON_Delete(root);
        ESP_LOGW(kTag, "Business MQTT config response is incomplete");
        return false;
    }

    BackendMqttConfig config;
    config.enabled = cJSON_IsTrue(enabled);
    if (!config.enabled) {
        business_mqtt_.Stop();
        mqtt_restart_requested_.store(false);
        cJSON_Delete(root);
        return true;
    }

    const cJSON* port = cJSON_GetObjectItemCaseSensitive(data, "port");
    const cJSON* tls = cJSON_GetObjectItemCaseSensitive(data, "tls");
    const cJSON* qos = cJSON_GetObjectItemCaseSensitive(data, "qos");
    config.host = GetJsonString(data, "host");
    config.client_id = GetJsonString(data, "client_id");
    config.username = GetJsonString(data, "username");
    config.password = GetJsonString(data, "password");
    config.event_topic = GetJsonString(data, "event_topic");
    config.port = cJSON_IsNumber(port) ? port->valueint : 0;
    config.tls = cJSON_IsBool(tls) && cJSON_IsTrue(tls);
    config.qos = cJSON_IsNumber(qos) ? qos->valueint : 1;
    cJSON_Delete(root);
    const bool started = business_mqtt_.Start(config);
    if (started) {
        mqtt_restart_requested_.store(false);
    }
    return started;
}

/**
 * @brief 通过 HTTPS 读取事件箱，处理支持的类型并逐条完成确认。
 * @details 单页最多 8 条、单轮最多 4 页，防止异常积压长期独占后台 Worker。
 */
void BackendService::SyncDeviceEvents() {
    if (!EnsureActivated() || !network_connected_.load()) {
        return;
    }

    for (int page = 0; page < 4; ++page) {
        HttpResult result = PostAuthenticated(
            "/api/device-event/sync",
            "{\"limit\":8}",
            kDeviceEventResponseLimit);
        if (!result.success) {
            ESP_LOGW(kTag, "Device event compensation sync failed, code=%d", result.business_code);
            return;
        }

        cJSON* root = cJSON_Parse(result.response_body.c_str());
        const cJSON* data = root == nullptr
            ? nullptr
            : cJSON_GetObjectItemCaseSensitive(root, "data");
        const cJSON* events = cJSON_GetObjectItemCaseSensitive(data, "events");
        const cJSON* has_more = cJSON_GetObjectItemCaseSensitive(data, "has_more");
        if (!cJSON_IsArray(events) || !cJSON_IsBool(has_more)) {
            cJSON_Delete(root);
            ESP_LOGW(kTag, "Device event sync response is incomplete");
            return;
        }

        std::vector<std::string> completed_event_ids;
        std::vector<std::string> reminder_event_ids;
        std::vector<std::string> mqtt_config_event_ids;
        std::vector<std::string> tool_manifest_event_ids;
        std::vector<std::string> unsupported_event_ids;
        completed_event_ids.reserve(static_cast<size_t>(cJSON_GetArraySize(events)));
        cJSON* event = nullptr;
        cJSON_ArrayForEach(event, events) {
            const std::string event_id = GetJsonString(event, "event_id");
            const std::string type = GetJsonString(event, "type");
            if (event_id.size() != 36 || type.empty()) {
                continue;
            }

            if (type == "voice.reminder" || type == "reminder.schedule_changed") {
                reminder_event_ids.push_back(event_id);
            } else if (type == "data.refresh") {
                next_weather_refresh_ = xTaskGetTickCount();
                next_memo_refresh_ = xTaskGetTickCount();
                completed_event_ids.push_back(event_id);
            } else if (type == "device.config_changed") {
                mqtt_config_event_ids.push_back(event_id);
            } else if (type == "tool.manifest_changed") {
                tool_manifest_event_ids.push_back(event_id);
            } else if (type == "screen.notification" || type == "workflow.result") {
                const cJSON* payload = cJSON_GetObjectItemCaseSensitive(event, "payload");
                std::string content = GetJsonString(payload, "content");
                if (!content.empty()) {
                    if (content.size() > kMaximumReminderTextBytes) {
                        content.resize(kMaximumReminderTextBytes);
                    }
                    Application::GetInstance().Schedule([content = std::move(content)]() {
                        Board::GetInstance().GetDisplay()->ShowNotification(content.c_str(), 8000);
                    });
                    completed_event_ids.push_back(event_id);
                } else {
                    unsupported_event_ids.push_back(event_id);
                }
            } else {
                /*
                 * 当前固件尚未消费的事件必须明确标记 Failed。若一直保留 Pending，
                 * 该事件会占据同步首页并阻止更晚的主动提醒得到补偿。
                 */
                unsupported_event_ids.push_back(event_id);
            }
        }
        const bool more = cJSON_IsTrue(has_more);
        cJSON_Delete(root);

        if (!reminder_event_ids.empty() && SyncReminders()) {
            next_reminder_sync_ = xTaskGetTickCount() + kReminderSyncInterval;
            completed_event_ids.insert(
                completed_event_ids.end(),
                reminder_event_ids.begin(),
                reminder_event_ids.end());
        }
        if (!mqtt_config_event_ids.empty() && RefreshMqttConfig()) {
            completed_event_ids.insert(
                completed_event_ids.end(),
                mqtt_config_event_ids.begin(),
                mqtt_config_event_ids.end());
        }
        if (!tool_manifest_event_ids.empty()) {
            const bool reconnect_mqtt = business_mqtt_.IsConnected();
            if (reconnect_mqtt) {
                business_mqtt_.Stop();
            }
            if (SyncDynamicMcpManifest()) {
                completed_event_ids.insert(
                    completed_event_ids.end(),
                    tool_manifest_event_ids.begin(),
                    tool_manifest_event_ids.end());
            }
            if (reconnect_mqtt && network_connected_.load()) {
                mqtt_restart_requested_.store(true);
            }
        }

        for (const std::string& event_id : completed_event_ids) {
            AcknowledgeDeviceEvent(event_id);
        }
        for (const std::string& event_id : unsupported_event_ids) {
            AcknowledgeDeviceEvent(event_id, 4, "unsupported_firmware_event");
        }
        const size_t terminal_count = completed_event_ids.size() + unsupported_event_ids.size();
        if (!more || terminal_count == 0) {
            break;
        }
    }
}

/**
 * @brief 将设备事件单向推进到 Completed 或 Failed 状态。
 * @param event_id 已处理事件 UUID。
 * @param status DeviceEventStatusEnum 数字值：3 为 Completed，4 为 Failed。
 * @param error_code Failed 状态要求的安全机器码；Completed 时为空。
 */
void BackendService::AcknowledgeDeviceEvent(
    const std::string& event_id,
    int status,
    const char* error_code) {
    cJSON* body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "event_id", event_id.c_str());
    cJSON_AddNumberToObject(body, "status", status);
    if (error_code == nullptr || *error_code == '\0') {
        cJSON_AddNullToObject(body, "internal_error_code");
    } else {
        cJSON_AddStringToObject(body, "internal_error_code", error_code);
    }
    const std::string request_body = SerializeAndDelete(body);
    HttpResult result = PostAuthenticated(
        "/api/device-event/acknowledge",
        request_body,
        4096);
    if (!result.success) {
        ESP_LOGW(kTag, "Device event acknowledgement failed, code=%d", result.business_code);
    }
}

/**
 * @brief 获取提醒设置和未来计划，并用最多 8 项的有界快照替换本地缓存。
 * @return 完整响应解析成功时返回 true。
 */
bool BackendService::SyncReminders() {
    if (!EnsureActivated() || !network_connected_.load()) {
        return false;
    }
    HttpResult result = PostAuthenticated(
        "/api/reminder/sync",
        "{\"window_hours\":24}",
        kReminderResponseLimit);
    if (!result.success) {
        ESP_LOGW(kTag, "Reminder sync failed, code=%d", result.business_code);
        return false;
    }

    cJSON* root = cJSON_Parse(result.response_body.c_str());
    const cJSON* data = root == nullptr
        ? nullptr
        : cJSON_GetObjectItemCaseSensitive(root, "data");
    const cJSON* setting = cJSON_GetObjectItemCaseSensitive(data, "setting");
    const cJSON* reminders = cJSON_GetObjectItemCaseSensitive(data, "reminders");
    const cJSON* reminders_enabled = cJSON_GetObjectItemCaseSensitive(setting, "reminders_enabled");
    const cJSON* voice_enabled = cJSON_GetObjectItemCaseSensitive(setting, "voice_enabled");
    const cJSON* screen_enabled = cJSON_GetObjectItemCaseSensitive(setting, "screen_enabled");
    const cJSON* snooze_minutes = cJSON_GetObjectItemCaseSensitive(setting, "default_snooze_minutes");
    if (!cJSON_IsArray(reminders) || !cJSON_IsBool(reminders_enabled)
        || !cJSON_IsBool(voice_enabled) || !cJSON_IsBool(screen_enabled)
        || !cJSON_IsNumber(snooze_minutes)) {
        cJSON_Delete(root);
        ESP_LOGW(kTag, "Reminder sync response is incomplete");
        return false;
    }

    BackendReminderSetting parsed_setting;
    parsed_setting.reminders_enabled = cJSON_IsTrue(reminders_enabled);
    parsed_setting.voice_enabled = cJSON_IsTrue(voice_enabled);
    parsed_setting.screen_enabled = cJSON_IsTrue(screen_enabled);
    parsed_setting.default_snooze_minutes = static_cast<uint16_t>(
        std::clamp(snooze_minutes->valueint, 1, 1440));

    std::vector<BackendReminderPlan> parsed_plans;
    parsed_plans.reserve(std::min(
        static_cast<size_t>(cJSON_GetArraySize(reminders)),
        kMaximumReminderPlans));
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, reminders) {
        if (parsed_plans.size() >= kMaximumReminderPlans) {
            break;
        }

        BackendReminderPlan plan;
        plan.reminder_id = GetJsonString(item, "reminder_id");
        plan.display_text = GetJsonString(item, "display_text");
        if (plan.display_text.empty()) {
            plan.display_text = GetJsonString(item, "content");
        }
        plan.audio_asset_id = GetJsonString(item, "audio_asset_id");
        const std::string trigger_at = GetJsonString(item, "trigger_at_utc");
        const std::string expires_at = GetJsonString(item, "expires_at");
        const cJSON* priority = cJSON_GetObjectItemCaseSensitive(item, "priority");
        const cJSON* status = cJSON_GetObjectItemCaseSensitive(item, "status");
        const cJSON* audio_status = cJSON_GetObjectItemCaseSensitive(item, "audio_status");
        if (plan.reminder_id.size() != 36 || plan.display_text.empty()
            || plan.display_text.size() > kMaximumReminderTextBytes
            || !GetJsonUInt64(item, "version", plan.version)
            || !ParseUtcTimestamp(trigger_at.c_str(), plan.trigger_at_utc)
            || !ParseUtcTimestamp(expires_at.c_str(), plan.expires_at_utc)
            || plan.expires_at_utc <= plan.trigger_at_utc
            || !cJSON_IsNumber(priority) || priority->valueint < 0 || priority->valueint > 3
            || !cJSON_IsNumber(status) || (status->valueint != 0 && status->valueint != 1)
            || !cJSON_IsNumber(audio_status)
            || audio_status->valueint < 0 || audio_status->valueint > 4) {
            ESP_LOGW(kTag, "Skipped invalid reminder plan");
            continue;
        }
        plan.priority = static_cast<uint8_t>(priority->valueint);
        plan.audio_status = static_cast<uint8_t>(audio_status->valueint);
        if (!plan.audio_asset_id.empty() && plan.audio_asset_id.size() != 36) {
            plan.audio_asset_id.clear();
        }
        parsed_plans.push_back(std::move(plan));
    }
    cJSON_Delete(root);

    std::sort(parsed_plans.begin(), parsed_plans.end(), [](const auto& left, const auto& right) {
        if (left.trigger_at_utc != right.trigger_at_utc) {
            return left.trigger_at_utc < right.trigger_at_utc;
        }
        return left.priority > right.priority;
    });
    const size_t parsed_plan_count = parsed_plans.size();
    {
        std::lock_guard<std::mutex> lock(reminder_mutex_);
        reminder_setting_ = parsed_setting;
        reminder_plans_ = std::move(parsed_plans);
    }
    ESP_LOGI(kTag, "Reminder snapshot updated, count=%u",
             static_cast<unsigned>(parsed_plan_count));
    return true;
}

/**
 * @brief 获取并应用设备当前授权的动态 MCP 工具清单。
 * @return 清单协议有效且未变化或已排入主任务替换时返回 true。
 */
bool BackendService::SyncDynamicMcpManifest() {
    if (!EnsureActivated() || !network_connected_.load()) {
        return false;
    }

    cJSON* request = cJSON_CreateObject();
    cJSON_AddNumberToObject(request, "schema_version", 1);
    cJSON_AddNumberToObject(
        request,
        "local_revision",
        dynamic_manifest_loaded_ ? static_cast<double>(dynamic_manifest_revision_) : 0);
    HttpResult result = PostAuthenticated(
        "/api/mcp-tool/manifest/get",
        SerializeAndDelete(request),
        kDynamicManifestResponseLimit);
    if (!result.success) {
        ESP_LOGW(kTag, "Dynamic MCP manifest sync failed, code=%d", result.business_code);
        return false;
    }

    cJSON* root = cJSON_Parse(result.response_body.c_str());
    const cJSON* data = root == nullptr
        ? nullptr
        : cJSON_GetObjectItemCaseSensitive(root, "data");
    const std::string schema_version = GetJsonString(data, "schema_version");
    const cJSON* changed = cJSON_GetObjectItemCaseSensitive(data, "changed");
    uint64_t revision = 0;
    if (schema_version != "1.0" || !cJSON_IsBool(changed)
        || !GetJsonUInt64(data, "revision", revision)) {
        cJSON_Delete(root);
        ESP_LOGW(kTag, "Dynamic MCP manifest response is incomplete");
        return false;
    }
    if (!cJSON_IsTrue(changed)) {
        dynamic_manifest_revision_ = revision;
        dynamic_manifest_loaded_ = true;
        cJSON_Delete(root);
        return true;
    }

    const std::string manifest_hash = GetJsonString(data, "manifest_hash");
    const cJSON* tools = cJSON_GetObjectItemCaseSensitive(data, "tools");
    const bool hash_valid = manifest_hash.size() == 64
        && std::all_of(manifest_hash.begin(), manifest_hash.end(), [](unsigned char character) {
            return (character >= '0' && character <= '9')
                || (character >= 'a' && character <= 'f');
        });
    if (!hash_valid || !cJSON_IsArray(tools)
        || cJSON_GetArraySize(tools) > static_cast<int>(kMaximumDynamicTools)) {
        cJSON_Delete(root);
        ESP_LOGW(kTag, "Dynamic MCP manifest exceeds firmware limits");
        return false;
    }

    std::vector<McpDynamicToolDefinition> definitions;
    std::vector<std::string> names;
    definitions.reserve(static_cast<size_t>(cJSON_GetArraySize(tools)));
    names.reserve(definitions.capacity());
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, tools) {
        const std::string tool_name = GetJsonString(item, "tool_name");
        const std::string tool_version_id = GetJsonString(item, "tool_version_id");
        const std::string description = GetJsonString(item, "description");
        const cJSON* version_no = cJSON_GetObjectItemCaseSensitive(item, "version_no");
        const cJSON* input_schema = cJSON_GetObjectItemCaseSensitive(item, "input_schema");
        if (!IsStableDynamicName(tool_name) || tool_name.rfind("self.", 0) == 0
            || !IsUuidText(tool_version_id) || !cJSON_IsNumber(version_no)
            || version_no->valuedouble < 1
            || std::floor(version_no->valuedouble) != version_no->valuedouble
            || version_no->valuedouble > std::numeric_limits<uint32_t>::max()
            || description.empty()
            || description.size() > kMaximumDynamicDescriptionBytes
            || std::find(names.begin(), names.end(), tool_name) != names.end()) {
            cJSON_Delete(root);
            ESP_LOGW(kTag, "Dynamic MCP tool metadata is invalid");
            return false;
        }

        PropertyList properties;
        std::string input_schema_json;
        if (!ParseDynamicMcpSchema(input_schema, properties, input_schema_json)) {
            cJSON_Delete(root);
            ESP_LOGW(kTag, "Dynamic MCP tool schema is invalid: %s", tool_name.c_str());
            return false;
        }

        McpDynamicToolDefinition definition;
        definition.name = tool_name;
        definition.description = description;
        definition.properties = std::move(properties);
        definition.input_schema_json = std::move(input_schema_json);
        definition.callback = [this, tool_name, tool_version_id](
                                  const PropertyList& call_properties,
                                  McpToolCompletion completion) {
            QueueDynamicMcpExecution(
                tool_name,
                tool_version_id,
                call_properties,
                std::move(completion));
        };
        names.push_back(tool_name);
        definitions.push_back(std::move(definition));
    }
    cJSON_Delete(root);
    result.response_body.clear();

    /*
     * MCP 工具容器只能从应用主任务替换。definitions 的所有权移动到调度闭包，
     * 后端 Worker 不再保留 Schema 副本，控制常驻内存只存在一份。
     */
    Application::GetInstance().Schedule([definitions = std::move(definitions)]() mutable {
        McpServer::GetInstance().ReplaceDynamicTools(std::move(definitions));
    });
    dynamic_manifest_revision_ = revision;
    dynamic_manifest_loaded_ = true;
    ESP_LOGI(kTag, "Dynamic MCP manifest accepted, revision=%llu count=%u",
             static_cast<unsigned long long>(revision),
             static_cast<unsigned>(names.size()));
    return true;
}

/**
 * @brief 解析平台受限 JSON Schema，保留完整 Schema 并生成基础类型校验列表。
 * @param schema input_schema 根对象。
 * @param properties 输出参数列表。
 * @param schema_json 输出紧凑 Schema 文本。
 * @return object 结构、名称、类型、默认值和整数范围均有效时返回 true。
 */
bool BackendService::ParseDynamicMcpSchema(
    const cJSON* schema,
    PropertyList& properties,
    std::string& schema_json) {
    if (!cJSON_IsObject(schema)) {
        return false;
    }
    const std::string root_type = GetJsonString(schema, "type");
    const cJSON* schema_properties = cJSON_GetObjectItemCaseSensitive(schema, "properties");
    const cJSON* required = cJSON_GetObjectItemCaseSensitive(schema, "required");
    const cJSON* additional = cJSON_GetObjectItemCaseSensitive(schema, "additionalProperties");
    if (root_type != "object" || !cJSON_IsObject(schema_properties)
        || !cJSON_IsArray(required) || !cJSON_IsBool(additional) || cJSON_IsTrue(additional)
        || cJSON_GetArraySize(schema_properties) > 10
        || cJSON_GetArraySize(required) > cJSON_GetArraySize(schema_properties)) {
        return false;
    }

    std::vector<std::string> property_names;
    property_names.reserve(static_cast<size_t>(cJSON_GetArraySize(schema_properties)));
    cJSON* schema_property = nullptr;
    cJSON_ArrayForEach(schema_property, schema_properties) {
        const std::string name = schema_property->string == nullptr
            ? std::string()
            : std::string(schema_property->string);
        const std::string type = GetJsonString(schema_property, "type");
        if (!IsStableDynamicName(name)
            || (type != "string" && type != "integer" && type != "boolean")
            || std::find(property_names.begin(), property_names.end(), name) != property_names.end()) {
            return false;
        }

        const bool is_required = IsRequiredSchemaProperty(required, name);
        const cJSON* default_value = cJSON_GetObjectItemCaseSensitive(schema_property, "default");
        if (is_required && default_value != nullptr) {
            return false;
        }

        PropertyType property_type = kPropertyTypeString;
        if (type == "integer") {
            property_type = kPropertyTypeInteger;
        } else if (type == "boolean") {
            property_type = kPropertyTypeBoolean;
        }

        Property property(name, property_type);
        if (default_value != nullptr) {
            if (property_type == kPropertyTypeString && cJSON_IsString(default_value)
                && default_value->valuestring != nullptr) {
                property = Property(name, property_type, std::string(default_value->valuestring));
            } else if (property_type == kPropertyTypeInteger && cJSON_IsNumber(default_value)
                       && std::floor(default_value->valuedouble) == default_value->valuedouble
                       && default_value->valuedouble >= std::numeric_limits<int>::min()
                       && default_value->valuedouble <= std::numeric_limits<int>::max()) {
                property = Property(name, property_type, static_cast<int>(default_value->valuedouble));
            } else if (property_type == kPropertyTypeBoolean && cJSON_IsBool(default_value)) {
                property = Property(name, property_type, cJSON_IsTrue(default_value) != 0);
            } else {
                return false;
            }
        }
        property.set_required(is_required);

        if (property_type == kPropertyTypeInteger) {
            const cJSON* minimum = cJSON_GetObjectItemCaseSensitive(schema_property, "minimum");
            const cJSON* maximum = cJSON_GetObjectItemCaseSensitive(schema_property, "maximum");
            if (minimum != nullptr) {
                if (!cJSON_IsNumber(minimum) || std::floor(minimum->valuedouble) != minimum->valuedouble
                    || minimum->valuedouble < std::numeric_limits<int>::min()
                    || minimum->valuedouble > std::numeric_limits<int>::max()) {
                    return false;
                }
                property.set_minimum(static_cast<int>(minimum->valuedouble));
            }
            if (maximum != nullptr) {
                if (!cJSON_IsNumber(maximum) || std::floor(maximum->valuedouble) != maximum->valuedouble
                    || maximum->valuedouble < std::numeric_limits<int>::min()
                    || maximum->valuedouble > std::numeric_limits<int>::max()) {
                    return false;
                }
                property.set_maximum(static_cast<int>(maximum->valuedouble));
            }
            if (minimum != nullptr && maximum != nullptr
                && minimum->valuedouble > maximum->valuedouble) {
                return false;
            }
            if (default_value != nullptr) {
                const int default_integer = static_cast<int>(default_value->valuedouble);
                if ((minimum != nullptr && default_integer < static_cast<int>(minimum->valuedouble))
                    || (maximum != nullptr && default_integer > static_cast<int>(maximum->valuedouble))) {
                    return false;
                }
            }
        }

        property_names.push_back(name);
        properties.AddProperty(property);
    }

    std::vector<std::string> required_names;
    cJSON* required_item = nullptr;
    cJSON_ArrayForEach(required_item, required) {
        if (!cJSON_IsString(required_item) || required_item->valuestring == nullptr) {
            return false;
        }
        const std::string name = required_item->valuestring;
        if (std::find(property_names.begin(), property_names.end(), name) == property_names.end()
            || std::find(required_names.begin(), required_names.end(), name) != required_names.end()) {
            return false;
        }
        required_names.push_back(name);
    }

    char* serialized = cJSON_PrintUnformatted(schema);
    if (serialized == nullptr) {
        return false;
    }
    schema_json = serialized;
    cJSON_free(serialized);
    return !schema_json.empty() && schema_json.size() <= kMaximumDynamicSchemaBytes;
}

/**
 * @brief 把动态工具参数包装为固定版本代理执行请求，并标准化 MCP 返回文本。
 * @param tool_name 权威工具名称。
 * @param tool_version_id 权威不可变版本 UUID。
 * @param properties 已由 MCP 框架填入调用值和默认值的参数列表。
 * @param completion 原 JSON-RPC 调用完成回调。
 */
void BackendService::QueueDynamicMcpExecution(
    const std::string& tool_name,
    const std::string& tool_version_id,
    const PropertyList& properties,
    McpToolCompletion completion) {
    cJSON* body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "tool_name", tool_name.c_str());
    cJSON_AddStringToObject(body, "tool_version_id", tool_version_id.c_str());
    cJSON* arguments = cJSON_CreateObject();
    for (const Property& property : properties) {
        if (!property.has_value()) {
            continue;
        }
        if (property.type() == kPropertyTypeBoolean) {
            cJSON_AddBoolToObject(arguments, property.name().c_str(), property.value<bool>());
        } else if (property.type() == kPropertyTypeInteger) {
            cJSON_AddNumberToObject(arguments, property.name().c_str(), property.value<int>());
        } else if (property.type() == kPropertyTypeString) {
            cJSON_AddStringToObject(
                arguments,
                property.name().c_str(),
                property.value<std::string>().c_str());
        }
    }
    cJSON_AddItemToObject(body, "arguments", arguments);

    std::string request_body = SerializeAndDelete(body);
    if (request_body.size() > kMaximumDynamicExecutionRequestBytes) {
        completion({"动态工具参数超过设备允许大小", true});
        return;
    }

    QueueApiCall(
        "/api/mcp-tool/execute",
        std::move(request_body),
        false,
        [completion = std::move(completion)](McpToolResult api_result) mutable {
            if (api_result.is_error) {
                completion(std::move(api_result));
                return;
            }
            cJSON* root = cJSON_Parse(api_result.text.c_str());
            const cJSON* data = root == nullptr
                ? nullptr
                : cJSON_GetObjectItemCaseSensitive(root, "data");
            const cJSON* status = cJSON_GetObjectItemCaseSensitive(data, "status");
            std::string content = GetJsonString(data, "content");
            if (!cJSON_IsNumber(status) || (status->valueint != 1 && status->valueint != 2)
                || (status->valueint == 1 && content.empty())) {
                cJSON_Delete(root);
                completion({"动态服务返回格式无效", true});
                return;
            }
            const bool succeeded = status->valueint == 1;
            if (!succeeded && content.empty()) {
                content = "服务暂时不可用，请稍后再试。";
            }
            cJSON_Delete(root);
            completion({std::move(content), !succeeded});
        });
}

/**
 * @brief 在设备空闲时认领最早到期提醒，过期计划直接从本地快照移除。
 */
void BackendService::ProcessDueReminder() {
    if (reminder_playback_active_.load()) {
        return;
    }
    const std::time_t now = std::time(nullptr);
    if (now < 1704067200) {
        // 系统时间早于 2024-01-01 时视为尚未完成 SNTP/OTA 校时。
        return;
    }

    BackendReminderPlan due;
    bool has_due = false;
    {
        std::lock_guard<std::mutex> lock(reminder_mutex_);
        reminder_plans_.erase(
            std::remove_if(
                reminder_plans_.begin(),
                reminder_plans_.end(),
                [now](const BackendReminderPlan& plan) {
                    return plan.expires_at_utc <= now;
                }),
            reminder_plans_.end());
        if (!reminder_setting_.reminders_enabled || reminder_plans_.empty()
            || reminder_plans_.front().trigger_at_utc > now) {
            return;
        }
        if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
            // 对话、连接、升级或配网期间保留计划，下一轮 Worker 再检查宽限期。
            return;
        }
        due = reminder_plans_.front();
        reminder_plans_.erase(reminder_plans_.begin());
        has_due = true;
    }

    if (has_due) {
        DeliverReminder(due);
    }
}

/**
 * @brief 展示并播放一条提醒，随后向后端提交单向投递状态。
 * @param reminder 已从未来快照移除的当前提醒副本。
 */
void BackendService::DeliverReminder(const BackendReminderPlan& reminder) {
    BackendReminderSetting setting;
    {
        std::lock_guard<std::mutex> lock(reminder_mutex_);
        setting = reminder_setting_;
    }
    if (!setting.reminders_enabled) {
        return;
    }

    reminder_playback_cancelled_.store(false);
    reminder_playback_active_.store(true);
    if (setting.screen_enabled) {
        const std::string display_text = reminder.display_text;
        Application::GetInstance().Schedule([display_text]() {
            auto& board = Board::GetInstance();
            board.EnterScreensaver();
            if (board.IsScreensaverActive()) {
                board.GetDisplay()->SetScreensaverMemos({display_text});
            } else {
                board.GetDisplay()->ShowNotification(display_text.c_str(), 10000);
            }
        });
    }

    AcknowledgeReminderDelivery(reminder, 1, 0, nullptr);
    bool delivered = !setting.voice_enabled;
    uint32_t playback_duration_ms = 0;
    const bool reconnect_business_mqtt = setting.voice_enabled
        && reminder.audio_status == 2
        && !reminder.audio_asset_id.empty()
        && business_mqtt_.IsConnected();
    if (reconnect_business_mqtt) {
        /*
         * 动态音频需要新建 HTTPS TLS。临时释放业务 MQTT TLS，避免它与小智 MQTT、
         * HTTPS 三条加密连接同时占用 SRAM；事件箱会在重连后通过 HTTPS 补偿。
         */
        business_mqtt_.Stop();
    }
    if (setting.voice_enabled && reminder.audio_status == 2
        && !reminder.audio_asset_id.empty()) {
        delivered = StreamReminderAudio(reminder);
    }

    if (setting.voice_enabled && !delivered && !reminder_playback_cancelled_.load()) {
        /*
         * TTS 尚未生成、下载失败或用户关闭语音时，仍以固定提示音完成可感知提醒。
         * 正文已显示在屏保，固定音不包含用户数据，也不会额外占用网络。
        */
        auto& audio = Application::GetInstance().GetAudioService();
        delivered = audio.PlaySoundInterruptible(
            Lang::Sounds::OGG_VIBRATION,
            reminder_playback_cancelled_);
        if (delivered) {
            audio.WaitForPlaybackQueueEmpty();
        }
        delivered = delivered && !reminder_playback_cancelled_.load();
    }

    if (reminder_playback_cancelled_.load()) {
        AcknowledgeReminderDelivery(reminder, 4, 0, nullptr);
    } else if (delivered) {
        AcknowledgeReminderDelivery(reminder, 2, playback_duration_ms, nullptr);
    } else {
        AcknowledgeReminderDelivery(reminder, 3, 0, "audio_delivery_failed");
    }

    reminder_playback_active_.store(false);
    if (setting.screen_enabled && !setting.voice_enabled) {
        /*
         * 纯屏幕提醒没有音频播放时长可作为可见窗口，因此保留正文 10 秒，再由
         * 常规备忘录接口恢复表盘内容；Worker 不在这里阻塞等待。
         */
        next_memo_refresh_ = xTaskGetTickCount() + pdMS_TO_TICKS(10000);
    } else {
        RestoreScreensaverMemos();
    }
    if (reconnect_business_mqtt && network_connected_.load()) {
        /* 用户已唤醒时等待对话结束，避免 MQTT TLS 重连与小智握手争抢 SRAM。 */
        mqtt_restart_requested_.store(true);
    }
}

/**
 * @brief 获取音频授权元数据并把 Ogg 分片逐步解封装到现有 Opus 解码队列。
 * @param reminder 当前提醒 UUID、版本和音频资源标识。
 * @return 文件大小、SHA-256、Ogg 包和播放均完整时返回 true。
 */
bool BackendService::StreamReminderAudio(const BackendReminderPlan& reminder) {
    cJSON* metadata_body = cJSON_CreateObject();
    cJSON_AddStringToObject(metadata_body, "reminder_id", reminder.reminder_id.c_str());
    cJSON_AddNumberToObject(metadata_body, "reminder_version", static_cast<double>(reminder.version));
    HttpResult metadata_result = PostAuthenticated(
        "/api/reminder/audio/metadata/get",
        SerializeAndDelete(metadata_body),
        kReminderAudioMetadataResponseLimit);
    if (!metadata_result.success || reminder_playback_cancelled_.load()) {
        return false;
    }

    cJSON* root = cJSON_Parse(metadata_result.response_body.c_str());
    const cJSON* data = root == nullptr
        ? nullptr
        : cJSON_GetObjectItemCaseSensitive(root, "data");
    BackendReminderAudioMetadata metadata;
    metadata.audio_asset_id = GetJsonString(data, "audio_asset_id");
    metadata.download_path = GetJsonString(data, "download_path");
    metadata.sha256 = GetJsonString(data, "sha256");
    metadata.content_type = GetJsonString(data, "content_type");
    const cJSON* codec = cJSON_GetObjectItemCaseSensitive(data, "codec");
    const cJSON* sample_rate = cJSON_GetObjectItemCaseSensitive(data, "sample_rate");
    const cJSON* channels = cJSON_GetObjectItemCaseSensitive(data, "channels");
    const cJSON* frame_duration = cJSON_GetObjectItemCaseSensitive(data, "frame_duration_ms");
    const cJSON* duration = cJSON_GetObjectItemCaseSensitive(data, "duration_ms");
    const cJSON* size_bytes = cJSON_GetObjectItemCaseSensitive(data, "size_bytes");
    metadata.codec = cJSON_IsNumber(codec) ? static_cast<uint8_t>(codec->valueint) : 0;
    metadata.sample_rate = cJSON_IsNumber(sample_rate)
        ? static_cast<uint32_t>(sample_rate->valuedouble) : 0;
    metadata.channels = cJSON_IsNumber(channels)
        ? static_cast<uint8_t>(channels->valueint) : 0;
    metadata.frame_duration_ms = cJSON_IsNumber(frame_duration)
        ? static_cast<uint16_t>(frame_duration->valueint) : 0;
    metadata.duration_ms = cJSON_IsNumber(duration)
        ? static_cast<uint32_t>(duration->valuedouble) : 0;
    metadata.size_bytes = cJSON_IsNumber(size_bytes)
        ? static_cast<uint32_t>(size_bytes->valuedouble) : 0;
    cJSON_Delete(root);

    const bool sha256_is_valid = metadata.sha256.size() == 64
        && std::all_of(metadata.sha256.begin(), metadata.sha256.end(), [](unsigned char character) {
            return (character >= '0' && character <= '9')
                || (character >= 'a' && character <= 'f');
        });
    if (metadata.audio_asset_id != reminder.audio_asset_id
        || metadata.download_path.rfind("/api/reminder/audio/", 0) != 0
        || metadata.download_path.find("://") != std::string::npos
        || !sha256_is_valid || metadata.content_type != "audio/ogg" || metadata.codec != 1
        || metadata.sample_rate != 24000 || metadata.channels != 1
        || metadata.frame_duration_ms != 60 || metadata.duration_ms == 0
        || metadata.size_bytes == 0 || metadata.size_bytes > kMaximumReminderAudioBytes) {
        ESP_LOGW(kTag, "Reminder audio metadata validation failed");
        return false;
    }

    Settings settings(kSettingsNamespace, false);
    const std::string device_id = settings.GetString(kDeviceIdKey);
    const std::string access_token = settings.GetString(kAccessTokenKey);
    if (device_id.empty() || access_token.empty()) {
        return false;
    }

    auto http = Board::GetInstance().GetNetwork()->CreateHttp(2);
    if (!http) {
        return false;
    }
    const esp_app_desc_t* app = esp_app_get_description();
    http->SetTimeout(2000);
    http->SetKeepAlive(false);
    http->SetHeader("Accept", "audio/ogg");
    http->SetHeader("Range", "bytes=0-");
    http->SetHeader("Authorization", "Bearer " + access_token);
    http->SetHeader("X-Device-Id", device_id);
    http->SetHeader("X-Request-Id", GenerateUuid());
    http->SetHeader("X-Client-Id", Board::GetInstance().GetUuid());
    http->SetHeader("X-Firmware-Version", app->version);
    if (!http->Open("GET", std::string(kApiBaseUrl) + metadata.download_path)) {
        return false;
    }

    const int status = http->GetStatusCode();
    const size_t declared_length = http->GetBodyLength();
    if ((status != 200 && status != 206)
        || (declared_length != 0 && declared_length != metadata.size_bytes)) {
        http->Close();
        ESP_LOGW(kTag, "Reminder audio HTTP response is invalid, status=%d", status);
        return false;
    }

    auto& audio = Application::GetInstance().GetAudioService();
    audio.PrepareStreamPlayback();
    /* OggDemuxer 内含 8 KB 固定包缓冲，必须放在堆上，不能占用 8 KB Worker 栈。 */
    auto demuxer = std::make_unique<OggDemuxer>();
    bool packet_delivery_ok = true;
    size_t opus_packet_count = 0;
    demuxer->OnDemuxerFinished(
        [this, &audio, &packet_delivery_ok, &opus_packet_count](
            const uint8_t* packet_data,
            int packet_sample_rate,
            size_t packet_size) {
            if (!packet_delivery_ok || reminder_playback_cancelled_.load()) {
                return;
            }
            auto packet = std::make_unique<AudioStreamPacket>();
            packet->sample_rate = packet_sample_rate;
            packet->frame_duration = 60;
            packet->payload.assign(packet_data, packet_data + packet_size);
            packet_delivery_ok = audio.PushPacketToDecodeQueueInterruptible(
                std::move(packet),
                reminder_playback_cancelled_);
            if (packet_delivery_ok) {
                ++opus_packet_count;
            }
        });

    mbedtls_sha256_context sha256;
    mbedtls_sha256_init(&sha256);
    bool hash_ok = mbedtls_sha256_starts(&sha256, 0) == 0;
    std::array<char, kAudioReadBufferBytes> buffer{};
    size_t total_read = 0;
    while (packet_delivery_ok && hash_ok && !reminder_playback_cancelled_.load()) {
        const int read = http->Read(buffer.data(), buffer.size());
        if (read < 0) {
            packet_delivery_ok = false;
            break;
        }
        if (read == 0) {
            break;
        }
        total_read += static_cast<size_t>(read);
        if (total_read > metadata.size_bytes) {
            packet_delivery_ok = false;
            break;
        }
        hash_ok = mbedtls_sha256_update(
            &sha256,
            reinterpret_cast<const unsigned char*>(buffer.data()),
            static_cast<size_t>(read)) == 0;
        if (demuxer->Process(
                reinterpret_cast<const uint8_t*>(buffer.data()),
                static_cast<size_t>(read)) != static_cast<size_t>(read)) {
            packet_delivery_ok = false;
            break;
        }
    }
    http->Close();

    std::array<unsigned char, 32> digest{};
    hash_ok = hash_ok && mbedtls_sha256_finish(&sha256, digest.data()) == 0;
    mbedtls_sha256_free(&sha256);
    char digest_text[65]{};
    for (size_t index = 0; index < digest.size(); ++index) {
        std::snprintf(digest_text + index * 2, 3, "%02x", digest[index]);
    }
    hash_ok = hash_ok && metadata.sha256 == digest_text;

    if (packet_delivery_ok && hash_ok && !reminder_playback_cancelled_.load()
        && total_read == metadata.size_bytes && opus_packet_count > 0) {
        audio.WaitForPlaybackQueueEmpty();
    }
    return packet_delivery_ok && hash_ok && !reminder_playback_cancelled_.load()
        && total_read == metadata.size_bytes && opus_packet_count > 0;
}

/**
 * @brief 提交当前提醒版本的 Started、Completed、Failed 或 Dismissed 状态。
 * @param reminder 当前提醒计划。
 * @param status 后端 ReminderDeliveryStatusEnum 数字值。
 * @param playback_duration_ms 完成状态下的播放时长，未知时传 0。
 * @param error_code 失败状态的安全机器码。
 */
void BackendService::AcknowledgeReminderDelivery(
    const BackendReminderPlan& reminder,
    int status,
    uint32_t playback_duration_ms,
    const char* error_code) {
    if (!network_connected_.load()) {
        return;
    }
    cJSON* body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "reminder_id", reminder.reminder_id.c_str());
    cJSON_AddNumberToObject(body, "reminder_version", static_cast<double>(reminder.version));
    cJSON_AddNumberToObject(body, "status", status);
    if (playback_duration_ms == 0) {
        cJSON_AddNullToObject(body, "playback_duration_ms");
    } else {
        cJSON_AddNumberToObject(body, "playback_duration_ms", playback_duration_ms);
    }
    if (error_code == nullptr || *error_code == '\0') {
        cJSON_AddNullToObject(body, "internal_error_code");
    } else {
        cJSON_AddStringToObject(body, "internal_error_code", error_code);
    }
    HttpResult result = PostAuthenticated(
        "/api/reminder/delivery/acknowledge",
        SerializeAndDelete(body),
        4096);
    if (!result.success) {
        ESP_LOGW(kTag, "Reminder delivery acknowledgement failed, status=%d code=%d",
                 status, result.business_code);
    }
}

/**
 * @brief 解析带 Z 或显式偏移的 ISO 8601 时间并换算为 UTC Unix 秒。
 * @param value 后端 DateTime JSON 字符串。
 * @param timestamp 成功时接收结果。
 * @return 格式和日期范围合法时返回 true。
 */
bool BackendService::ParseUtcTimestamp(const char* value, std::time_t& timestamp) {
    if (value == nullptr || std::strlen(value) < 20) {
        return false;
    }
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (std::sscanf(value, "%4d-%2d-%2dT%2d:%2d:%2d",
                    &year, &month, &day, &hour, &minute, &second) != 6
        || value[4] != '-' || value[7] != '-' || value[10] != 'T'
        || value[13] != ':' || value[16] != ':'
        || !IsValidCivilDate(year, month, day)
        || hour < 0 || hour > 23 || minute < 0 || minute > 59
        || second < 0 || second > 59) {
        return false;
    }

    const char* zone = value + 19;
    if (*zone == '.') {
        ++zone;
        if (!std::isdigit(static_cast<unsigned char>(*zone))) {
            return false;
        }
        while (std::isdigit(static_cast<unsigned char>(*zone))) {
            ++zone;
        }
    }

    int offset_seconds = 0;
    if (*zone == 'Z' && zone[1] == '\0') {
        offset_seconds = 0;
    } else if ((*zone == '+' || *zone == '-') && std::strlen(zone) == 6
               && zone[3] == ':') {
        int offset_hour = 0;
        int offset_minute = 0;
        if (std::sscanf(zone + 1, "%2d:%2d", &offset_hour, &offset_minute) != 2
            || offset_hour > 14 || offset_minute > 59) {
            return false;
        }
        offset_seconds = (offset_hour * 60 + offset_minute) * 60;
        if (*zone == '-') {
            offset_seconds = -offset_seconds;
        }
    } else {
        return false;
    }

    const int64_t epoch = DaysFromCivil(
        year,
        static_cast<unsigned>(month),
        static_cast<unsigned>(day)) * 86400
        + hour * 3600 + minute * 60 + second - offset_seconds;
    if (epoch < 0 || epoch > std::numeric_limits<std::time_t>::max()) {
        return false;
    }
    timestamp = static_cast<std::time_t>(epoch);
    return true;
}

/**
 * @brief 主动提醒结束后恢复后端最近一次成功获取的屏保备忘录。
 */
void BackendService::RestoreScreensaverMemos() {
    std::vector<std::string> snapshot;
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        snapshot = memo_cache_;
    }
    Application::GetInstance().Schedule([snapshot = std::move(snapshot)]() {
        auto& board = Board::GetInstance();
        if (board.IsScreensaverActive()) {
            board.GetDisplay()->SetScreensaverMemos(snapshot);
        }
    });
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

    if (screensaver_active_.load() && screensaver_generation_.load() == generation
        && !reminder_playback_active_.load()) {
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
