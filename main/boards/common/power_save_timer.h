#pragma once

#include <functional>

#include <esp_timer.h>
#include <esp_pm.h>

/**
 * @file power_save_timer.h
 * @brief 根据用户空闲时间降低 CPU/屏幕功耗。
 */

/**
 * @brief 周期检查活动状态，在睡眠和关机阈值触发应用回调。
 */
class PowerSaveTimer {
public:
    /**
     * @param cpu_max_freq 唤醒时允许的最高 CPU 频率 MHz。
     * @param seconds_to_sleep 无操作多少秒进入显示省电；-1 禁用。
     * @param seconds_to_shutdown 无操作多少秒请求关机；-1 禁用。
     */
    PowerSaveTimer(int cpu_max_freq, int seconds_to_sleep = 20, int seconds_to_shutdown = -1);
    ~PowerSaveTimer();

    /**
     * @brief 启停空闲计时；禁用时会立即退出睡眠模式。
     */
    void SetEnabled(bool enabled);
    /**
     * @brief 更新进入省电状态的空闲等待时间。
     * @param seconds 等待秒数；-1 表示关闭自动进入省电状态。
     * @details 修改后会立即退出当前省电状态并从零重新计时。
     */
    void SetSleepTimeout(int seconds);
    /**
     * @brief 设置首次进入省电状态时的回调。
     */
    void OnEnterSleepMode(std::function<void()> callback);
    /**
     * @brief 设置因用户活动退出省电状态时的回调。
     */
    void OnExitSleepMode(std::function<void()> callback);
    /**
     * @brief 设置达到关机阈值后的回调。
     */
    void OnShutdownRequest(std::function<void()> callback);
    /**
     * @brief 重置空闲计数并在必要时恢复 CPU 和屏幕。
     */
    void WakeUp();

private:
    /**
     * @brief 每秒检查状态、累计空闲时间并触发阈值。
     */
    void PowerSaveCheck();

    esp_timer_handle_t power_save_timer_ = nullptr;
    bool enabled_ = false;
    bool in_sleep_mode_ = false;
    bool is_wake_word_running_ = false;
    int ticks_ = 0;
    int cpu_max_freq_;
    int seconds_to_sleep_;
    int seconds_to_shutdown_;

    std::function<void()> on_enter_sleep_mode_;
    std::function<void()> on_exit_sleep_mode_;
    std::function<void()> on_shutdown_request_;
};
