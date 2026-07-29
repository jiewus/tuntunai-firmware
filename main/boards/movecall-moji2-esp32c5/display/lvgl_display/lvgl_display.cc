/**
 * @file lvgl_display.cc
 * @brief lvgl_display.cc 中各类和辅助函数的具体实现。
 */
#include <esp_log.h>
#include <esp_err.h>
#include <string>
#include <cstdlib>
#include <cstring>
#include <font_awesome.h>

#include "lvgl_display.h"
#include "board.h"
#include "app/application.h"
#include "audio_codec.h"
#include "system/settings.h"
#include "assets/lang_config.h"
#include "jpg/image_to_jpeg.h"

#define TAG "Display"

/**
 * @brief 构造 LvglDisplay 对象并初始化该模块运行所需的成员和系统资源。
 * @details 构造阶段只建立本模块自身资源；需要异步运行的任务由后续 Start 或 Initialize 方法启动。
 */
LvglDisplay::LvglDisplay() {
    // Notification timer
    esp_timer_create_args_t notification_timer_args = {
        .callback = [](void *arg) {
            LvglDisplay *display = static_cast<LvglDisplay*>(arg);
            DisplayLockGuard lock(display);
            lv_obj_add_flag(display->notification_label_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(display->status_label_, LV_OBJ_FLAG_HIDDEN);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "notification_timer",
        .skip_unhandled_events = false,
    };
    ESP_ERROR_CHECK(esp_timer_create(&notification_timer_args, &notification_timer_));

    // Create a power management lock
    auto ret = esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, "display_update", &pm_lock_);
    if (ret == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGI(TAG, "Power management not supported");
    } else {
        ESP_ERROR_CHECK(ret);
    }
}

/**
 * @brief 析构 LvglDisplay 对象并释放其持有的系统资源。
 * @details 释放顺序与创建顺序相反，先停止异步来源，再销毁句柄和动态内存，避免回调访问失效对象。
 */
LvglDisplay::~LvglDisplay() {
    if (notification_timer_ != nullptr) {
        esp_timer_stop(notification_timer_);
        esp_timer_delete(notification_timer_);
    }

    if (network_label_ != nullptr) {
        lv_obj_del(network_label_);
    }
    if (time_label_ != nullptr) {
        lv_obj_del(time_label_);
    }
    if (notification_label_ != nullptr) {
        lv_obj_del(notification_label_);
    }
    if (status_label_ != nullptr) {
        lv_obj_del(status_label_);
    }
    if (mute_label_ != nullptr) {
        lv_obj_del(mute_label_);
    }
    if (battery_label_ != nullptr) {
        lv_obj_del(battery_label_);
    }
    if( low_battery_popup_ != nullptr ) {
        lv_obj_del(low_battery_popup_);
    }
    if (pm_lock_ != nullptr) {
        esp_pm_lock_delete(pm_lock_);
    }
}

/**
 * @brief 更新对应配置并同步到底层资源。
 * @details 实现会维护 LvglDisplay 的内部一致性；发生错误时记录日志，并避免向后续流程传播无效资源。
 */
void LvglDisplay::SetStatus(const char* status) {
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "SetStatus('%s') called before SetupUI() - message will be lost!", status);
    }
    DisplayLockGuard lock(this);
    if (status_label_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(TAG, "SetStatus('%s') failed: status_label_ is nullptr (SetupUI() was called but label not created)", status);
        }
        return;
    }
    lv_label_set_text(status_label_, status);
    lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    last_status_update_time_ = std::chrono::system_clock::now();
}

/**
 * @brief 执行 ShowNotification 对应的模块内部流程。
 * @details 实现会维护 LvglDisplay 的内部一致性；发生错误时记录日志，并避免向后续流程传播无效资源。
 */
void LvglDisplay::ShowNotification(const std::string &notification, int duration_ms) {
    ShowNotification(notification.c_str(), duration_ms);
}

/**
 * @brief 执行 ShowNotification 对应的模块内部流程。
 * @details 实现会维护 LvglDisplay 的内部一致性；发生错误时记录日志，并避免向后续流程传播无效资源。
 */
