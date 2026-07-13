#ifndef _APPLICATION_H_
#define _APPLICATION_H_

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <esp_timer.h>

#include <string>
#include <atomic>
#include <mutex>
#include <deque>
#include <memory>
#include <functional>

#include "protocol.h"
#include "ota.h"
#include "audio_service.h"
#include "device_state.h"
#include "device_state_machine.h"

/**
 * @file application.h
 * @brief 语音助手应用层总控类。
 *
 * Application 负责把网络、音频、显示、OTA 和设备状态机组织成一个完整的
 * 事件驱动应用。其他 FreeRTOS 任务不能直接修改业务状态，而是通过事件位或
 * Schedule() 把操作投递到 Run() 所在的主任务执行，从而避免跨任务竞态。
 */

// 主事件组中的事件位。每一位代表一种需要由主事件循环串行处理的异步事件。
#define MAIN_EVENT_SCHEDULE             (1 << 0)
#define MAIN_EVENT_SEND_AUDIO           (1 << 1)
#define MAIN_EVENT_WAKE_WORD_DETECTED   (1 << 2)
#define MAIN_EVENT_VAD_CHANGE           (1 << 3)
#define MAIN_EVENT_ERROR                (1 << 4)
#define MAIN_EVENT_ACTIVATION_DONE      (1 << 5)
#define MAIN_EVENT_CLOCK_TICK           (1 << 6)
#define MAIN_EVENT_NETWORK_CONNECTED    (1 << 7)
#define MAIN_EVENT_NETWORK_DISCONNECTED (1 << 8)
#define MAIN_EVENT_TOGGLE_CHAT          (1 << 9)
#define MAIN_EVENT_START_LISTENING      (1 << 10)
#define MAIN_EVENT_STOP_LISTENING       (1 << 11)
#define MAIN_EVENT_STATE_CHANGED        (1 << 12)


/**
 * @brief 小智语音助手的单例应用控制器。
 */
class Application {
public:
    /**
     * @brief 获取全局唯一的应用实例。
     * @return Application 单例引用。
     */
    static Application& GetInstance() {
        static Application instance;
        return instance;
    }
    // 单例不可复制，避免出现两个主事件循环或两套协议资源。
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    /**
     * @brief 初始化显示、音频、网络回调、状态机和定时器。
     *
     * 网络连接会异步启动，本方法完成并不表示云端已经连接。应在 app_main 中
     * 调用一次，随后调用 Run() 进入主事件循环。
     */
    void Initialize();

    /**
     * @brief 运行应用主事件循环。
     *
     * 本方法阻塞等待事件组，依次处理网络、状态变化、按键和音频事件，正常情况
     * 下不会返回。所有业务状态切换均在这个调用上下文中完成。
     */
    void Run();

    /**
     * @brief 获取当前设备业务状态。
     * @return 当前 DeviceState 枚举值。
     */
    DeviceState GetDeviceState() const { return state_machine_.GetState(); }
    /**
     * @brief 查询语音活动检测器当前是否判断用户正在说话。
     */
    bool IsVoiceDetected() const { return audio_service_.IsVoiceDetected(); }
    
    /**
     * @brief 请求状态机切换到指定状态。
     * @param state 目标设备状态。
     * @return 转移符合状态规则并执行成功时返回 true，否则返回 false。
     */
    bool SetDeviceState(DeviceState state);

    /**
     * @brief 将回调投递到主任务执行。
     * @param callback 待执行的一次性回调；所有权通过右值移动到内部队列。
     * @note 可从任意任务调用，适合把网络或音频回调转换为主任务操作。
     */
    void Schedule(std::function<void()>&& callback);

    /**
     * @brief 显示一条告警并按需播放提示音。
     * @param status 状态栏文字，必须在调用期间有效。
     * @param message 告警正文。
     * @param emotion 表情资源名称，空字符串表示不改变表情。
     * @param sound 内嵌 OGG 数据视图，空视图表示静默告警。
     */
    void Alert(const char* status, const char* message, const char* emotion = "", const std::string_view& sound = "");
    /**
     * @brief 关闭当前告警并恢复与设备状态对应的正常界面。
     */
    void DismissAlert();

    /**
     * @brief 中断云端正在进行的语音播报。
     * @param reason 中断原因，会发送给服务器。
     */
    void AbortSpeaking(AbortReason reason);

    /**
     * @brief 在线程安全的事件方式下切换对话状态。
     *
     * 空闲时开始聆听，聆听或播报时执行停止/中断。这里只设置事件位，实际操作
     * 由 Run() 中的 HandleToggleChatEvent() 完成。
     */
    void ToggleChatState();

