#ifndef LVGL_DISPLAY_H
#define LVGL_DISPLAY_H

#include "display.h"
#include "lvgl_image.h"

#include <lvgl.h>
#include <esp_timer.h>
#include <esp_log.h>
#include <esp_pm.h>

#include <string>
#include <chrono>

/**
 * @file lvgl_display.h
 * @brief 所有 LVGL 显示共有的状态栏和通知逻辑。
 */

/**
 * @brief 在 Display 接口上实现状态栏、通知定时器、省电锁和截图。
 */
class LvglDisplay : public Display {
public:
    LvglDisplay();
    virtual ~LvglDisplay();

    virtual void SetStatus(const char* status);
    virtual void ShowNotification(const char* notification, int duration_ms = 3000);
    virtual void ShowNotification(const std::string &notification, int duration_ms = 3000);
    virtual void SetPreviewImage(std::unique_ptr<LvglImage> image);
    /**
     * @brief 刷新顶部状态栏中的时间、网络和电量信息。
     * @param update_all true 表示立即读取全部状态，通常用于界面初始化或连接状态变化；
     * false 表示按周期策略刷新，网络图标每十次调用检查一次。
     * @details 时间标签每次调用都会比较并按需更新，但时间变化本身不会唤醒已熄灭的屏幕；
     * 网络、电量或静音图标发生变化时仍沿用原有的屏幕唤醒行为。
     */
    virtual void UpdateStatusBar(bool update_all = false);
    virtual void SetPowerSaveMode(bool on);
    /**
     * @brief 把当前 LVGL 活动屏幕渲染为 JPEG。
     * @param jpeg_data 输出完整 JPEG 字节串。
     * @param quality 编码质量 1-100，数值越高体积越大。
     * @return 快照和编码均成功时返回 true。
     */
    virtual bool SnapshotToJpeg(std::string& jpeg_data, int quality = 80) override;

protected:
    esp_pm_lock_handle_t pm_lock_ = nullptr;
    lv_display_t *display_ = nullptr;

    lv_obj_t *network_label_ = nullptr;
    /**
     * @brief 顶部状态栏中间的独立时间标签。
     * @details 该标签只由 UpdateStatusBar() 刷新，不与设备运行状态或临时通知共用，
     * 因此“待机”“聆听中”等状态变化不会覆盖当前时间。
     */
    lv_obj_t *time_label_ = nullptr;
    lv_obj_t *status_label_ = nullptr;
    lv_obj_t *notification_label_ = nullptr;
    lv_obj_t *mute_label_ = nullptr;
    lv_obj_t *battery_label_ = nullptr;
    lv_obj_t* low_battery_popup_ = nullptr;
    lv_obj_t* low_battery_label_ = nullptr;
    
    const char* battery_icon_ = nullptr;
    const char* network_icon_ = nullptr;
    bool muted_ = false;

    std::chrono::system_clock::time_point last_status_update_time_;
    esp_timer_handle_t notification_timer_ = nullptr;

    friend class DisplayLockGuard;
    /**
     * @brief 子类必须实现的 LVGL 锁。
     * @param timeout_ms 最大等待毫秒数。
     */
    virtual bool Lock(int timeout_ms = 0) = 0;
    virtual void Unlock() = 0;
};


#endif
