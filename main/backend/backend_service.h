#ifndef TUNTUN_BACKEND_SERVICE_H
#define TUNTUN_BACKEND_SERVICE_H

#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

class McpServer;

/**
 * @file backend_service.h
 * @brief 囤囤管家业务 API 的设备绑定、屏保天气和未接入业务占位能力入口。
 */

/**
 * @brief 管理固件与囤囤管家后端之间的异步业务流程。
 *
 * 当前版本实现设备绑定码申请、状态轮询、设备 Token 领取、绑定页面联动和屏保天气同步。
 * 备忘录、主动提醒和动态工具同步仍保留占位内容。
 */
class BackendService {
public:
    /**
     * @brief 获取固件生命周期内唯一的业务后端服务实例。
     * @return BackendService 单例引用。
     */
    static BackendService& GetInstance();

    /**
     * @brief 从 NVS 恢复绑定会话和设备凭据。
     * @details 网络尚未连接时不发起请求；存在未完成绑定会话时会在网络连接后恢复轮询。
     */
    void Start();

    /**
     * @brief 注册由小智大模型调用的设备绑定工具。
     * @param server 已完成基础工具初始化的设备 MCP 服务器。
     */
    void RegisterMcpTools(McpServer& server);

    /**
     * @brief 接收设备网络已连接通知。
     * @details 设置联网标志，并恢复 NVS 中尚未完成的绑定状态轮询。
     */
    void OnNetworkConnected();

    /**
     * @brief 接收设备网络已断开通知。
     * @details 绑定 Worker 保留会话并暂停 HTTP 请求，网络恢复后继续轮询。
     */
    void OnNetworkDisconnected();

    /**
     * @brief 接收小智 MCP 会话已断开通知。
     * @details 绑定属于设备级流程，不因单次语音会话结束而取消。
     */
    void OnMcpDisconnected();

    /**
     * @brief 根据屏保可见状态启停按需天气同步，并维护备忘录占位内容。
     * @param active true 表示屏保已经显示；false 表示屏保已经退出。
     * @details 进入屏保后优先显示内存中的最近天气；缓存过期且网络与设备凭据可用时创建
     *          单次天气任务。退出屏保后不再创建请求，已经发出的请求允许安全收尾并缓存结果。
     */
    void OnScreensaverChanged(bool active);

private:
    /**
     * @brief MCP 申请绑定码完成后的轻量结果回调。
     * @param message 返回给大模型的中文结果文本。
     * @param is_error true 表示本次未能生成或恢复绑定码。
     */
    using BindingCompletion = std::function<void(const std::string& message, bool is_error)>;

    /**
     * @brief MCP 设置天气位置模式完成后的轻量结果回调。
     * @param message 返回给大模型的中文设置结果。
     * @param is_error true 表示固定城市或 IP 自动定位模式未能保存到囤囤管家平台。
     */
    using WeatherLocationCompletion = std::function<void(
        const std::string& message,
        bool is_error)>;

    /**
     * @brief 传递给独立绑定任务的启动参数。
     */
    struct BindingTaskContext {
        /**
         * @brief 指向固件生命周期内唯一的后端服务实例。
         */
        BackendService* service = nullptr;
        /**
         * @brief 申请完成后回复原始 MCP 工具调用的一次性回调。
         */
        BindingCompletion completion;
        /**
         * @brief true 表示先废弃旧会话并向平台申请新的绑定码。
         */
        bool request_new_session = false;
    };

    /**
     * @brief 传递给独立天气城市设置任务的启动参数。
     */
    struct WeatherLocationTaskContext {
        /**
         * @brief 指向固件生命周期内唯一的后端服务实例。
         */
        BackendService* service = nullptr;
        /**
         * @brief 用户通过语音指定的城市或区县名称。
         */
        std::string location_name;
        /**
         * @brief true 表示切换为设备公网 IP 自动定位；false 表示使用固定城市名称。
         */
        bool use_ip_auto = false;
        /**
         * @brief 保存完成后回复原始 MCP 工具调用的一次性回调。
         */
        WeatherLocationCompletion completion;
    };

