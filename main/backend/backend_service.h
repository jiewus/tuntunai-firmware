#ifndef TUNTUN_BACKEND_SERVICE_H
#define TUNTUN_BACKEND_SERVICE_H

#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mqtt.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <memory>
#include <string>
#include <vector>

class McpServer;

/**
 * @file backend_service.h
 * @brief 囤囤AI业务 API 的设备绑定、天气、备忘录和动态 MCP 能力入口。
 */

/**
 * @brief 管理固件与囤囤AI后端之间的异步业务流程。
 *
 * 当前版本实现设备绑定码申请、状态轮询、设备 Token 领取、绑定页面联动、屏保天气与备忘录
 * 同步、内置和动态 MCP 工具执行，以及主动通知同步和语音播放。天气、备忘录、MCP 和通知
 * 的非实时后端请求统一由一个常驻 Worker 串行调度。
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
     * @brief 注册由小智大模型调用的设备绑定、天气和备忘录工具。
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
     * @brief 根据屏保可见状态启停按需天气和备忘录同步。
     * @param active true 表示屏保已经显示；false 表示屏保已经退出。
     * @details 进入屏保后优先显示内存中的最近天气；缓存过期且网络与设备凭据可用时把请求
     *          加入常驻 Worker。退出屏保后不再创建请求，已经发出的请求允许安全收尾并缓存结果。
     */
    void OnScreensaverChanged(bool active);

    /**
     * @brief 用户开始主动交互时中断正在下载或播放的后台通知。
     * @return true 表示存在通知播放任务且已经发出中断请求。
     */
    bool InterruptNotificationPlayback();

    /**
     * @brief 判断设备绑定覆盖页面当前是否可见。
     * @return 绑定页面正在显示时返回 true，否则返回 false。
     */
    bool IsBindingPageVisible() const;

    /**
     * @brief 由语音指令或物理按键退出当前绑定页面，但保留后台绑定轮询。
     * @return 成功关闭正在显示的绑定页面时返回 true，否则返回 false。
     */
    bool ExitBindingPage();

