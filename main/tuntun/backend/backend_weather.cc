/**
 * @file backend_weather.cc
 * @brief 囤囤AI天气同步、位置设置和播报设置实现。
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

namespace
{
template <size_t N>
void KeepTextAfterFirstBoundary(
    std::string &value,
    const std::array<const char *, N> &boundaries)
{
    size_t first_boundary_end = std::string::npos;
    for (const char *boundary : boundaries)
    {
        size_t position = 0;
        const size_t boundary_length = std::strlen(boundary);
        while ((position = value.find(boundary, position)) != std::string::npos)
        {
            const size_t boundary_end = position + boundary_length;
            if (position > 0 && boundary_end < value.size() &&
                (first_boundary_end == std::string::npos ||
                 boundary_end < first_boundary_end))
            {
                first_boundary_end = boundary_end;
            }
            position = boundary_end;
        }
    }
    if (first_boundary_end != std::string::npos)
    {
        value.erase(0, first_boundary_end);
    }
}

std::string FormatWeatherLocationForDisplay(const std::string &location_name)
{
    const size_t first_character = location_name.find_first_not_of(" \t\r\n");
    if (first_character == std::string::npos)
    {
        return location_name;
    }
    const size_t last_character = location_name.find_last_not_of(" \t\r\n");
    std::string display_name = location_name.substr(
        first_character, last_character - first_character + 1);
    const std::string original_name = display_name;

    // 先去掉省级前缀，再按市、自治州等上级边界提取最终展示的行政区。
    KeepTextAfterFirstBoundary(
        display_name,
        std::array<const char *, 3>{"特别行政区", "自治区", "省"});
    KeepTextAfterFirstBoundary(
        display_name,
        std::array<const char *, 4>{"自治州", "地区", "盟", "市"});

    constexpr std::array<const char *, 12> kTerminalSuffixes = {
        "特别行政区", "自治州", "自治县", "自治旗", "新区", "地区",
        "林区", "市", "区", "县", "旗", "盟"};
    for (const char *suffix : kTerminalSuffixes)
    {
        const size_t suffix_length = std::strlen(suffix);
        if (display_name.size() > suffix_length &&
            display_name.compare(
                display_name.size() - suffix_length,
                suffix_length,
                suffix) == 0)
        {
            display_name.erase(display_name.size() - suffix_length);
            break;
        }
    }

    return display_name.empty() ? original_name : display_name;
}
} // namespace

/**
 * @brief 在天气缓存确实需要更新时把请求加入常驻后端 Worker。
 * @param force_refresh true 忽略最近成功时间；false 遵守 30 分钟本地新鲜周期。
 */
void BackendService::StartWeatherSync(bool force_refresh)
{
    if (notification_playback_active_.load())
    {
        if (force_refresh)
        {
            weather_refresh_requested_.store(true);
        }
        return;
    }
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
 * @brief 通过设备 Token 获取一组天气数据并更新运行内存缓存。
 */
void BackendService::RunWeatherSync()
{
    if (notification_playback_active_.load())
    {
        weather_refresh_requested_.store(true);
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
        ShowWeatherStatusIfUnavailable("");
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
        if (parsed)
        {
            snapshot.location_name = FormatWeatherLocationForDisplay(
                snapshot.location_name);
        }
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
