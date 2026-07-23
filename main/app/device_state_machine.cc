/**
 * @file device_state_machine.cc
 * @brief device_state_machine.cc 中各类和辅助函数的具体实现。
 */
#include "app/device_state_machine.h"

#include <algorithm>
#include <esp_log.h>

static const char* TAG = "StateMachine";

// State name strings for logging
static const char* const STATE_STRINGS[] = {
    "unknown",
    "starting",
    "wifi_configuring",
    "idle",
    "connecting",
    "listening",
    "speaking",
    "upgrading",
    "activating",
    "audio_testing",
    "fatal_error",
    "invalid_state"
};

/**
 * @brief 构造 DeviceStateMachine 对象并初始化该模块运行所需的成员和系统资源。
 * @details 构造阶段只建立本模块自身资源；需要异步运行的任务由后续 Start 或 Initialize 方法启动。
 */
DeviceStateMachine::DeviceStateMachine() {
}

/**
 * @brief 获取适合日志输出的状态名称。
 * @param state 状态枚举。
 * @return 静态字符串。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
const char* DeviceStateMachine::GetStateName(DeviceState state) {
    if (state >= 0 && state <= kDeviceStateFatalError) {
        return STATE_STRINGS[state];
    }
    return STATE_STRINGS[kDeviceStateFatalError + 1];
}

/**
 * @brief 查询固定状态转移表。
 * @param from 源状态。
 * @param to 目标状态。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
bool DeviceStateMachine::IsValidTransition(DeviceState from, DeviceState to) const {
    // Allow transition to the same state (no-op)
    if (from == to) {
        return true;
    }

    // Define valid state transitions based on the state diagram
    switch (from) {
        case kDeviceStateUnknown:
            // Can only go to starting
            return to == kDeviceStateStarting;

        case kDeviceStateStarting:
            // Can go to wifi configuring or activating
            return to == kDeviceStateWifiConfiguring ||
                   to == kDeviceStateActivating;

        case kDeviceStateWifiConfiguring:
            // Can go to activating (after wifi connected) or audio testing
            return to == kDeviceStateActivating ||
                   to == kDeviceStateAudioTesting;

        case kDeviceStateAudioTesting:
            // Can go back to wifi configuring
            return to == kDeviceStateWifiConfiguring;

        case kDeviceStateActivating:
            // Can go to upgrading, idle, or back to wifi configuring (on error)
            return to == kDeviceStateUpgrading ||
                   to == kDeviceStateIdle ||
                   to == kDeviceStateWifiConfiguring;

        case kDeviceStateUpgrading:
            // Can go to idle (upgrade failed) or activating
            return to == kDeviceStateIdle ||
                   to == kDeviceStateActivating;

        case kDeviceStateIdle:
            // Can go to connecting, listening (manual mode), speaking, activating, upgrading, or wifi configuring
            return to == kDeviceStateConnecting ||
                   to == kDeviceStateListening ||
                   to == kDeviceStateSpeaking ||
                   to == kDeviceStateActivating ||
                   to == kDeviceStateUpgrading ||
                   to == kDeviceStateWifiConfiguring;

        case kDeviceStateConnecting:
            // Can go to idle (failed) or listening (success)
            return to == kDeviceStateIdle ||
                   to == kDeviceStateListening;

        case kDeviceStateListening:
            // Can go to speaking or idle
            return to == kDeviceStateSpeaking ||
                   to == kDeviceStateIdle;

        case kDeviceStateSpeaking:
            // Can go to listening or idle
            return to == kDeviceStateListening ||
                   to == kDeviceStateIdle;

        case kDeviceStateFatalError:
            // Cannot transition out of fatal error
            return false;

        default:
            return false;
    }
}

/**
 * @brief 只检查从当前状态到目标状态是否允许，不执行切换。
 * @param target 待检查的目标状态。
 * @return 状态转移表允许该路径时返回 true。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
bool DeviceStateMachine::CanTransitionTo(DeviceState target) const {
    return IsValidTransition(current_state_.load(), target);
}

/**
 * @file device_state_machine.h
 * @brief 设备业务状态机，负责校验状态转移并通知观察者。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
bool DeviceStateMachine::TransitionTo(DeviceState new_state) {
    DeviceState old_state = current_state_.load();
    
    // No-op if already in the target state
    if (old_state == new_state) {
        return true;
    }

    // Validate transition
    if (!IsValidTransition(old_state, new_state)) {
        ESP_LOGW(TAG, "设备状态转换无效：%s -> %s",
                 GetStateName(old_state), GetStateName(new_state));
        return false;
    }

    // Perform transition
    current_state_.store(new_state);
    ESP_LOGI(TAG, "设备状态：%s -> %s",
             GetStateName(old_state), GetStateName(new_state));

    // Notify callback
    NotifyStateChange(old_state, new_state);
    return true;
}

/**
 * @brief 添加状态变化监听器。
 * @param callback 每次成功切换后同步调用的回调。
 * @return 监听器编号，后续可传给 RemoveStateChangeListener() 注销。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
int DeviceStateMachine::AddStateChangeListener(StateCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    int id = next_listener_id_++;
    listeners_.emplace_back(id, std::move(callback));
    return id;
}

/**
 *
 * @brief 按编号删除监听器。
 * @param listener_id AddStateChangeListener() 返回的编号。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void DeviceStateMachine::RemoveStateChangeListener(int listener_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    listeners_.erase(
        std::remove_if(listeners_.begin(), listeners_.end(),
            [listener_id](const auto& p) { return p.first == listener_id; }),
        listeners_.end());
}

/**
 *
 * @brief 复制监听器快照并逐个通知，避免持锁执行用户回调。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void DeviceStateMachine::NotifyStateChange(DeviceState old_state, DeviceState new_state) {
    std::vector<StateCallback> callbacks_copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callbacks_copy.reserve(listeners_.size());
        for (const auto& [id, cb] : listeners_) {
            callbacks_copy.push_back(cb);
        }
    }
    
    for (const auto& cb : callbacks_copy) {
        cb(old_state, new_state);
    }
}
