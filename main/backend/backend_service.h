#ifndef TUNTUN_BACKEND_SERVICE_H
#define TUNTUN_BACKEND_SERVICE_H

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "backend_models.h"
#include "backend_mqtt_client.h"
#include "mcp_server.h"

/**
 * @file backend_service.h
 * @brief 吞吞生活后端认证、HTTP 队列、业务 MQTT、主动提醒和 MCP 接入。
 */

/**
 * @brief 屏保天气区域需要长期保存在 RAM 中的最小数据集合。
 */
struct BackendWeatherData {
    /**
     * @brief 获取或设置后端返回的当前天气位置名称，例如“上海市松江区”。
     */
    std::string location;
    /**
     * @brief 获取或设置当前整数温度，单位为摄氏度。
     */
    int temperature = 0;
    /**
     * @brief 获取或设置当天最高整数温度，单位为摄氏度。
     */
    int high_temperature = 0;
    /**
     * @brief 获取或设置当天最低整数温度，单位为摄氏度。
     */
    int low_temperature = 0;
    /**
     * @brief 获取或设置高德返回的中文天气描述，例如“晴”或“小雨”。
     */
    std::string weather;
    /**
     * @brief 获取或设置是否至少成功获取过一次后端天气数据。
     */
    bool valid = false;
};

/**
 * @brief 串行访问 TuntunLife Web API 的单例后台服务。
 *
 * 所有阻塞 HTTP、JSON 解析和重试均在独立 FreeRTOS Worker 中执行。应用主任务、
 * 网络回调和 LVGL 线程只负责投递作业或接收已经整理好的缓存，避免语音链路因
 * DNS、TLS、数据库或高德接口延迟而卡顿。
 */
class BackendService {
public:
    /**
     * @brief 获取进程生命周期内唯一的后端服务实例。
     * @return BackendService 单例引用。
     */
    static BackendService& GetInstance();

    /**
     * @brief 创建长度为 12 的作业队列、业务 MQTT 提示队列和 8 KB 后台 Worker。
     *
     * 重复调用不会创建第二个任务。该方法不发起网络请求，可以在应用初始化阶段
     * 安全调用；首次联网由 OnNetworkConnected() 触发自动激活。
     */
    void Start();

    /**
     * @brief 注册天气位置及备忘录全套异步 MCP 工具。
     * @param server 已经初始化的设备 MCP 服务器单例。
     */
    void RegisterMcpTools(McpServer& server);

    /**
     * @brief 通知 Worker Wi-Fi 已取得可用 IP。
     *
     * 若 NVS 中没有后端凭据，会自动排队激活；凭据可用后依次获取业务 MQTT
     * 配置、补偿设备事件箱并同步未来 24 小时提醒快照。
     */
    void OnNetworkConnected();

    /**
     * @brief 通知 Worker 当前网络不可用。
     *
     * 已经开始的 HTTP 由底层超时收尾；业务 MQTT 在 Worker 中停止，提醒流设置
     * 原子取消标志。小智协议连接和 Wi-Fi 重连策略不由本服务修改。
     */
    void OnNetworkDisconnected();

    /**
     * @brief 通知 Worker 当前小智 MCP 协议通道已经被释放。
     *
     * 后续尚未成功的 MCP 读操作会停止网络退避；创建、修改和删除等写操作继续执行，
     * 但协议已经断开时其最终结果只会被发送逻辑安全丢弃。
     */
    void OnMcpDisconnected();

    /**
     * @brief 通知后端服务屏保进入或退出。
     * @param active true 表示屏保当前可见；false 表示已经回到对话界面。
     *
     * 进入屏保后立即依次刷新天气和备忘录，并分别建立 30 分钟、5 分钟周期。
     * 退出屏保后停止周期入队，但已经收到的响应仍可更新 RAM 缓存。
     */
    void OnScreensaverChanged(bool active);

    /**
     * @brief 删除后端设备标识和访问令牌。
     *
     * 本方法只应由完整恢复出厂流程或收到后端 2001 未授权响应时调用；普通 Wi-Fi
     * 配置重置不得调用，以免每次配网都重新签发凭据。
     */
    void ClearCredentials();

