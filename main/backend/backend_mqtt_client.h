#ifndef TUNTUN_BACKEND_MQTT_CLIENT_H
#define TUNTUN_BACKEND_MQTT_CLIENT_H

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <atomic>
#include <memory>

#include <mqtt.h>

#include "backend_models.h"

/**
 * @file backend_mqtt_client.h
 * @brief 与小智协议完全隔离的吞吞生活业务 MQTT 客户端。
 */

/**
 * @brief 管理业务 Broker 连接，并用固定队列隔离 MQTT 回调与业务 Worker。
 *
 * 本类不创建额外业务任务。底层 MQTT 组件维护网络循环，收到消息后仅验证主题、
 * 载荷大小和三个提示字段，再把 POD 结构无等待写入长度为 8 的 FreeRTOS 队列。
 */
class BackendMqttClient {
public:
    /**
     * @brief 创建固定容量事件提示队列。
     */
    BackendMqttClient();

    /**
     * @brief 断开独立业务连接并释放提示队列。
     */
    ~BackendMqttClient();

    BackendMqttClient(const BackendMqttClient&) = delete;
    BackendMqttClient& operator=(const BackendMqttClient&) = delete;

    /**
     * @brief 应用后端下发配置并建立独立 MQTT 连接。
     * @param config 已通过认证 HTTPS 获取的 Broker、账号和专属主题配置。
     * @return 配置合法且初次连接成功时返回 true；底层后续仍可自动重连。
     * @details 当前 ESP 网络抽象按 8883 端口启用 TLS，因此 tls=true 时只接受 8883，
     *          防止后端误配置导致凭据通过明文连接发送。
     */
    bool Start(const BackendMqttConfig& config);

    /**
     * @brief 主动停止业务连接并清空未处理提示。
     */
    void Stop();

    /**
     * @brief 查询底层业务 MQTT 是否已经连接 Broker。
     * @return 已连接时返回 true。
     */
    bool IsConnected() const;

    /**
     * @brief 从固定队列取出最早的一条轻量提示。
     * @param hint 成功时写入事件 UUID、类型和版本。
     * @return 队列存在且至少包含一条提示时返回 true。
     */
    bool TryPop(BackendEventHint& hint);

    /**
     * @brief 消费“连接或重连成功”标志。
     * @return 自上次消费后至少发生过一次连接成功时返回 true。
     * @details Worker 用该标志触发 HTTPS 全量补偿，不能把 MQTT 恢复本身视为数据完整。
     */
    bool ConsumeConnectedSignal();

private:
    /**
     * @brief 验证并压入一条不超过 1 KB 的 MQTT 通知。
     * @param topic 底层回调提供的完整主题。
     * @param payload UTF-8 JSON 轻量通知。
     */
    void HandleMessage(const std::string& topic, const std::string& payload);

    /**
     * @brief 将协议字符串转换为设备内部枚举。
     * @param value 后端稳定类型值，例如 reminder.schedule_changed。
     * @return 已知类型的枚举；未知值返回 BackendEventType::Unknown。
     */
    static BackendEventType ParseEventType(const char* value);

    std::unique_ptr<Mqtt> mqtt_;
    QueueHandle_t event_queue_ = nullptr;
    std::string event_topic_;
    int qos_ = 1;
    std::atomic<bool> connected_signal_{false};
};

#endif  // TUNTUN_BACKEND_MQTT_CLIENT_H
