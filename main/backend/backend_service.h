#ifndef TUNTUN_BACKEND_SERVICE_H
#define TUNTUN_BACKEND_SERVICE_H

class McpServer;

/**
 * @file backend_service.h
 * @brief 新版业务 API 完成前使用的后端能力占位服务。
 */

/**
 * @brief 保留固件与业务后端之间的统一接入点。
 *
 * 当前版本不访问任何自建 HTTP 接口，不连接业务 MQTT，也不注册天气、备忘录、
 * 主动提醒或动态 MCP 工具。保留这些生命周期方法，是为了避免后续接入新版 API
 * 时再次修改应用主流程和板级屏保流程。
 */
class BackendService {
public:
    /**
     * @brief 获取固件生命周期内唯一的后端占位服务实例。
     * @return BackendService 单例引用。
     */
    static BackendService& GetInstance();

    /**
     * @brief 初始化后端占位服务。
     *
     * 当前实现只输出一次停用说明，不创建任务、队列、定时器或网络连接。
     */
    void Start();

    /**
     * @brief 保留自定义 MCP 工具的注册入口。
     * @param server 已完成基础工具初始化的设备 MCP 服务器。
     *
     * 新版 API 协议尚未完成，因此当前方法不会向服务器注册任何业务工具。
     */
    void RegisterMcpTools(McpServer& server);

    /**
     * @brief 接收设备网络已连接通知。
     *
     * 当前实现不执行激活、同步或数据刷新，仅保留未来后端接入所需的生命周期入口。
     */
    void OnNetworkConnected();

    /**
     * @brief 接收设备网络已断开通知。
     *
     * 当前没有业务网络资源需要释放，因此该方法为空操作。
     */
    void OnNetworkDisconnected();

    /**
     * @brief 接收小智 MCP 会话已断开通知。
     *
     * 当前没有自定义 MCP 请求需要取消，因此该方法为空操作。
     */
    void OnMcpDisconnected();

    /**
     * @brief 根据屏保可见状态写入临时天气和备忘录内容。
     * @param active true 表示屏保已经显示；false 表示屏保已经退出。
     *
     * 进入屏保时直接写入“服务开发中”占位内容，不创建后台任务，也不发起网络请求。
     */
    void OnScreensaverChanged(bool active);

private:
    /**
     * @brief 构造后端占位服务。
     */
    BackendService() = default;

    /**
     * @brief 销毁后端占位服务。
     */
    ~BackendService() = default;

    BackendService(const BackendService&) = delete;
    BackendService& operator=(const BackendService&) = delete;

    /**
     * @brief 记录 Start() 是否已经执行，避免重复输出初始化日志。
     */
    bool started_ = false;
};

#endif