    /**
     * @brief 立即取消当前主动提醒的下载和音频生产。
     * @return 取消前确实存在活动提醒时返回 true。
     * @details 唤醒词回调会先调用本方法，再由应用主任务清理解码队列并进入小智会话。
     *          本方法只修改原子标志，不阻塞音频采集任务或 MQTT 回调。
     */
    bool CancelActiveReminderPlayback();

private:
    /**
     * @brief Worker 支持的作业类别。
     */
    enum class JobType {
        /**
         * @brief 使用编译期激活码换取设备凭据。
         */
        Activate,
        /**
         * @brief 获取屏保天气并更新 RAM 缓存。
         */
        WeatherRefresh,
        /**
         * @brief 获取屏保备忘录数组并更新 RAM 缓存。
         */
        MemoRefresh,
        /**
         * @brief 获取独立业务 MQTT 的短期连接配置并建立订阅。
         */
        MqttConfigRefresh,
        /**
         * @brief 通过 HTTPS 补偿同步设备事件箱，并确认已处理事件。
         */
        DeviceEventSync,
        /**
         * @brief 获取未来 24 小时提醒权威快照并替换本地计划。
         */
        ReminderSync,
        /**
         * @brief 执行一个由 MCP 工具发起的通用业务请求。
         */
        ApiCall
    };

    /**
     * @brief 队列中以指针传递的一次后台作业。
     */
    struct Job {
        /**
         * @brief 决定 Worker 使用激活、刷新或通用接口处理分支。
         */
        JobType type = JobType::ApiCall;
        /**
         * @brief 保存以斜杠开头的相对 API 路径，例如 /api/memo/create。
         */
        std::string endpoint;
        /**
         * @brief 保存已经序列化的 UTF-8 JSON 请求体。
         */
        std::string body = "{}";
        /**
         * @brief 保存写操作所有网络重试共同复用的幂等 UUID。
         */
        std::string idempotency_key;
        /**
         * @brief 限制本次接口允许读取的最大响应字节数。
         */
        size_t response_limit = 4096;
        /**
         * @brief true 表示网络短暂断开后仍应按退避策略尽力完成。
         */
        bool write_operation = false;
        /**
         * @brief true 表示作业由 MCP tools/call 发起，而不是屏保周期刷新。
         */
        bool mcp_request = false;
        /**
         * @brief 记录作业入队时的屏保世代，用于过滤迟到的 UI 更新。
         */
        uint32_t screensaver_generation = 0;
        /**
         * @brief 保存 MCP 作业结束后回复原 JSON-RPC 请求的回调。
         */
        McpToolCompletion completion;
    };

    /**
     * @brief 单次 HTTP 调用整理后的内部结果。
     */
    struct HttpResult {
        /**
         * @brief HTTP 成功且 ApiResponse.code 为 0 时设置为 true。
         */
        bool success = false;
        /**
         * @brief 网络错误、超时或 HTTP 5xx 允许退避重试时设置为 true。
         */
        bool retryable = false;
        /**
         * @brief 保存 HTTP 状态码，连接尚未建立时保持为 0。
         */
        int http_status = 0;
        /**
         * @brief 保存 ApiResponse.code，无法解析时使用本地系统错误码。
         */
        int business_code = 0;
        /**
         * @brief 保存可返回给 MCP 且不包含敏感内容的业务说明。
         */
        std::string message;
        /**
         * @brief 保存完整但已经受接口字节上限约束的响应 JSON。
         */
        std::string response_body;
    };

    BackendService() = default;
    ~BackendService() = default;
    BackendService(const BackendService&) = delete;
    BackendService& operator=(const BackendService&) = delete;

    /**
     * @brief FreeRTOS 任务入口，将 C 回调参数还原为 BackendService。
     * @param argument Start() 传入的 this 指针。
     */
    static void WorkerEntry(void* argument);

    /**
     * @brief 循环接收作业，并检查 MQTT 提示、周期补偿、屏保刷新和提醒截止时间。
     */
    void WorkerLoop();

