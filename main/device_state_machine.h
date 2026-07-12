#ifndef DEVICE_STATE_MACHINE_H
#define DEVICE_STATE_MACHINE_H

#include <atomic>
#include <functional>
#include <mutex>
#include <vector>

#include "device_state.h"

/**
 * @file device_state_machine.h
 * @brief 设备业务状态机，负责校验状态转移并通知观察者。
 */

/**
 * @brief 线程安全的设备状态机。
 *
 * 当前状态使用原子变量保存；监听器列表由互斥锁保护。回调在调用 TransitionTo()
 * 的任务上下文中同步执行，因此回调中不能长时间阻塞，也不要递归切换状态。
 */
class DeviceStateMachine {
public:
    DeviceStateMachine();
    ~DeviceStateMachine() = default;

    // 状态机不可复制，否则监听器和当前状态会产生歧义。
    DeviceStateMachine(const DeviceStateMachine&) = delete;
    DeviceStateMachine& operator=(const DeviceStateMachine&) = delete;

    /**
     * @brief 原子读取当前状态。
     * @return 当前 DeviceState，不需要外部加锁。
     */
    DeviceState GetState() const { return current_state_.load(); }

    /**
     * @brief 尝试切换到新状态。
     * @param new_state 目标状态。
     * @return 转移合法且状态已更新时返回 true；非法转移或目标等于当前状态时返回 false。
     */
    bool TransitionTo(DeviceState new_state);

    /**
     * @brief 只检查从当前状态到目标状态是否允许，不执行切换。
     * @param target 待检查的目标状态。
     * @return 状态转移表允许该路径时返回 true。
     */
    bool CanTransitionTo(DeviceState target) const;

    /**
     * @brief 状态变化回调类型。
     * @param old_state 切换前状态。
     * @param new_state 切换后状态。
     */
    using StateCallback = std::function<void(DeviceState, DeviceState)>;

    /**
     * @brief 添加状态变化监听器。
     * @param callback 每次成功切换后同步调用的回调。
     * @return 监听器编号，后续可传给 RemoveStateChangeListener() 注销。
     */
    int AddStateChangeListener(StateCallback callback);

    /**
     *
     * @brief 按编号删除监听器。
     * @param listener_id AddStateChangeListener() 返回的编号。
     */
    void RemoveStateChangeListener(int listener_id);

    /**
     *
     * @brief 获取适合日志输出的状态名称。
     * @param state 状态枚举。
     * @return 静态字符串。
     */
    static const char* GetStateName(DeviceState state);

private:
    std::atomic<DeviceState> current_state_{kDeviceStateUnknown};
    std::vector<std::pair<int, StateCallback>> listeners_;
    int next_listener_id_{0};
    std::mutex mutex_;

    /**
     *
     * @brief 查询固定状态转移表。
     * @param from 源状态。
     * @param to 目标状态。
     */
    bool IsValidTransition(DeviceState from, DeviceState to) const;

    /**
     *
     * @brief 复制监听器快照并逐个通知，避免持锁执行用户回调。
     */
    void NotifyStateChange(DeviceState old_state, DeviceState new_state);
};

#endif // DEVICE_STATE_MACHINE_H