    /**
     * @brief 保存最近一次成功同步且已经完成边界校验的屏保天气。
     * @details 快照只保存在运行内存中，不周期写入 Flash，避免天气刷新造成 NVS 擦写损耗。
     */
    struct WeatherSnapshot {
        /**
         * @brief true 表示其余字段包含一组完整有效的天气数据。
         */
        bool valid = false;
        /**
         * @brief 城市或区县的屏幕显示名称。
         */
        std::string location_name;
        /**
         * @brief 当前摄氏温度。
         */
        int temperature = 0;
        /**
         * @brief 中文天气描述，例如“晴”或“多云”。
         */
        std::string weather;
        /**
         * @brief 当天最低摄氏温度。
         */
        int low_temperature = 0;
        /**
         * @brief 当天最高摄氏温度。
         */
        int high_temperature = 0;
    };

    /**
     * @brief 构造业务后端服务。
     */
    BackendService() = default;

    /**
     * @brief 销毁业务后端服务。
     */
    ~BackendService() = default;

    BackendService(const BackendService&) = delete;
    BackendService& operator=(const BackendService&) = delete;

    /**
     * @brief 检查已有绑定状态，并按需启动申请新会话或恢复旧会话的独立任务。
     * @param request_new_session true 先向后端申请新绑定码；false 使用 NVS 中的现有会话。
     * @param completion 新申请由 MCP 触发时返回结果的回调；自动恢复时为空。
     * @details 显式申请绑定且本地已经存在完整设备凭据时直接返回已绑定状态，不访问网络，
     *          也不覆盖当前凭据或生成新的绑定码。
     */
    void StartBindingTask(bool request_new_session, BindingCompletion completion = {});

    /**
     * @brief FreeRTOS 绑定任务入口。
     * @param context 指向由 StartBindingTask 分配的 BindingTaskContext。
     */
    static void BindingTaskEntry(void* context);

    /**
     * @brief 申请绑定码并持续轮询，绑定成功后领取设备 Token。
     * @param request_new_session true 表示先创建新的绑定会话。
     * @param completion MCP 工具的一次性完成回调。
     */
    void RunBindingTask(bool request_new_session, BindingCompletion& completion);

    /**
     * @brief 启动修改固定城市或 IP 自动定位模式的独立任务。
     * @param location_name 固定模式下由用户提供的城市或区县名称；IP 模式下为空。
     * @param use_ip_auto true 切换为设备公网 IP 自动定位；false 设置固定城市。
     * @param completion 保存结果的一次性回调。
     */
    void StartWeatherLocationTask(
        const std::string& location_name,
        bool use_ip_auto,
        WeatherLocationCompletion completion);

    /**
     * @brief FreeRTOS 天气城市设置任务入口。
     * @param context 指向由 StartWeatherLocationTask 分配的 WeatherLocationTaskContext。
     */
    static void WeatherLocationTaskEntry(void* context);

    /**
     * @brief 调用设备天气设置接口并使旧天气缓存立即失效。
     * @param location_name 固定模式下用户指定的城市或区县名称；IP 模式下为空。
     * @param use_ip_auto true 保存 IP 自动定位模式；false 保存固定城市模式。
     * @param completion 保存完成后回复 MCP 请求的一次性回调。
     */
    void RunWeatherLocationTask(
        const std::string& location_name,
        bool use_ip_auto,
        WeatherLocationCompletion& completion);

    /**
     * @brief 在满足屏保、网络、凭据和缓存条件时创建单次天气同步任务。
     * @param force_refresh true 忽略本地缓存有效期，用于网络重连或刚完成设备绑定；
     *                      false 仅在缓存缺失或超过刷新周期时请求。
     */
    void StartWeatherSync(bool force_refresh);

    /**
     * @brief FreeRTOS 天气同步任务入口。
     * @param context 指向当前 BackendService 单例。
     */
    static void WeatherTaskEntry(void* context);