    /**
     * @brief 尝试把动态作业指针放入队列。
     * @param job 调用方使用 new 创建的作业，成功后所有权交给 Worker。
     * @param allow_coalesce true 时队列满可将重复屏保刷新合并为一次。
     * @return 成功入队时返回 true；队列已满时返回 false，由调用方释放或稍后重建作业。
     */
    bool Enqueue(Job* job, bool allow_coalesce);

    /**
     * @brief 按作业类型执行激活、刷新或通用 API 请求。
     * @param job Worker 独占的作业对象。
     */
    void ProcessJob(Job& job);

    /**
     * @brief 确保 NVS 中存在可用于认证请求的设备凭据。
     * @return 已有凭据或自动激活成功时返回 true。
     */
    bool EnsureActivated();

    /**
     * @brief 调用设备激活接口并把新凭据原子写入同一 NVS 命名空间。
     * @return 激活成功且响应字段完整时返回 true。
     */
    bool Activate();

    /**
     * @brief 执行带认证头、响应限制和统一业务码解析的 POST 请求。
     * @param endpoint 以斜杠开头的相对 API 路径。
     * @param body UTF-8 JSON 请求体。
     * @param response_limit 响应体最大允许字节数。
     * @param idempotency_key 可选的写操作幂等 UUID。
     * @param authenticated true 时添加设备认证请求头。
     * @return HTTP 状态、业务状态、响应体和重试建议。
     */
    HttpResult Post(const std::string& endpoint, const std::string& body,
                    size_t response_limit, const std::string& idempotency_key,
                    bool authenticated);

    /**
     * @brief 执行一次认证 POST，并在业务码 2001 时仅重新激活和重放一次。
     * @param endpoint 以斜杠开头的业务接口路径。
     * @param body UTF-8 JSON 请求体。
     * @param response_limit 最大响应字节数。
     * @return 首次结果或重新签发凭据后的第二次结果。
     * @details 本方法不执行网络退避，适合周期同步和状态确认，避免长时间阻塞提醒调度。
     */
    HttpResult PostAuthenticated(const std::string& endpoint,
                                 const std::string& body,
                                 size_t response_limit);

    /**
     * @brief 按 10 秒、30 秒、60 秒退避执行请求。
     * @param job 包含接口、请求体、上限和写操作属性的作业。
     * @return 最后一次调用结果或首次成功结果。
     */
    HttpResult ExecuteWithRetry(const Job& job);

    /**
     * @brief 获取并应用业务 MQTT 配置。
     * @return 配置接口成功，且配置为关闭或客户端成功启动时返回 true。
     */
    bool RefreshMqttConfig();

    /**
     * @brief 分页同步当前设备仍可处理的事件，并根据类型刷新权威业务数据。
     */
    void SyncDeviceEvents();

    /**
     * @brief 确认一个设备事件已经完成处理。
     * @param event_id 需要完成的事件 UUID。
     */
    void AcknowledgeDeviceEvent(const std::string& event_id,
                                int status = 3,
                                const char* error_code = nullptr);

    /**
     * @brief 获取未来提醒快照并以固定上限替换本地缓存。
     * @return 响应成功且提醒设置与数组均可解析时返回 true。
     */
    bool SyncReminders();

    /**
     * @brief 同步设备当前绑定的动态 MCP 工具完整权威清单。
     * @return 清单未变化，或新清单通过校验并已排入主任务替换时返回 true。
     */
    bool SyncDynamicMcpManifest();

    /**
     * @brief 把受限 object JSON Schema 转换为固件基础参数定义。
     * @param schema 后端清单中 input_schema 对象。
     * @param properties 成功时接收最多 10 个参数定义。
     * @param schema_json 成功时接收保留完整约束的紧凑 JSON 文本。
     * @return 类型、必填、默认值和整数范围均合法时返回 true。
     */
    static bool ParseDynamicMcpSchema(const cJSON* schema,
                                      PropertyList& properties,
                                      std::string& schema_json);