    /**
     * @brief 请求设备开始录音和上传语音。
     * @note 方法本身不直接操作音频硬件，可从按键回调等其他任务安全调用。
     */
    void StartListening();

    /**
     *
     * @brief 请求设备停止录音并通知云端本轮输入结束。
     */
    void StopListening();

    /**
     * @brief 等待必要数据落盘后重启 ESP32。
     */
    void Reboot();
    /**
     * @brief 处理本地唤醒词触发。
     * @param wake_word 被识别出的唤醒词文本。
     */
    void WakeWordInvoke(const std::string& wake_word);
    /**
     * @brief 从指定地址下载并安装固件。
     * @param url 固件二进制的 HTTP/HTTPS 地址。
     * @param version 用于界面显示的目标版本号，可为空。
     * @return 升级流程启动并完成成功时返回 true。
     */
    bool UpgradeFirmware(const std::string& url, const std::string& version = "");
    /**
     * @brief 判断当前状态是否允许进入省电模式。
     */
    bool CanEnterSleepMode();
    /**
     * @brief 通过当前云端协议发送 MCP JSON 消息。
     * @param payload 完整 JSON 文本。
     */
    void SendMcpMessage(const std::string& payload);
    /**
     * @brief 注册 MCP 广播发送器。
     * @param callback 接收待广播 JSON 文本的回调。
     */
    void RegisterMcpBroadcastCallback(std::function<void(const std::string&)> callback);
    /**
     * @brief 异步播放一段内嵌 OGG 音频。
     * @param sound 指向完整 OGG 数据的视图。
     */
    void PlaySound(const std::string_view& sound);
    /**
     * @brief 获取音频服务，供板级工具查询或控制音频功能。
     */
    AudioService& GetAudioService() { return audio_service_; }
    
    /**
     * @brief 线程安全地释放协议和 OTA 资源。
     *
     * 会关闭音频通道并销毁联网后创建的对象。可从任意任务调用，但真正的资源
     * 生命周期变更会与主任务同步，防止断网回调与协议收包同时访问悬空对象。
     */
    void ResetProtocol();

private:
    Application();
    ~Application();

    std::mutex mutex_;
    std::deque<std::function<void()>> main_tasks_;
    std::unique_ptr<Protocol> protocol_;
    EventGroupHandle_t event_group_ = nullptr;
    esp_timer_handle_t clock_timer_handle_ = nullptr;
    DeviceStateMachine state_machine_;
    ListeningMode listening_mode_ = kListeningModeAutoStop;
    std::string last_error_message_;
    AudioService audio_service_;
    std::unique_ptr<Ota> ota_;

    std::function<void(const std::string&)> mcp_broadcast_callback_;

    bool has_server_time_ = false;
    bool aborted_ = false;
    bool assets_version_checked_ = false;
    bool play_popup_on_listening_ = false;  // Flag to play popup sound after state changes to listening
    /**
     * @brief 标记最近一次状态转移是否代表一轮对话已经结束。
     * @details 状态机监听器可能运行在协议回调任务中，因此使用原子变量把“监听或播报转为空闲”
     *          事件安全地交给主循环消费；主循环消费后立即清零，避免后续无关空闲状态误进屏保。
     */
    std::atomic<bool> enter_screensaver_after_conversation_{false};
    int clock_ticks_ = 0;
    TaskHandle_t activation_task_handle_ = nullptr;


    // 下列事件处理器仅由 Run() 调用，因此可直接修改业务状态。
    void HandleStateChangedEvent();
    void HandleToggleChatEvent();
    void HandleStartListeningEvent();
    void HandleStopListeningEvent();
    void HandleNetworkConnectedEvent();
    void HandleNetworkDisconnectedEvent();
    void HandleActivationDoneEvent();
    void HandleWakeWordDetectedEvent();
    void ContinueOpenAudioChannel(ListeningMode mode);
    void ContinueWakeWordInvoke(const std::string& wake_word);

    /**
     * @brief 后台执行设备激活，完成后设置 MAIN_EVENT_ACTIVATION_DONE。
     */
    void ActivationTask();

    // 主流程内部辅助方法。
    void CheckAssetsVersion();
    void CheckNewVersion();
    void InitializeProtocol();
    void ShowActivationCode(const std::string& code, const std::string& message);
    void SetListeningMode(ListeningMode mode);
    ListeningMode GetDefaultListeningMode() const;
    
    // State change handler called by state machine
    void OnStateChanged(DeviceState old_state, DeviceState new_state);
};


#endif // _APPLICATION_H_
