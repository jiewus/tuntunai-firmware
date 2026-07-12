/**
 * @file power_save_timer.cc
 * @brief power_save_timer.cc 中各类和辅助函数的具体实现。
 */
#include "power_save_timer.h"
#include "application.h"
#include "settings.h"

#include <esp_log.h>

#define TAG "PowerSaveTimer"


/**
 * @brief 构造 PowerSaveTimer 对象并初始化该模块运行所需的成员和系统资源。
 * @details 构造阶段只建立本模块自身资源；需要异步运行的任务由后续 Start 或 Initialize 方法启动。
 */
PowerSaveTimer::PowerSaveTimer(int cpu_max_freq, int seconds_to_sleep, int seconds_to_shutdown)
    : cpu_max_freq_(cpu_max_freq), seconds_to_sleep_(seconds_to_sleep), seconds_to_shutdown_(seconds_to_shutdown) {
    esp_timer_create_args_t timer_args = {
        .callback = [](void* arg) {
            auto self = static_cast<PowerSaveTimer*>(arg);
            self->PowerSaveCheck();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "power_save_timer",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &power_save_timer_));
}

/**
 * @brief 析构 PowerSaveTimer 对象并释放其持有的系统资源。
 * @details 释放顺序与创建顺序相反，先停止异步来源，再销毁句柄和动态内存，避免回调访问失效对象。
 */
PowerSaveTimer::~PowerSaveTimer() {
    esp_timer_stop(power_save_timer_);
    esp_timer_delete(power_save_timer_);
}

/**
 * @brief 启停空闲计时；禁用时会立即退出睡眠模式。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void PowerSaveTimer::SetEnabled(bool enabled) {
    if (enabled && !enabled_) {
        if (cpu_max_freq_ != -1) {
            Settings settings("wifi", false);
            if (!settings.GetBool("sleep_mode", true)) {
                ESP_LOGI(TAG, "Power save timer is disabled by settings");
                return;
            }
        }

        ticks_ = 0;
        enabled_ = enabled;
        ESP_ERROR_CHECK(esp_timer_start_periodic(power_save_timer_, 1000000));
        ESP_LOGI(TAG, "Power save timer enabled");
    } else if (!enabled && enabled_) {
        ESP_ERROR_CHECK(esp_timer_stop(power_save_timer_));
        enabled_ = enabled;
        WakeUp();
        ESP_LOGI(TAG, "Power save timer disabled");
    }
}

/**
 * @brief 更新进入省电状态的空闲等待时间。
 * @param seconds 等待秒数；-1 表示关闭自动进入省电状态。
 * @details 无论新旧值是否相同，都会恢复亮屏并从零重新计算空闲时间，确保运行时配置立即生效。
 */
void PowerSaveTimer::SetSleepTimeout(int seconds) {
    seconds_to_sleep_ = seconds;
    WakeUp();
}

/**
 * @brief 设置首次进入省电状态时的回调。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void PowerSaveTimer::OnEnterSleepMode(std::function<void()> callback) {
    on_enter_sleep_mode_ = callback;
}

/**
 * @brief 设置因用户活动退出省电状态时的回调。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void PowerSaveTimer::OnExitSleepMode(std::function<void()> callback) {
    on_exit_sleep_mode_ = callback;
}

/**
 * @brief 设置达到关机阈值后的回调。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void PowerSaveTimer::OnShutdownRequest(std::function<void()> callback) {
    on_shutdown_request_ = callback;
}

/**
 * @brief 每秒检查状态、累计空闲时间并触发阈值。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void PowerSaveTimer::PowerSaveCheck() {
    auto& app = Application::GetInstance();
    if (!in_sleep_mode_ && !app.CanEnterSleepMode()) {
        ticks_ = 0;
        return;
    }

    ticks_++;
    if (seconds_to_sleep_ != -1 && ticks_ >= seconds_to_sleep_) {
        if (!in_sleep_mode_) {
            ESP_LOGI(TAG, "Enabling power save mode");
            in_sleep_mode_ = true;
            if (on_enter_sleep_mode_) {
                on_enter_sleep_mode_();
            }

            if (cpu_max_freq_ != -1) {
                // Disable wake word detection
                auto& audio_service = app.GetAudioService();
                is_wake_word_running_ = audio_service.IsWakeWordRunning();
                if (is_wake_word_running_) {
                    audio_service.EnableWakeWordDetection(false);
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
                // Disable audio input
                auto codec = Board::GetInstance().GetAudioCodec();
                if (codec) {
                    codec->EnableInput(false);
                }

                esp_pm_config_t pm_config = {
                    .max_freq_mhz = cpu_max_freq_,
                    .min_freq_mhz = 40,
                    .light_sleep_enable = true,
                };
                esp_pm_configure(&pm_config);
            }
        }
    }
    if (seconds_to_shutdown_ != -1 && ticks_ >= seconds_to_shutdown_ && on_shutdown_request_) {
        on_shutdown_request_();
    }
}

/**
 * @brief 重置空闲计数并在必要时恢复 CPU 和屏幕。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void PowerSaveTimer::WakeUp() {
    ticks_ = 0;
    if (in_sleep_mode_) {
        ESP_LOGI(TAG, "Exiting power save mode");
        in_sleep_mode_ = false;

        if (cpu_max_freq_ != -1) {
            esp_pm_config_t pm_config = {
                .max_freq_mhz = cpu_max_freq_,
                .min_freq_mhz = cpu_max_freq_,
                .light_sleep_enable = false,
            };
            esp_pm_configure(&pm_config);

            // Enable wake word detection
            auto& app = Application::GetInstance();
            auto& audio_service = app.GetAudioService();
            if (is_wake_word_running_) {
                audio_service.EnableWakeWordDetection(true);
            }
        }

        if (on_exit_sleep_mode_) {
            on_exit_sleep_mode_();
        }
    }
}