    /**
     * @brief 将动态工具参数序列化并投递到固定工具版本执行接口。
     * @param tool_name 清单中的稳定工具名称。
     * @param tool_version_id 清单中的不可变工具版本 UUID。
     * @param properties 小智调用后已经完成基础类型校验的参数值。
     * @param completion 原 MCP tools/call 的异步完成回调。
     */
    void QueueDynamicMcpExecution(const std::string& tool_name,
                                  const std::string& tool_version_id,
                                  const PropertyList& properties,
                                  McpToolCompletion completion);

    /**
     * @brief 检查本地最早提醒是否到期，并在设备空闲时执行。
     */
    void ProcessDueReminder();

    /**
     * @brief 执行一条已经从未来计划缓存中认领的提醒。
     * @param reminder 提醒 UUID、版本、显示文本和音频资源信息副本。
     */
    void DeliverReminder(const BackendReminderPlan& reminder);

    /**
     * @brief 查询音频元数据并通过认证 HTTP Range 流式播放 Ogg/Opus。
     * @param reminder 当前执行的提醒版本。
     * @return 完整下载、解封装和音频入队成功且未被用户取消时返回 true。
     */
    bool StreamReminderAudio(const BackendReminderPlan& reminder);

    /**
     * @brief 向后端确认提醒开始、完成或失败。
     * @param reminder 当前执行的提醒版本。
     * @param status ReminderDeliveryStatusEnum 数字值：1 开始、2 完成、3 失败。
     * @param playback_duration_ms 成功时实际音频时长；其他状态传 0。
     * @param error_code 失败时使用的安全机器码；不得包含用户文本。
     */
    void AcknowledgeReminderDelivery(const BackendReminderPlan& reminder,
                                     int status,
                                     uint32_t playback_duration_ms,
                                     const char* error_code);

    /**
     * @brief 解析 .NET DateTime JSON 使用的 UTC ISO 8601 文本。
     * @param value 例如 2026-07-14T12:30:00.0000000Z。
     * @param timestamp 成功时写入 UTC Unix 秒。
     * @return 日期、时间和时区格式均有效时返回 true。
     */
    static bool ParseUtcTimestamp(const char* value, std::time_t& timestamp);

    /**
     * @brief 恢复屏保原有备忘录数组，结束主动提醒的临时覆盖。
     */
    void RestoreScreensaverMemos();

    /**
     * @brief 解析天气成功响应并更新屏保缓存和当前可见 UI。
     * @param response_body ApiResponse&lt;WeatherDto&gt; JSON 文本。
     * @param generation 请求入队时记录的屏保世代号。
     */
    void ApplyWeatherResponse(const std::string& response_body, uint32_t generation);

    /**
     * @brief 解析备忘录屏保响应，最多缓存前 5 条完整正文。
     * @param response_body ApiResponse&lt;MemoScreensaverResult&gt; JSON 文本。
     * @param generation 请求入队时记录的屏保世代号。
     */
    void ApplyMemoResponse(const std::string& response_body, uint32_t generation);

    /**
     * @brief 把通用 API 作业投递给 Worker。
     * @param endpoint 目标相对路径。
     * @param body 紧凑 JSON 请求体。
     * @param write_operation 是否属于创建、修改、完成、恢复或删除操作。
     * @param completion MCP 原请求完成回调。
     * @param idempotency_key 创建备忘录所需的稳定幂等键。
     */
    void QueueApiCall(const std::string& endpoint, std::string body,
                      bool write_operation, McpToolCompletion completion,
                      std::string idempotency_key = "");

    /**
     * @brief 生成 RFC 4122 UUID v4 文本，用于请求追踪或业务幂等。
     * @return 36 字符小写 UUID。
     */
    static std::string GenerateUuid();

    /**
     * @brief 将 cJSON 根对象序列化为紧凑字符串并释放根对象。
     * @param root 调用方移交所有权的 JSON 根对象。
     * @return 序列化结果；失败时返回空对象文本。
     */
    static std::string SerializeAndDelete(cJSON* root);

