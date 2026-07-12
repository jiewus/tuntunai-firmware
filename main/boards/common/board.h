#ifndef BOARD_H
#define BOARD_H

#include <http.h>
#include <web_socket.h>
#include <mqtt.h>
#include <udp.h>
#include <string>
#include <functional>
#include <network_interface.h>

#include "led/led.h"
#include "backlight.h"
#include "assets.h"

/**
 * @file board.h
 * @brief 开发板硬件能力与网络能力的统一抽象接口。
 */

/**
 * @brief 网络层向应用层上报的统一事件。
 */
enum class NetworkEvent {
    Scanning,              ///< 正在扫描可用网络。
    Connecting,            ///< 正在连接，data 为 SSID 或网络名称。
    Connected,             ///< 已连接，data 为当前 SSID 或网络名称。
    Disconnected,          ///< 网络连接已断开。
    WifiConfigModeEnter,   ///< 已进入热点配网模式。
    WifiConfigModeExit,    ///< 已退出热点配网模式。
    ModemDetecting,        ///< 蜂窝扩展：正在探测模块。
    ModemErrorNoSim,       ///< 蜂窝扩展：未检测到 SIM。
    ModemErrorRegDenied,   ///< 蜂窝扩展：运营商注册被拒绝。
    ModemErrorInitFailed,  ///< 蜂窝扩展：模块初始化失败。
    ModemErrorTimeout      ///< 蜂窝扩展：操作超时。
};

/**
 * @brief 网络和 CPU 的功耗策略。
 */
enum class PowerSaveLevel {
    LOW_POWER,    ///< 最积极省电，允许更高网络延迟。
    BALANCED,     ///< 功耗和响应速度折中。
    PERFORMANCE,  ///< 关闭省电以获得最低延迟。
};

/**
 * @brief 网络事件回调；data 的含义由 event 决定。
 */
using NetworkEventCallback = std::function<void(NetworkEvent event, const std::string& data)>;

void* create_board();
class AudioCodec;
class Display;
/**
 * @brief 当前硬件板的单例抽象。
 *
 * 应用层只能通过本接口获取 Codec、屏幕、LED、电池和网络，具体 GPIO 与芯片
 * 初始化保留在板级实现中。DECLARE_BOARD 宏负责创建唯一具体板对象。
 */
class Board {
private:
    Board(const Board&) = delete; // 禁用拷贝构造函数
    Board& operator=(const Board&) = delete; // 禁用赋值操作

protected:
    /**
     * @brief 构造时加载或生成软件 UUID。
     */
    Board();
    /**
     * @brief 生成 RFC 4122 风格随机 UUID 文本。
     */
    std::string GenerateUuid();

    // 软件生成的设备唯一标识
    std::string uuid_;

public:
    /**
     * @brief 获取构建时注册的具体开发板实例。
     */
    static Board& GetInstance() {
        static Board* instance = static_cast<Board*>(create_board());
        return *instance;
    }

    virtual ~Board() = default;
    /**
     * @return 上报服务器的固定板型名称。
     */
    virtual std::string GetBoardType() = 0;
    /**
     * @return 保存在 NVS 中的软件设备 UUID。
     */
    virtual std::string GetUuid() { return uuid_; }
    /**
     * @return 背光控制器；无屏板返回 nullptr。
     */
    virtual Backlight* GetBacklight() { return nullptr; }
    /**
     * @brief 根据活动来源恢复背光并更新屏幕空闲计时。
     * @param user_initiated true 表示唤醒词、按键等用户主动操作，允许退出屏保；
     *                       false 表示网络、电量、通知或状态变化等后台活动，屏保显示期间
     *                       只能更新内容或恢复背光，不得切换到对话界面。
     * @details 无屏幕或未实现屏幕空闲策略的板型使用默认空实现。
     */
    virtual void WakeUpScreen(bool user_initiated = false) { (void)user_initiated; }
    /**
     * @brief 设置自动熄屏等待时间。
     * @param seconds 等待秒数；0 表示关闭自动熄屏。
     * @return 板型支持并成功应用配置时返回 true，否则返回 false。
     */
    virtual bool SetScreenAutoOffTimeout(int seconds) { (void)seconds; return false; }
    /**
     * @brief 获取当前自动熄屏等待时间。
     * @return 最近一次保存的等待秒数；是否启用由 IsScreenAutoOffEnabled() 返回，
     *         -1 表示板型不支持。
     */
    virtual int GetScreenAutoOffTimeout() const { return -1; }
    /**
     * @brief 设置是否允许无互动时进入屏保。
     * @param enabled true 开启屏保；false 关闭屏保。
     * @return 当前板型支持并成功应用时返回 true。
     */
    virtual bool SetScreensaverEnabled(bool enabled) { (void)enabled; return false; }
    /**
     * @return true 表示自动屏保已开启。
     */
    virtual bool IsScreensaverEnabled() const { return false; }
    /**
     * @return true 表示表盘屏保当前正在屏幕上显示，而不只是配置为允许进入。
     */
    virtual bool IsScreensaverActive() const { return false; }
    /**
     * @brief 设置是否允许无互动时自动关闭背光。
     * @param enabled true 开启自动熄屏；false 禁止自动熄屏。
     * @return 当前板型支持并成功应用时返回 true。
     */
    virtual bool SetScreenAutoOffEnabled(bool enabled) { (void)enabled; return false; }
    /**
     * @return true 表示自动熄屏已开启。
     */
    virtual bool IsScreenAutoOffEnabled() const { return false; }
    /**
     * @return LED 状态指示器；默认返回空实现。
     */
    virtual Led* GetLed();
    /**
     * @return 板载音频 Codec，调用者不取得所有权。
     */
    virtual AudioCodec* GetAudioCodec() = 0;
    /**
     * @brief 读取芯片温度。
     * @param esp32temp 输出摄氏度。
     */
    virtual bool GetTemperature(float& esp32temp);
    /**
     * @return 显示对象；无屏板返回空显示实现。
     */
    virtual Display* GetDisplay();
    /**
     * @return 可创建 HTTP、WebSocket、MQTT、UDP 的网络接口。
     */
    virtual NetworkInterface* GetNetwork() = 0;
    /**
     * @brief 异步启动网络连接或配网流程。
     */
    virtual void StartNetwork() = 0;
    /**
     * @brief 注册网络状态回调。
     */
    virtual void SetNetworkEventCallback(NetworkEventCallback callback) { (void)callback; }
    /**
     * @return 当前网络状态对应的 Font Awesome 图标 UTF-8 字符串。
     */
    virtual const char* GetNetworkStateIcon() = 0;
    /**
     * @brief 读取电池状态。
     * @param level 输出 0-100。
     * @param charging 输出充电状态。
     * @param discharging 输出放电状态。
     */
    virtual bool GetBatteryLevel(int &level, bool& charging, bool& discharging);
    /**
     * @return 芯片、Flash、内存和版本组成的 JSON 文本。
     */
    virtual std::string GetSystemInfoJson();
    /**
     * @brief 应用指定功耗策略。
     */
    virtual void SetPowerSaveLevel(PowerSaveLevel level) = 0;
    /**
     * @return 板型静态能力 JSON。
     */
    virtual std::string GetBoardJson() = 0;
    /**
     * @return 电池、温度和网络等实时状态 JSON。
     */
    virtual std::string GetDeviceStatusJson() = 0;
};

#define DECLARE_BOARD(BOARD_CLASS_NAME) \
void* create_board() { \
    return new BOARD_CLASS_NAME(); \
}

#endif // BOARD_H