void LvglDisplay::ShowNotification(const char* notification, int duration_ms) {
    // 通知属于用户可感知的状态变化，需要立即亮屏并重置自动熄屏计时。
    Board::GetInstance().WakeUpScreen();

    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "ShowNotification('%s') called before SetupUI() - message will be lost!", notification);
    }
    DisplayLockGuard lock(this);
    if (notification_label_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(TAG, "ShowNotification('%s') failed: notification_label_ is nullptr (SetupUI() was called but label not created)", notification);
        }
        return;
    }
    lv_label_set_text(notification_label_, notification);
    lv_obj_remove_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);

    esp_timer_stop(notification_timer_);
    ESP_ERROR_CHECK(esp_timer_start_once(notification_timer_, duration_ms * 1000));
}

/**
 * @brief 刷新界面顶部的时间、网络、电量和可选静音信息。
 * @param update_all true 时立即读取网络等全部状态；false 时网络状态按十秒周期读取，
 *                   用于减少不必要的硬件查询和界面刷新。
 * @details 时间使用独立标签并始终按当前系统时间更新。时间刷新不会调用 WakeUpScreen()，
 *          因而不会破坏待机计时；网络、电量等状态变化可以恢复背光或刷新内容，但表盘
 *          已经显示时不会切换回普通对话界面。
 */
void LvglDisplay::UpdateStatusBar(bool update_all) {
    auto& app = Application::GetInstance();
    auto& board = Board::GetInstance();
    auto codec = board.GetAudioCodec();
    bool visible_status_changed = false;

    // 静音标签只在包含静音图标的布局中创建；圆屏三栏布局不会创建该标签。
    {
        DisplayLockGuard lock(this);
        if (mute_label_ != nullptr) {
            // 仅当音量的静音状态发生变化时更新图标，避免重复刷新 LVGL 对象。
            if (codec->output_volume() == 0 && !muted_) {
                muted_ = true;
                lv_label_set_text(mute_label_, FONT_AWESOME_VOLUME_XMARK);
                visible_status_changed = true;
            } else if (codec->output_volume() > 0 && muted_) {
                muted_ = false;
                lv_label_set_text(mute_label_, "");
                visible_status_changed = true;
            }
        }
    }

    /*
     * 时间拥有独立标签，因此无论设备处于待机、聆听还是播放状态都保持显示。
     * 系统尚未通过网络校准时间时显示占位符，避免把 1970 年的无效时间展示给用户。
     * 此处不设置 visible_status_changed，防止每分钟刷新时间时唤醒已经自动熄灭的屏幕。
     */
    if (time_label_ != nullptr) {
        char time_str[16] = "--:--";
        const time_t now = time(nullptr);
        struct tm local_time = {};
        if (localtime_r(&now, &local_time) != nullptr && local_time.tm_year >= 2025 - 1900) {
            strftime(time_str, sizeof(time_str), "%H:%M", &local_time);
        }

        DisplayLockGuard lock(this);
        if (strcmp(lv_label_get_text(time_label_), time_str) != 0) {
            lv_label_set_text(time_label_, time_str);
        }
    }

    esp_pm_lock_acquire(pm_lock_);
    // Update battery icon
    int battery_level;
    bool charging, discharging;
    const char* icon = nullptr;
    if (board.GetBatteryLevel(battery_level, charging, discharging)) {
        if (charging) {
            icon = FONT_AWESOME_BATTERY_BOLT;
        } else {
            const char* levels[] = {
                FONT_AWESOME_BATTERY_EMPTY, // 0-19%
                FONT_AWESOME_BATTERY_QUARTER,    // 20-39%
                FONT_AWESOME_BATTERY_HALF,    // 40-59%
                FONT_AWESOME_BATTERY_THREE_QUARTERS,    // 60-79%
                FONT_AWESOME_BATTERY_FULL, // 80-99%
                FONT_AWESOME_BATTERY_FULL, // 100%
            };
            icon = levels[battery_level / 20];
        }
        DisplayLockGuard lock(this);
        if (battery_label_ != nullptr && battery_icon_ != icon) {
            battery_icon_ = icon;
            lv_label_set_text(battery_label_, battery_icon_);
            visible_status_changed = true;
        }

        // Check low battery popup only when clock tick event is triggered
        // Because when initializing, the battery level is not ready yet.
        if (low_battery_popup_ != nullptr && !update_all) {
            if (strcmp(icon, FONT_AWESOME_BATTERY_EMPTY) == 0 && discharging) {
                if (lv_obj_has_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN)) { // Show if low battery popup is hidden
                    lv_obj_remove_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
                    visible_status_changed = true;
                    app.Schedule([&app]() {
                        app.PlaySound(Lang::Sounds::OGG_LOW_BATTERY);
                    });
                }
            } else {
                // Hide the low battery popup when the battery is not empty
                if (!lv_obj_has_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN)) { // Hide if low battery popup is shown
                    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
                }
            }
        }
    }

    // Update network icon every 10 seconds
    static int seconds_counter = 0;
    if (update_all || seconds_counter++ % 10 == 0) {
        // Don't read 4G network status during firmware upgrade to avoid occupying UART resources
        auto device_state = Application::GetInstance().GetDeviceState();
        static const std::vector<DeviceState> allowed_states = {
            kDeviceStateIdle,
            kDeviceStateStarting,
            kDeviceStateWifiConfiguring,
            kDeviceStateListening,
            kDeviceStateActivating,
        };
        if (std::find(allowed_states.begin(), allowed_states.end(), device_state) != allowed_states.end()) {
            icon = board.GetNetworkStateIcon();
            if (network_label_ != nullptr && icon != nullptr && network_icon_ != icon) {
                DisplayLockGuard lock(this);
                network_icon_ = icon;
                lv_label_set_text(network_label_, network_icon_);
                visible_status_changed = true;
            }
        }
    }

    esp_pm_lock_release(pm_lock_);

    if (visible_status_changed) {
        board.WakeUpScreen();
    }
}