private:
    /**
     * @brief 后端常驻 Worker 任务栈大小，单位为字节。
     */
    static constexpr uint32_t kBackendWorkerStackSize = 16384;
    /**
     * @brief 常驻后端 Worker 优先级。
     */
    static constexpr UBaseType_t kBackendWorkerPriority = 1;
    /**
     * @brief 后端 Worker 固定环形队列最大任务数。
     */
    static constexpr size_t kBackendWorkerQueueCapacity = 12;

    /** @brief 业务 EMQX 首次断线重连等待秒数。 */
    static constexpr uint32_t kNotificationReconnectInitialSeconds = 5;
    /** @brief MQTT 回调允许缓存的最大通知提示数量。 */
    static constexpr size_t kNotificationHintCapacity = 4;

    /**
     * @brief MCP 申请绑定码完成后的轻量结果回调。
     * @param message 返回给大模型的中文结果文本。
     * @param is_error true 表示本次未能生成或恢复绑定码。
     */
    using BindingCompletion = std::function<void(const std::string& message, bool is_error)>;

    /**
     * @brief MCP 设置天气位置模式完成后的轻量结果回调。
     * @param message 返回给大模型的中文设置结果。
     * @param is_error true 表示固定城市或 IP 自动定位模式未能保存到囤囤AI平台。
     */
    using WeatherLocationCompletion = std::function<void(
        const std::string& message,
        bool is_error)>;

    /**
     * @brief 动态 MCP 工具请求完成后的轻量结果回调。
     * @param message 返回给小智模型的统一业务文本。
     * @param is_error true 表示平台或第三方服务未能成功执行工具。
     */
    using DynamicToolCompletion = std::function<void(
        const std::string& message,
        bool is_error)>;

    /**
     * @brief 备忘录语音工具完成后的轻量结果回调。
     * @param message 返回给小智模型的中文创建、查询、修改、删除或统计结果。
     * @param is_error true 表示备忘录操作未成功完成。
     */
    using MemoCompletion = std::function<void(
        const std::string& message,
        bool is_error)>;

    /**
     * @brief 定义共享备忘录 Worker 当前执行的语音操作。
     */
    enum class MemoToolOperation : uint8_t {
        Create,
        Query,
        Update,
        Delete,
        Statistics
    };

    /** @brief 常驻后端 Worker 支持的固定任务类型。 */
    enum class BackendJobType : uint8_t {
        McpManifest,
        DynamicTool,
        MemoTool,
        MemoSync,
        WeatherSync,
        WeatherLocation,
        WeatherAnnouncement,
        NotificationSync,
        Heartbeat
    };

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
     * @brief 传递给独立天气播报设置任务的强类型参数。
     */
    struct WeatherAnnouncementTaskContext {
        /** @brief 指向固件生命周期内唯一后端服务实例。 */
        BackendService* service = nullptr;
        /** @brief true 表示开启或修改每日播报，false 表示关闭。 */
        bool enabled = false;
        /** @brief 开启时使用的 HH:mm:ss 本地时间。 */
        std::string announcement_time;
        /** @brief 设置完成后回复原 MCP 调用的一次性回调。 */
        WeatherLocationCompletion completion;
    };

    /**
     * @brief 传递给备忘录语音工具 Worker 的强类型调用参数。
     */
    struct MemoToolTaskContext {
        /**
         * @brief 指向固件生命周期内唯一的后端服务实例。
         */
        BackendService* service = nullptr;
        /**
         * @brief 本次需要执行的创建、查询或统计操作。
         */
        MemoToolOperation operation = MemoToolOperation::Query;
        /**
         * @brief 创建或修改操作使用的备忘录正文。
         */
        std::string content;
        /**
         * @brief 修改或删除操作使用的后端备忘录唯一编号。
         */
        std::string memo_id;
        /**
         * @brief 创建或修改操作使用的 RFC 3339 提醒时间；空字符串表示没有提醒时间。
         */
        std::string remind_at;
        /**
         * @brief 查询或修改使用的状态值；查询时 0 表示全部，1、2 表示未完成、已完成。
         */
        int status = 1;
        /**
         * @brief 查询操作使用的时间范围；0 至 4 分别表示不限、今天、明天、本周、未到期。
         */
        int time_range = 0;
        /**
         * @brief HTTP 操作结束后回复原始 MCP tools/call 的一次性回调。
         */
        MemoCompletion completion;
    };

    /**
     * @brief 传递给动态 MCP 执行任务的固定上下文。
     */
    struct DynamicToolTaskContext {
        /** @brief 动态 MCP 工具代码。 */
        std::string tool_name;
        /** @brief 动态 MCP 工具清单修订号。 */
        uint32_t tool_revision = 0;
        /** @brief 工具执行完成后回复 MCP 调用的一次性回调。 */
        DynamicToolCompletion completion;
    };

    /**
     * @brief 一个固定环形队列中的后端任务描述。
     */
    struct BackendJob {
        /** @brief 后端任务类型。 */
        BackendJobType type = BackendJobType::McpManifest;
        /** @brief 可选的强类型任务上下文，任务执行后由 Worker 释放。 */
        void* context = nullptr;
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
     * @brief 保存最近一次成功同步且已经完成边界校验的屏保备忘录。
     */
    struct MemoSnapshot {
        /**
         * @brief true 表示 contents 是后端最近一次成功返回的权威列表，包括空列表。
         */
        bool valid = false;
        /**
         * @brief 已按提醒时间排序的屏保文本，首行为时间，后续为正文，最多保存前 5 条。
         */
        std::vector<std::string> contents;
    };

    /**
     * @brief 保存一项已经安装到设备 MCP 服务器的动态工具展示信息。
     */
    struct DynamicToolSummary {
        /**
         * @brief 后端签发且包含 custom. 前缀的完整工具代码。
         */
        std::string name;
        /**
         * @brief 后台配置的中文工具名称。
         */
        std::string display_name;
        /**
         * @brief 后台配置并提供给大模型的工具用途说明。
         */
        std::string description;
    };

    /**
     * @brief 保存当前设备等待处理的一条主动通知。
     */
    struct PendingNotification {
        /** @brief 通知 UUID。 */
        std::string notification_id;
        /** @brief 设备投递 UUID。 */
        std::string delivery_id;
        /** @brief 设备页面显示文本。 */
        std::string display_text;
        /** @brief MCP 名称、天气播报或备忘录提醒。 */
        std::string source_title;
        /** @brief 1直接播报，2询问后播报。 */
        int notification_mode = 1;
        /** @brief true 表示后端已经准备可播放 Ogg/Opus。 */
        bool has_audio = false;
        /** @brief true 表示其余字段包含有效通知。 */
        bool valid = false;
    };

    /**
     * @brief 保存 MQTT 回调已经完成边界校验的一条轻量通知提示。
     */
    struct NotificationHint {
        /** @brief 通知 UUID，以空字符结尾。 */
        std::array<char, 51> notification_id{};
        /** @brief 设备投递 UUID，以空字符结尾。 */
        std::array<char, 51> delivery_id{};
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
     * @brief 常驻后端 Worker 任务入口。
     * @param context 指向 BackendService 单例。
     */
    static void BackendWorkerTaskEntry(void* context);

    /**
     * @brief 把一个非实时后端操作加入固定环形 Worker 队列。
     * @param type 要执行的后端任务类型。
     * @param context 任务上下文；无上下文任务传入空指针。
     * @return 成功入队时返回 true；Worker 不可用或队列已满时返回 false。
     */
    bool EnqueueBackendJob(BackendJobType type, void* context = nullptr);

    /**
     * @brief 执行一个已经从固定队列取出的后端任务。
     * @param job 固定队列中的任务描述。
     */
    void ExecuteBackendJob(const BackendJob& job);

    /** @brief 执行动态 MCP 清单同步任务并释放任务状态。 */
    void ExecuteMcpManifestJob();
    /** @brief 执行动态 MCP 工具任务并释放任务上下文。 */
    void ExecuteDynamicToolJob(DynamicToolTaskContext* context);
    /** @brief 执行备忘录语音任务并释放任务上下文。 */
    void ExecuteMemoToolJob(MemoToolTaskContext* context);
    /** @brief 执行备忘录屏保同步任务。 */
    void ExecuteMemoSyncJob();
    /** @brief 执行天气屏保同步任务。 */
    void ExecuteWeatherSyncJob();
    /** @brief 执行天气城市设置任务并释放任务上下文。 */
    void ExecuteWeatherLocationJob(WeatherLocationTaskContext* context);
    /** @brief 执行天气播报设置任务并释放任务上下文。 */
    void ExecuteWeatherAnnouncementJob(WeatherAnnouncementTaskContext* context);
    /** @brief 执行主动通知同步任务。 */
    void ExecuteNotificationSyncJob();
    /** @brief 通过业务 EMQX 发布一次设备心跳。 */
    void ExecuteHeartbeatJob();

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
     * @brief 申请或恢复绑定会话，持续轮询并在绑定成功后领取设备 Token。
     * @param request_new_session true 创建新会话并显示绑定页面；false 在后台静默恢复已有会话。
     * @param completion MCP 工具的一次性完成回调。
     */
    void RunBindingTask(bool request_new_session, BindingCompletion& completion);

    /**
     * @brief 显示设备绑定覆盖页面；用户主动退出后忽略当前会话的后续状态更新。
     * @param binding_code 当前绑定码；空字符串表示仅显示流程状态。
     * @param message 绑定页面底部提示或流程结果。
     */
    void ShowBindingPage(const std::string& binding_code, const std::string& message);

    /**
     * @brief 隐藏设备绑定覆盖页面，不改变后台绑定会话和轮询状态。
     */
    void HideBindingPage();

    /**
     * @brief 把修改固定城市或 IP 自动定位模式的请求加入常驻后端 Worker。
     * @param location_name 固定模式下由用户提供的城市或区县名称；IP 模式下为空。
     * @param use_ip_auto true 切换为设备公网 IP 自动定位；false 设置固定城市。
     * @param completion 保存结果的一次性回调。
     */
    void StartWeatherLocationTask(
        const std::string& location_name,
        bool use_ip_auto,
        WeatherLocationCompletion completion);

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
     * @brief 把开启、修改或关闭每日天气播报的请求加入常驻后端 Worker。
     */
    void StartWeatherAnnouncementTask(
        bool enabled,
        const std::string& announcement_time,
        WeatherLocationCompletion completion);

    /**
     * @brief 读取当前天气位置并只更新每日播报配置。
     */
    void RunWeatherAnnouncementTask(
        bool enabled,
        const std::string& announcement_time,
        WeatherLocationCompletion& completion);

    /**
     * @brief 在网络和设备凭据可用时把动态 MCP 清单同步加入常驻后端 Worker。
     * @details 同一时间最多存在一个清单请求；临时网络失败不会清除最近一次成功清单。
     */
    void StartMcpManifestSync();

    /**
     * @brief 获取并严格校验当前设备的权威动态 MCP 工具清单。
     * @details 校验成功后把工具替换操作投递到 Application 主任务，保证 MCP 工具集合切换安全。
     */
    void RunMcpManifestSync();

    /**
     * @brief 把单个已发布动态 MCP 工具执行请求加入常驻后端 Worker。
     * @param tool_name 后端清单中的完整工具名称。
     * @param tool_revision 清单固定的工具版本号。
     * @param completion 执行结束后回复小智的一次性回调。
     */
    void StartDynamicToolExecution(
        const std::string& tool_name,
        uint32_t tool_revision,
        DynamicToolCompletion completion);

    /**
     * @brief 调用平台执行接口并把 ServiceExecutionResult v1 转为 MCP 文本结果。
     * @param tool_name 本次请求的完整工具名称。
     * @param tool_revision 本次请求的已发布工具版本号。
     * @param completion 执行结束后回复原始 MCP 请求的一次性回调。
     */
    void RunDynamicToolExecution(
        const std::string& tool_name,
        uint32_t tool_revision,
        DynamicToolCompletion& completion);

    /**
     * @brief 在主任务中清空全部后端动态工具和本地清单修订状态。
     * @details 仅在设备认证明确失效时调用；普通断网或服务器错误继续保留最近成功清单。
     */
    void ClearDynamicTools();

    /**
     * @brief 把一个备忘录语音操作加入常驻后端 Worker。
     * @param context 包含操作类型、输入参数和最终结果回调的任务上下文。
     * @details 屏保同步和语音操作共用一个任务槽，避免多个备忘录 HTTPS 请求同时占用内存。
     */
    void StartMemoToolTask(MemoToolTaskContext* context);

    /**
     * @brief 根据任务操作调用创建、列表、修改、删除或统计接口，并生成中文结果。
     * @param context 已完成固件边界校验的语音工具任务上下文。
     */
    void RunMemoToolTask(MemoToolTaskContext& context);

    /**
     * @brief 在满足屏保、网络、凭据和缓存条件时把备忘录同步加入常驻后端 Worker。
     * @param force_refresh true 忽略最近成功时间；false 遵守本地缓存新鲜周期。
     */
    void StartMemoSync(bool force_refresh);

    /**
     * @brief 使用设备 Token 获取前 5 条未完成备忘录并更新运行内存快照。
     */
    void RunMemoSync();

    /**
     * @brief 把有效备忘录快照写入当前 LCD 表盘。
     * @param snapshot 已完成条数和正文长度校验的备忘录快照。
     */
    void ShowMemoSnapshot(const MemoSnapshot& snapshot);

    /**
     * @brief 仅在没有成功备忘录缓存时显示同步状态，避免短暂失败覆盖旧内容。
     * @param message 备忘录区域需要显示的简短状态文本。
     */
    void ShowMemoStatusIfUnavailable(const std::string& message);

    /**
     * @brief 在满足屏保、网络、凭据和缓存条件时把天气同步加入常驻后端 Worker。
     * @param force_refresh true 忽略本地缓存有效期，用于网络重连或刚完成设备绑定；
     *                      false 仅在缓存缺失或超过刷新周期时请求。
     */
    void StartWeatherSync(bool force_refresh);

    /**
     * @brief 周期检查定时器回调，只负责尝试启动天气任务，不直接执行网络请求。
     * @param context 指向当前 BackendService 单例。
     */
    static void WeatherTimerCallback(void* context);

    /**
     * @brief 备忘录兜底同步定时器回调，只负责尝试启动备忘录任务。
     * @param context 指向当前 BackendService 单例。
     */
    static void MemoTimerCallback(void* context);

    /**
     * @brief 备忘录退避重试定时器回调。
     * @param context 指向当前 BackendService 单例。
     */
    static void MemoRetryTimerCallback(void* context);

    /**
     * @brief 按 1、2、5、10、20、30 分钟退避序列安排下一次备忘录同步。
     */
    void ScheduleMemoRetry();

    /**
     * @brief 同步成功后清除备忘录重试状态并停止一次性重试定时器。
     */
    void ResetMemoRetry();

    /**
     * @brief 使用设备 Token 获取、校验并缓存一组屏保天气数据。
     * @details HTTP 和 JSON 处理全部在常驻后端 Worker 中执行，不阻塞 LVGL、音频或应用主任务。
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
     * @brief 在设备已绑定且联网时把通知同步加入常驻后端 Worker。
     * @param reconnect_mqtt true 表示同时按需获取配置并连接业务 EMQX。
     */
    void StartNotificationSync(bool reconnect_mqtt);

    /**
     * @brief 连接独立业务 EMQX 并通过 HTTPS 拉取可处理通知。
     */
    void RunNotificationSync();

    /**
     * @brief 获取业务 EMQX 独立凭据并建立当前设备唯一主题订阅。
     * @return 成功连接和订阅时返回 true。
     */
    bool ConnectNotificationMqtt();

    /**
     * @brief 把合法 MQTT 通知提示写入固定容量队列并按投递标识去重。
     * @param notification_id MQTT 消息中的通知 UUID。
     * @param delivery_id MQTT 消息中的设备投递 UUID。
     * @return 提示成功入队时返回 true；重复或队列已满时返回 false。
     */
    bool EnqueueNotificationHint(
        const std::string& notification_id,
        const std::string& delivery_id);

    /**
     * @brief 从固定容量队列取出最早到达的一条 MQTT 提示。
     * @param hint 接收已经完成边界校验的提示副本。
     * @return 队列存在提示时返回 true。
     */
    bool DequeueNotificationHint(NotificationHint& hint);

    /**
     * @brief 在网络可用时按当前指数退避间隔安排业务 EMQX 重连。
     */
    void ScheduleNotificationMqttReconnect();

    /**
     * @brief 启动两分钟心跳周期并立即安排一次心跳。
     */
    void StartHeartbeatPublishing();

    /**
     * @brief 停止业务 EMQX 心跳周期。
     */
    void StopHeartbeatPublishing();

    /**
     * @brief 在没有待处理心跳时把一次发布加入常驻后端 Worker。
     */
    void EnqueueHeartbeat();

    /**
     * @brief 显示并处理当前待通知；设备忙碌时向后端确认延迟。
     * @param notification 已完成 JSON 边界校验的通知快照。
     */
    void HandlePendingNotification(const PendingNotification& notification);

    /**
     * @brief 启动确认模式的内置询问语和无回答超时任务。
     */
    void StartNotificationConfirmation();

    /**
     * @brief 确认模式内置询问和超时任务入口。
     * @param context 指向当前 BackendService 单例。
     */
    static void NotificationConfirmationTaskEntry(void* context);

    /**
     * @brief 播放固定询问语、开启自动停止监听，并在超时后结束监听和确认延迟。
     */
    void RunNotificationConfirmation();

    /**
     * @brief 创建等待设备空闲后下载并播放当前通知的独立任务。
     */
    void StartPendingNotificationPlayback();

    /**
     * @brief 通知音频播放任务入口。
     * @param context 指向当前 BackendService 单例。
     */
    static void NotificationPlaybackTaskEntry(void* context);

    /**
     * @brief 下载当前通知音频、更新页面并依次确认播放状态。
     */
    void RunPendingNotificationPlayback();

    /**
     * @brief 通过单条 HTTP 连接流式下载并实时解封装通知 Ogg/Opus 音频。
     * @param path 当前通知音频的 API 相对路径。
     * @param access_token 当前绑定设备的 Bearer Token。
     * @return 至少成功送入一个 Opus 包且传输未失败、未被用户中断时返回 true。
     */
    bool StreamNotificationAudio(
        const std::string& path,
        const std::string& access_token);

    /**
     * @brief 向后端确认当前通知投递状态。
     * @param notification 当前通知快照。
     * @param ack_type 固定设备确认动作数值。
     * @return 后端成功接收确认时返回 true。
     */
    bool AckNotification(
        const PendingNotification& notification,
        int ack_type);

    /**
     * @brief 周期通知同步定时器回调。
     * @param context 指向当前 BackendService 单例。
     */
    static void NotificationTimerCallback(void* context);

    /**
     * @brief 业务 EMQX 指数退避重连定时器回调。
     * @param context 指向当前 BackendService 单例。
     */
    static void NotificationReconnectTimerCallback(void* context);

    /**
     * @brief 两分钟设备心跳定时器回调，只负责安排发布任务。
     * @param context 指向当前 BackendService 单例。
     */
    static void HeartbeatTimerCallback(void* context);

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
     * @brief 常驻后端 Worker 任务句柄；整个固件生命周期内保持运行。
     */
    TaskHandle_t backend_worker_task_handle_ = nullptr;
    /**
     * @brief 保护常驻 Worker 待处理任务队列。
     */
    std::mutex backend_job_mutex_;
    /** @brief 常驻 Worker 的固定任务环形队列，避免闭包和队列节点反复分配。 */
    std::array<BackendJob, kBackendWorkerQueueCapacity> backend_jobs_{};
    /** @brief 环形队列下一次取出的下标。 */
    size_t backend_job_head_ = 0;
    /** @brief 环形队列下一次写入的下标。 */
    size_t backend_job_tail_ = 0;
    /** @brief 环形队列当前任务数量。 */
    size_t backend_job_count_ = 0;
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
     * @brief true 表示绑定覆盖页面当前可见，供物理按键优先处理退出操作。
     */
    std::atomic<bool> binding_page_visible_{false};
    /**
     * @brief true 表示用户已主动退出当前绑定页面，后台轮询不得再次弹出该页面。
     */
    bool binding_page_dismissed_ = false;
    /**
     * @brief 串行保护动态清单任务、执行任务和最近成功清单修订号。
     */
    std::mutex dynamic_mcp_mutex_;
    /**
     * @brief 当前动态 MCP 清单请求占用标记；非空表示请求已排队或正在执行。
     */
    TaskHandle_t mcp_manifest_task_handle_ = nullptr;
    /**
     * @brief 当前动态 MCP 工具执行占用标记；第一版同一时间只执行一个自定义工具。
     */
    TaskHandle_t dynamic_tool_task_handle_ = nullptr;
    /**
     * @brief 最近一次成功安装到设备 MCP 服务器的后端清单修订号。
     */
    uint32_t mcp_manifest_revision_ = 0;
    /**
     * @brief true 表示设备已经成功同步过清单，包括修订号为0的空清单。
     */
    bool mcp_manifest_loaded_ = false;
    /**
     * @brief 最近一次成功安装的动态 MCP 名称和说明快照。
     */
    std::vector<DynamicToolSummary> dynamic_tool_summaries_;
    /**
     * @brief true 表示最近一次动态工具执行遇到版本冲突，应在执行任务释放栈后重新同步清单。
     */
    bool mcp_manifest_refresh_requested_ = false;
    /**
     * @brief true 表示金属黑表盘当前可见，只有该状态允许创建天气请求。
     */
    std::atomic<bool> screensaver_active_{false};
    /**
     * @brief 串行保护天气任务句柄、最近快照和成功时间戳。
     */
    std::mutex weather_mutex_;
    /**
     * @brief 当前天气同步占用标记；非空表示请求已排队或正在执行。
     */
    TaskHandle_t weather_task_handle_ = nullptr;
    /**
     * @brief 当前天气城市设置占用标记；非空表示请求已排队或正在执行。
     */
    TaskHandle_t weather_location_task_handle_ = nullptr;
    /**
     * @brief 当前天气播报设置占用标记。
     */
    TaskHandle_t weather_announcement_task_handle_ = nullptr;
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
    /**
     * @brief true 表示收到后端天气设置变更提示，需要忽略缓存并补做一次同步。
     */
    std::atomic<bool> weather_refresh_requested_{false};

    /**
     * @brief 串行保护备忘录任务句柄、最近快照和成功时间戳。
     */
    std::mutex memo_mutex_;
    /**
     * @brief 周期检查备忘录缓存是否需要兜底更新的 esp_timer 句柄。
     */
    esp_timer_handle_t memo_timer_ = nullptr;
    /**
     * @brief 备忘录同步失败后使用的一次性退避重试定时器。
     */
    esp_timer_handle_t memo_retry_timer_ = nullptr;
    /**
     * @brief 当前备忘录操作占用标记；非空表示同步或语音操作已排队/正在执行。
     */
    TaskHandle_t memo_task_handle_ = nullptr;
    /**
     * @brief 最近一次成功屏保备忘录数据的运行内存副本。
     */
    MemoSnapshot memo_snapshot_;
    /**
     * @brief 最近一次成功同步备忘录的单调时钟时间，单位为微秒。
     */
    int64_t memo_last_success_us_ = 0;
    /**
     * @brief true 表示收到后端备忘录变更提示，需要忽略缓存并补做一次同步。
     */
    std::atomic<bool> memo_refresh_requested_{false};
    /**
     * @brief true 表示已经安排一次备忘录退避重试且尚未开始执行。
     */
    std::atomic<bool> memo_retry_pending_{false};
    /**
     * @brief true 表示备忘录退避时间已经到达，应在前置条件恢复后立即执行。
     */
    std::atomic<bool> memo_retry_due_{false};
    /**
     * @brief 下一次失败需要使用的备忘录重试间隔序号。
     */
    size_t memo_retry_index_ = 0;

    /**
     * @brief 串行保护业务 EMQX、通知任务和当前待确认通知。
     */
    std::mutex notification_mutex_;
    /**
     * @brief 囤囤AI业务 EMQX 客户端，不复用小智 MQTT。
     */
    std::unique_ptr<Mqtt> notification_mqtt_;
    /**
     * @brief 当前有效业务 EMQX 客户端裸指针，只用于过滤旧客户端的异步回调。
     */
    std::atomic<Mqtt*> active_notification_mqtt_{nullptr};
    /**
     * @brief 当前设备唯一允许发布的 MQTT 心跳主题。
     */
    std::string heartbeat_topic_;
    /**
     * @brief true 表示一次心跳已经排队或正在发布，防止周期任务重复堆积。
     */
    std::atomic<bool> heartbeat_job_pending_{false};
    /**
     * @brief 当前通知同步占用标记；非空表示请求已排队或正在执行。
     */
    TaskHandle_t notification_task_handle_ = nullptr;
    /**
     * @brief 下一次同步任务是否需要重新建立业务 EMQX。
     */
    std::atomic<bool> notification_reconnect_requested_{false};
    /**
     * @brief 下一次业务 EMQX 重连等待秒数，成功连接后恢复初始值。
     */
    std::atomic<uint32_t> notification_reconnect_delay_seconds_{
        kNotificationReconnectInitialSeconds};
    /**
     * @brief 当前通知音频播放任务句柄。
     */
    TaskHandle_t notification_playback_task_handle_ = nullptr;
    /**
     * @brief true 表示用户唤醒已经要求当前通知停止下载和播放。
     */
    std::atomic<bool> notification_playback_interrupted_{false};
    /**
     * @brief 当前确认模式询问和超时任务句柄。
     */
    TaskHandle_t notification_confirmation_task_handle_ = nullptr;
    /**
     * @brief 五分钟 HTTPS 补偿同步定时器。
     */
    esp_timer_handle_t notification_timer_ = nullptr;
    /**
     * @brief 业务 EMQX 断线后的单次指数退避重连定时器。
     */
    esp_timer_handle_t notification_reconnect_timer_ = nullptr;
    /**
     * @brief 每两分钟安排一次业务 EMQX 心跳的周期定时器。
     */
    esp_timer_handle_t heartbeat_timer_ = nullptr;
    /**
     * @brief 当前等待直接播放或用户确认的通知。
     */
    PendingNotification pending_notification_;
    /**
     * @brief 固定容量 MQTT 提示环形队列，队列满时依赖五分钟 HTTPS 补偿。
     */
    std::array<NotificationHint, kNotificationHintCapacity> notification_hints_{};
    /** @brief 环形队列最早元素下标。 */
    size_t notification_hint_head_ = 0;
    /** @brief 环形队列下一写入下标。 */
    size_t notification_hint_tail_ = 0;
    /** @brief 环形队列当前有效元素数量。 */
    size_t notification_hint_count_ = 0;
};

#endif
