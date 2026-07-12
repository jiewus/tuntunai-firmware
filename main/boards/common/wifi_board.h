#ifndef WIFI_BOARD_H
#define WIFI_BOARD_H

#include "board.h"
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <esp_timer.h>

/**
 * @file wifi_board.h
 * @brief Wi-Fi 开发板的连接、配网和省电公共实现。
 */

/**
 * @brief 使用 esp-wifi-connect 管理 STA 连接和 SoftAP 配网的板级基类。
 */
class WifiBoard : public Board {
protected:
    esp_timer_handle_t connect_timer_ = nullptr;
    bool in_config_mode_ = false;
    NetworkEventCallback network_event_callback_ = nullptr;

    virtual std::string GetBoardJson() override;

    /**
     * @brief 转发 Wi-Fi 管理器事件到应用层。
     * @param event 网络事件类型。
     * @param data 附加信息；连接相关事件中通常为 SSID。
     */
    void OnNetworkEvent(NetworkEvent event, const std::string& data = "");

    /**
     *
     * @brief 读取已保存凭据并发起一次非阻塞 STA 连接。
     */
    void TryWifiConnect();

    /**
     *
     * @brief 停止 STA 重试并启动 SoftAP、DNS 和配网页面。
     */
    void StartWifiConfigMode();

    /**
     *
     * @brief 连接超时定时器入口。
     * @param arg 指向 WifiBoard 实例。
     */
    static void OnWifiConnectTimeout(void* arg);

public:
    WifiBoard();
    virtual ~WifiBoard();
    
    virtual std::string GetBoardType() override;
    
    /**
     * @brief 异步启动 Wi-Fi。
     *
     * 有已保存网络时尝试连接，否则进入配网模式。方法立即返回，结果通过
     * SetNetworkEventCallback() 注册的回调通知。
     */
    virtual void StartNetwork() override;
    
    /**
     * @return Wi-Fi 网络接口，用于创建 HTTP/WebSocket/MQTT/UDP 客户端。
     */
    virtual NetworkInterface* GetNetwork() override;
    virtual void SetNetworkEventCallback(NetworkEventCallback callback) override;
    virtual const char* GetNetworkStateIcon() override;
    /**
     * @brief 将 Wi-Fi 固定为关闭 Modem Sleep 的高性能模式。
     * @param level 应用层期望的功耗等级；当前稳定性优先策略会忽略该值，始终使用
     *              WifiPowerSaveLevel::PERFORMANCE，对应底层 WIFI_PS_NONE。
     */
    virtual void SetPowerSaveLevel(PowerSaveLevel level) override;
    virtual AudioCodec* GetAudioCodec() override { return nullptr; }
    virtual std::string GetDeviceStatusJson() override;
    
    /**
     *
     * @brief 线程安全地请求进入 Wi-Fi 配网模式，可从按键任务调用。
     */
    void EnterWifiConfigMode();
    
    /**
     *
     * @brief 查询 SoftAP 配网服务是否正在运行。
     */
    bool IsInWifiConfigMode() const;
};

#endif // WIFI_BOARD_H