/**
 * @brief 更新对应配置并同步到底层资源。
 * @details 实现会维护 LvglDisplay 的内部一致性；发生错误时记录日志，并避免向后续流程传播无效资源。
 */
void LvglDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
}

/**
 * @brief 更新对应配置并同步到底层资源。
 * @details 实现会维护 LvglDisplay 的内部一致性；发生错误时记录日志，并避免向后续流程传播无效资源。
 */
void LvglDisplay::SetPowerSaveMode(bool on) {
    if (on) {
        SetChatMessage("system", "");
        SetEmotion("sleepy");
    } else {
        SetChatMessage("system", "");
        SetEmotion("neutral");
    }
}

/**
 * @brief 把当前 LVGL 活动屏幕渲染为 JPEG。
 * @param jpeg_data 输出完整 JPEG 字节串。
 * @param quality 编码质量 1-100，数值越高体积越大。
 * @return 快照和编码均成功时返回 true。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
bool LvglDisplay::SnapshotToJpeg(std::string& jpeg_data, int quality) {
#if CONFIG_LV_USE_SNAPSHOT
    DisplayLockGuard lock(this);

    lv_obj_t* screen = lv_screen_active();
    lv_draw_buf_t* draw_buffer = lv_snapshot_take(screen, LV_COLOR_FORMAT_RGB565);
    if (draw_buffer == nullptr) {
        ESP_LOGE(TAG, "Failed to take snapshot, draw_buffer is nullptr");
        return false;
    }

    // swap bytes
    uint16_t* data = (uint16_t*)draw_buffer->data;
    size_t pixel_count = draw_buffer->data_size / 2;
    for (size_t i = 0; i < pixel_count; i++) {
        data[i] = __builtin_bswap16(data[i]);
    }

    // Clear output string and use callback version to avoid pre-allocating large memory blocks
    jpeg_data.clear();

    // Use callback-based JPEG encoder to further save memory
    bool ret = image_to_jpeg_cb((uint8_t*)draw_buffer->data, draw_buffer->data_size, draw_buffer->header.w, draw_buffer->header.h, V4L2_PIX_FMT_RGB565, quality,
        [](void *arg, size_t index, const void *data, size_t len) -> size_t {
        std::string* output = static_cast<std::string*>(arg);
        if (data && len > 0) {
            output->append(static_cast<const char*>(data), len);
        }
        return len;
    }, &jpeg_data);
    if (!ret) {
        ESP_LOGE(TAG, "Failed to convert image to JPEG");
    }

    lv_draw_buf_destroy(draw_buffer);
    return ret;
#else
    ESP_LOGE(TAG, "LV_USE_SNAPSHOT is not enabled");
    return false;
#endif
}
