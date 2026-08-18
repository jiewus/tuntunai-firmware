#ifndef _OTA_H
#define _OTA_H

#include <functional>
#include <string>

#include <esp_err.h>
#include "board.h"

/**
 * @file ota.h
 * @brief 设备版本检查、激活以及 HTTPS OTA 升级接口。
 */

/**
 * @brief 管理一次版本查询结果及后续激活/升级流程。
 */
class Ota {
public:
    /**
     * @brief 读取当前固件版本并初始化查询状态。
     */
    Ota();
    /**
     * @brief 释放查询过程中持有的资源。
     */
    ~Ota();

    /**
     * @brief 请求版本服务并解析 MQTT/WebSocket、激活和升级配置。
     */
    esp_err_t CheckVersion();
    /**
     * @brief 使用服务端 challenge 完成设备激活。
     */
    esp_err_t Activate();
    /**
     * @brief 以下 Has 方法用于判断对应字段是否由服务端有效下发。
     */
    bool HasActivationChallenge() { return has_activation_challenge_; }
    bool HasNewVersion() { return has_new_version_; }
    bool HasMqttConfig() { return has_mqtt_config_; }
    bool HasWebsocketConfig() { return has_websocket_config_; }
    bool HasActivationCode() { return has_activation_code_; }
    bool HasServerTime() { return has_server_time_; }
    /**
     * @brief 使用 CheckVersion() 得到的固件地址开始升级。
     * @param callback 进度回调；progress 为 0-100，speed 为每秒下载字节数。
     */
    bool StartUpgrade(std::function<void(int progress, size_t speed)> callback);
    /**
     * @brief 从任意 URL 执行 OTA 下载、校验和分区切换。
     * @param firmware_url 固件镜像的 HTTP/HTTPS 地址。
     * @param callback 下载进度和速度回调。
     * @return 写入并设置新启动分区成功时返回 true。
     */
    static bool Upgrade(const std::string& firmware_url, std::function<void(int progress, size_t speed)> callback);
    /**
     * @brief 将当前运行镜像标记为有效，取消 bootloader 回滚。
     */
    void MarkCurrentVersionValid();

    const std::string& GetFirmwareVersion() const { return firmware_version_; }
    const std::string& GetCurrentVersion() const { return current_version_; }
    const std::string& GetFirmwareUrl() const { return firmware_url_; }
    const std::string& GetActivationMessage() const { return activation_message_; }
    const std::string& GetActivationCode() const { return activation_code_; }
    /**
     * @brief 根据板型和配置生成版本检查服务 URL。
     */
    std::string GetCheckVersionUrl();

private:
    std::string activation_message_;
    std::string activation_code_;
    bool has_new_version_ = false;
    bool has_mqtt_config_ = false;
    bool has_websocket_config_ = false;
    bool has_server_time_ = false;
    bool has_activation_code_ = false;
    bool has_serial_number_ = false;
    bool has_activation_challenge_ = false;
    std::string current_version_;
    std::string firmware_version_;
    std::string firmware_url_;
    std::string activation_challenge_;
    std::string serial_number_;
    int activation_timeout_ms_ = 30000;

    std::function<void(int progress, size_t speed)> upgrade_callback_;
    /**
     * @brief 按数字段比较版本。
     * @return newVersion 更新时返回 true。
     */
    bool IsNewVersionAvailable(const std::string& currentVersion, const std::string& newVersion);
    /**
     * @brief 构造包含序列号、MAC 和 challenge 的激活请求 JSON。
     */
    std::string GetActivationPayload();
    /**
     * @brief 创建带设备请求头和 TLS 配置的 HTTP 客户端。
     */
    std::unique_ptr<Http> SetupHttp();
};

#endif // _OTA_H