    /**
     * @brief 长度固定为 12、元素类型为 Job 指针的 FreeRTOS 队列。
     */
    QueueHandle_t queue_ = nullptr;
    /**
     * @brief 使用 8 KB 栈且优先级低于应用主任务的 Worker 句柄。
     */
    TaskHandle_t worker_task_ = nullptr;
    /**
     * @brief 表示 Wi-Fi 是否已经取得可用于访问后端的 IP。
     */
    std::atomic<bool> network_connected_{false};
    /**
     * @brief 表示最近一次 MCP 调用所在的协议通道尚未被显式释放。
     */
    std::atomic<bool> mcp_connected_{false};
    /**
     * @brief 表示金属黑表盘屏保当前是否可见。
     */
    std::atomic<bool> screensaver_active_{false};
    /**
     * @brief 防止重复天气刷新同时入队或执行的合并标志。
     */
    std::atomic<bool> weather_queued_{false};
    /**
     * @brief 防止重复备忘录刷新同时入队或执行的合并标志。
     */
    std::atomic<bool> memo_queued_{false};
    /**
     * @brief 防止同一轮连续激活失败反复弹出通知的标志。
     */
    std::atomic<bool> activation_failure_notified_{false};
    /**
     * @brief 保存下次天气周期刷新对应的 FreeRTOS tick。
     */
    std::atomic<TickType_t> next_weather_refresh_{0};
    /**
     * @brief 保存下次备忘录周期刷新对应的 FreeRTOS tick。
     */
    std::atomic<TickType_t> next_memo_refresh_{0};
    /**
     * @brief 串行保护天气和备忘录 RAM 缓存的互斥量。
     */
    std::mutex cache_mutex_;
    /**
     * @brief 保存最近一次成功解析的屏保天气数据。
     */
    BackendWeatherData weather_cache_;
    /**
     * @brief 保存最近一次成功解析的最多 5 条备忘录正文。
     */
    std::vector<std::string> memo_cache_;
    /**
     * @brief 每次进入或退出屏保递增，用于拒绝迟到响应更新当前 UI。
     */
    std::atomic<uint32_t> screensaver_generation_{0};

    /**
     * @brief 与小智协议连接相互独立的业务 MQTT 客户端和 8 项提示队列。
     */
    BackendMqttClient business_mqtt_;

    /**
     * @brief 后端 Worker 保存的未来提醒快照，元素数量受固定上限约束。
     */
    std::vector<BackendReminderPlan> reminder_plans_;

    /**
     * @brief 串行保护未来提醒快照和生效设置。
     */
    std::mutex reminder_mutex_;

    /**
     * @brief 当前设备生效的提醒总开关、语音和屏幕策略。
     */
    BackendReminderSetting reminder_setting_;

    /**
     * @brief 当前是否正在下载或向解码队列生产主动提醒音频。
     */
    std::atomic<bool> reminder_playback_active_{false};

    /**
     * @brief 唤醒词或用户操作要求当前提醒尽快停止时设置为 true。
     */
    std::atomic<bool> reminder_playback_cancelled_{false};

    /**
     * @brief 联网状态变化后要求 Worker 停止独立 MQTT 的轻量标志。
     */
    std::atomic<bool> mqtt_stop_requested_{false};

    /**
     * @brief 动态音频或大清单同步临时释放业务 MQTT 后，要求回到 Idle 再连接。
     */
    std::atomic<bool> mqtt_restart_requested_{false};

    /**
     * @brief 下次无 MQTT 通知时仍执行事件箱全量补偿的 tick。
     */
    std::atomic<TickType_t> next_event_sync_{0};

    /**
     * @brief 下次固定执行未来提醒快照同步的 tick。
     */
    std::atomic<TickType_t> next_reminder_sync_{0};

    /**
     * @brief 当前 RAM 中已经应用的动态工具清单修订号。
     */
    uint64_t dynamic_manifest_revision_ = 0;

    /**
     * @brief true 表示本次启动已经至少应用或确认过一次权威动态工具清单。
     */
    bool dynamic_manifest_loaded_ = false;
};

#endif