    /**
     * @brief 周期检查定时器回调，只负责尝试启动天气任务，不直接执行网络请求。
     * @param context 指向当前 BackendService 单例。
     */
    static void WeatherTimerCallback(void* context);

    /**
     * @brief 使用设备 Token 获取、校验并缓存一组屏保天气数据。
     * @details HTTP 和 JSON 处理全部在独立任务中执行，不阻塞 LVGL、音频或应用主任务。
     */
    void RunWeatherSync();

    /**
     * @brief 把一组有效天气快照写入当前 LCD 表盘。
     * @param snapshot 已完成字段完整性和温度范围校验的天气快照。
     */
    void ShowWeatherSnapshot(const WeatherSnapshot& snapshot);

    /**
     * @brief 仅在没有成功天气缓存时显示同步状态，避免短暂失败覆盖旧天气。
     * @param message 天气位置区域需要显示的简短状态文本。
     */
    void ShowWeatherStatusIfUnavailable(const std::string& message);

    /**
     * @brief 从 NVS 恢复当前绑定码、绑定会话 Token 和已领取设备凭据。
     */
    void LoadBindingState();

    /**
     * @brief 保存新的短绑定码和私有绑定会话 Token。
     * @param binding_code 仅用于屏幕展示的数字码。
     * @param session_token 仅供设备轮询绑定状态的 Bearer Token。
     */
    void SavePendingBinding(const std::string& binding_code, const std::string& session_token);

    /**
     * @brief 清除已经失效或完成的临时绑定会话。
     */
    void ClearPendingBinding();

    /**
     * @brief 保存绑定完成后后端只返回一次的设备身份和访问 Token。
     * @param device_id 后端设备 UUID。
     * @param access_token 设备业务接口使用的 Bearer Token。
     */
    void SaveDeviceCredential(const std::string& device_id, const std::string& access_token);

    /**
     * @brief 记录 Start() 是否已经执行，避免重复加载 NVS。
     */
    bool started_ = false;
    /**
     * @brief true 表示板级网络已经取得可用连接，允许发起 HTTPS 请求。
     */
    std::atomic<bool> network_connected_{false};
    /**
     * @brief 串行保护任务句柄、临时绑定会话和最终设备凭据。
     */
    std::mutex binding_mutex_;
    /**
     * @brief 当前绑定任务句柄；为空表示允许创建新的绑定任务。
     */
    TaskHandle_t binding_task_handle_ = nullptr;
    /**
     * @brief 当前允许显示给用户的一次性短绑定码。
     */
    std::string binding_code_;
    /**
     * @brief 仅用于查询和完成绑定的私有会话 Bearer Token。
     */
    std::string binding_session_token_;
    /**
     * @brief 绑定完成后平台分配的稳定设备 ID。
     */
    std::string device_id_;
    /**
     * @brief 绑定完成后设备访问业务 API 使用的 Bearer Token。
     */
    std::string device_access_token_;
    /**
     * @brief true 表示金属黑表盘当前可见，只有该状态允许创建天气请求。
     */
    std::atomic<bool> screensaver_active_{false};
    /**
     * @brief 串行保护天气任务句柄、最近快照和成功时间戳。
     */
    std::mutex weather_mutex_;
    /**
     * @brief 当前单次天气任务句柄；为空表示没有天气 HTTP 请求正在执行。
     */
    TaskHandle_t weather_task_handle_ = nullptr;
    /**
     * @brief 当前天气城市设置任务句柄；为空表示允许接受新的语音设置请求。
     */
    TaskHandle_t weather_location_task_handle_ = nullptr;
    /**
     * @brief 周期检查天气缓存是否过期的 esp_timer 句柄。
     */
    esp_timer_handle_t weather_timer_ = nullptr;
    /**
     * @brief 最近一次成功天气数据的运行内存副本。
     */
    WeatherSnapshot weather_snapshot_;
    /**
     * @brief 最近一次成功同步的单调时钟时间，单位为微秒。
     */
    int64_t weather_last_success_us_ = 0;
};

#endif
