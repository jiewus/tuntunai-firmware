#ifndef DISPLAY_H
#define DISPLAY_H

#include <esp_log.h>

#include <cstddef>
#include <string>
#include <vector>

class Assets;
struct cJSON;

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
    virtual bool SetThemeByName(const std::string& theme_name) {
        (void)theme_name;
        return false;
    }
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
     * @param location 当前天气位置的简短显示名称，例如“松江”。
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
     * @brief 使用已经格式化的文本更新屏保天气区域。
     * @param location 天气位置或天气服务状态说明。
     * @param temperature 当前温度文本，例如“21℃”或占位文本“--℃”。
     * @param weather 天气描述文本，例如“晴”或占位文本“--”。
     * @param temperature_range 最低和最高温度文本，例如“18/26”或“--/--”。
     * @details 该接口用于无法提供有效数值的加载、停用和开发中状态。默认实现为空操作，
     *          具体 LCD 显示类负责复制文本并安全更新 LVGL 控件。
     */
    virtual void SetScreensaverWeatherText(const std::string& location,
                                           const std::string& temperature,
                                           const std::string& weather,
                                           const std::string& temperature_range) {
        (void)location;
        (void)temperature;
        (void)weather;
        (void)temperature_range;
    }
    /**
     * @brief 替换屏保循环展示的备忘录数组。
     * @param memos 已按后端优先级排序的屏保文本数组，最多使用前 5 条。
     * @details 每条结构化文本使用“时间换行正文”格式。默认实现为空操作，具体 LCD 实现负责
     *          复制文本、限制三行并重置轮播位置。
     */
    virtual void SetScreensaverMemos(const std::vector<std::string>& memos) {
        (void)memos;
    }
    /**
     * @brief 显示自定义 MCP 工具清单专用表盘。
     * @param title 固定显示在页面顶部的清单标题。
     * @param items 按顺序轮播的 MCP 名称和说明数组。
     * @details 专用表盘只保留背景、金属外圈和刻度，隐藏天气、日期、时间、备忘录以及
     *          网络和电量图标。默认实现为空操作，具体 LCD 实现负责单项轮播。
     */
    virtual void ShowCustomMcpList(
        const std::string& title,
        const std::vector<std::string>& items) {
        (void)title;
        (void)items;
    }
    /**
     * @brief 隐藏自定义 MCP 工具清单专用表盘。
     * @details 默认实现为空操作。带屏设备应恢复普通对话界面，并清理仍在运行的列表动画。
     */
    virtual void HideCustomMcpList() {
    }
    /**
     * @brief 显示设备绑定码专用页面。
     * @param binding_code 允许用户输入到网页端的短绑定码；为空时页面只显示流程状态。
     * @param message 绑定码下方的操作说明、成功提示或失败原因。
     * @details 默认实现为空操作。具体 LCD 实现应复制传入文本，并确保绑定页面位于普通对话
     *          和屏保页面之上。绑定会话 Token 与设备访问 Token 严禁通过该接口显示。
     */
    virtual void ShowDeviceBinding(const std::string& binding_code,
                                   const std::string& message) {
        (void)binding_code;
        (void)message;
    }
    /**
     * @brief 隐藏设备绑定码专用页面。
     * @details 默认实现为空操作。调用后应恢复绑定页面下方原本可见的屏保或对话页面。
     */
    virtual void HideDeviceBinding() {
    }
    /**
     * @brief 设置显示实现是否隐藏对话字幕。
     * @details 默认实现为空操作，无字幕显示能力的板型无需覆盖。
     */
    virtual void SetHideSubtitle(bool hide) {
        (void)hide;
    }
    /**
     * @brief 在资源分区解除映射前释放显示实现持有的资源引用。
     * @details 默认实现为空操作。引用 assets 分区数据的显示实现必须在此释放相关对象。
     */
    virtual void ReleaseAssetsForReload() {
    }
    /**
     * @brief 应用资源索引中与具体显示实现有关的字体、表情和皮肤配置。
     */
    virtual bool ApplyAssets(Assets& assets, cJSON* index, bool refresh_theme) {
        (void)assets;
        (void)index;
        (void)refresh_theme;
        return true;
    }
    /**
     * @brief 将当前画面编码为 JPEG；不支持截图的显示实现返回 false。
     */
    virtual bool SnapshotToJpeg(std::string& jpeg_data, int quality = 80) {
        (void)jpeg_data;
        (void)quality;
        return false;
    }
    /**
     * @brief 接管图片文件内存并临时预览。
     * @details 调用后 data 的所有权始终转移给显示实现。
     */
    virtual bool SetPreviewImageData(void* data, size_t size);
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
