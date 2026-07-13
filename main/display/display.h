#ifndef DISPLAY_H
#define DISPLAY_H

#include "emoji_collection.h"

#define HAVE_LVGL 1
#include <lvgl.h>

#include <esp_timer.h>
#include <esp_log.h>
#include <esp_pm.h>

#include <string>
#include <chrono>
#include <vector>

/**
 * @file display.h
 * @brief 显示系统的通用主题、界面接口和锁守卫。
 */

/**
 * @brief 所有显示主题的基础类型，仅保存稳定主题名称。
 */
class Theme {
public:
    /**
     * @param name 主题注册和持久化使用的唯一名称。
     */
    Theme(const std::string& name) : name_(name) {}
    virtual ~Theme() = default;

    inline std::string name() const { return name_; }
private:
    std::string name_;
};

/**
 * @brief 显示设备抽象。
 *
 * 公开方法允许应用更新状态、通知、表情和字幕；具体实现必须使用 Lock()/Unlock()
 * 串行化 LVGL 调用，因为 LVGL 默认不是线程安全的。
 */
class Display {
public:
    Display();
    virtual ~Display();

    /**
     * @brief 更新主状态文字。
     * @param status UTF-8 文本，方法会在需要时复制。
     */
    virtual void SetStatus(const char* status);
    /**
     * @brief 临时覆盖状态区域显示通知。
     * @param duration_ms 自动隐藏延时，单位 ms。
     */
    virtual void ShowNotification(const char* notification, int duration_ms = 3000);
    /**
     * @brief std::string 版本的通知接口。
     */
    virtual void ShowNotification(const std::string &notification, int duration_ms = 3000);
    /**
     * @brief 切换助手表情。
     * @param emotion 资源索引中的表情名称。
     */
    virtual void SetEmotion(const char* emotion);
    /**
     * @brief 显示一条对话字幕。
     * @param role user/assistant/system。
     * @param content UTF-8 正文。
     */
    virtual void SetChatMessage(const char* role, const char* content);
    /**
     * @brief 清除当前字幕/聊天内容。
     */
    virtual void ClearChatMessages();
    /**
     * @brief 切换主题。
     * @param theme 由主题管理器持有，不能在显示使用期间释放。
     */
    virtual void SetTheme(Theme* theme);
    virtual Theme* GetTheme() { return current_theme_; }
    /**
     * @brief 刷新网络、电池、静音和时钟。
     * @param update_all true 强制刷新未变化字段。
     */
    virtual void UpdateStatusBar(bool update_all = false);
    /**
     * @brief 切换显示省电状态。
     * @param on true 时降低刷新或关闭面板。
     */
    virtual void SetPowerSaveMode(bool on);
    /**
     * @brief 进入或退出常亮屏保界面。
     * @param enabled true 显示屏保并覆盖正常对话界面；false 隐藏屏保并恢复原界面。
     * @details 默认实现不执行任何操作，带屏幕的具体显示类负责创建和维护屏保内容。
     */
    virtual void SetScreensaverMode(bool enabled);
    /**
     * @brief 更新屏保天气位置和天气三列内容。
     * @param location 当前天气位置名称，例如“上海市松江区”。
     * @param temperature 当前温度，单位为摄氏度。
     * @param weather 中文天气描述。
     * @param low_temperature 当天最低温度，单位为摄氏度。
     * @param high_temperature 当天最高温度，单位为摄氏度。
     * @details 默认实现为空操作，只有带天气表盘的显示类需要覆盖。
     */
    virtual void SetScreensaverWeather(const std::string& location, int temperature,
                                       const std::string& weather,
                                       int low_temperature, int high_temperature) {
        (void)location;
        (void)temperature;
        (void)weather;
        (void)low_temperature;
        (void)high_temperature;
    }
    /**
     * @brief 替换屏保循环展示的备忘录数组。
     * @param memos 已按后端优先级排序的正文数组，最多使用前 5 条。
     * @details 默认实现为空操作，具体 LCD 实现负责复制正文并重置轮播位置。
     */
    virtual void SetScreensaverMemos(const std::vector<std::string>& memos) {
        (void)memos;
    }
    /**
     * @brief 创建基础 UI；子类覆盖时必须设置 setup_ui_called_。
     */
    virtual void SetupUI() { 
        setup_ui_called_ = true;
    }

    inline int width() const { return width_; }
    inline int height() const { return height_; }
    inline bool IsSetupUICalled() const { return setup_ui_called_; }

protected:
    int width_ = 0;
    int height_ = 0;
    bool setup_ui_called_ = false;  // Track if SetupUI() has been called

    Theme* current_theme_ = nullptr;

    friend class DisplayLockGuard;
    virtual bool Lock(int timeout_ms = 0) = 0;
    virtual void Unlock() = 0;
};


/**
 * @brief RAII 显示锁；构造最多等待 30 秒，析构自动解锁。
 */
class DisplayLockGuard {
public:
    DisplayLockGuard(Display *display) : display_(display) {
        if (!display_->Lock(30000)) {
            ESP_LOGE("Display", "Failed to lock display");
        }
    }
    ~DisplayLockGuard() {
        display_->Unlock();
    }

private:
    Display *display_;
};

/**
 * @brief 无屏设备的空显示实现。
 */
class NoDisplay : public Display {
private:
    virtual bool Lock(int timeout_ms = 0) override {
        return true;
    }
    virtual void Unlock() override {}
};

#endif
