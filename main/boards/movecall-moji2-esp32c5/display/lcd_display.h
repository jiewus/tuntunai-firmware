#ifndef LCD_DISPLAY_H
#define LCD_DISPLAY_H

#include "lvgl_display.h"
#include "gif/lvgl_gif.h"

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <font_emoji.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#define PREVIEW_IMAGE_DURATION_MS 5000

class LvglFont;

/**
 * @file lcd_display.h
 * @brief 基于 esp_lcd 和 LVGL 的彩色 LCD 界面实现。
 */

/**
 * @brief 管理状态栏、表情、字幕、预览图和主题的 LCD 基类。
 */
class LcdDisplay : public LvglDisplay {
protected:
    /** @brief NOMI 风格对话表情当前使用的运行状态。 */
    enum class ConversationFaceMode : uint8_t {
        Idle,
        Connecting,
        Listening,
        Speaking
    };
    /** @brief NOMI 风格双眼当前表达的情绪。 */
    enum class ConversationFaceExpression : uint8_t {
        Neutral,
        Happy,
        Sad,
        Angry,
        Thinking,
        Surprised,
        Relaxed,
        Sleepy
    };

    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    
    lv_draw_buf_t draw_buf_;
    lv_obj_t* top_bar_ = nullptr;
    lv_obj_t* status_bar_ = nullptr;
    lv_obj_t* content_ = nullptr;
    lv_obj_t* container_ = nullptr;
    lv_obj_t* side_bar_ = nullptr;
    lv_obj_t* bottom_bar_ = nullptr;
    lv_obj_t* preview_image_ = nullptr;
    lv_obj_t* emoji_label_ = nullptr;
    lv_obj_t* emoji_image_ = nullptr;
    std::unique_ptr<LvglGif> gif_controller_ = nullptr;
    lv_obj_t* emoji_box_ = nullptr;
    /** @brief NOMI 风格对话表情的黑色面板和左右眼。 */
    lv_obj_t* conversation_face_ = nullptr;
    lv_obj_t* conversation_left_eye_ = nullptr;
    lv_obj_t* conversation_right_eye_ = nullptr;
    /** @brief 驱动眨眼和状态表情切换的低频 LVGL 定时器。 */
    lv_timer_t* conversation_face_timer_ = nullptr;
    ConversationFaceMode conversation_face_mode_ = ConversationFaceMode::Idle;
    ConversationFaceExpression conversation_face_expression_ = ConversationFaceExpression::Neutral;
    uint32_t conversation_face_tick_ = 0;
    uint32_t conversation_face_last_frame_key_ = UINT32_MAX;
    lv_obj_t* chat_message_label_ = nullptr;
    lv_obj_t* screensaver_container_ = nullptr;
    /** 预渲染的金属表盘、圆环和固定刻度背景。 */
    lv_obj_t* screensaver_dial_image_ = nullptr;
    /**
     * @brief 独立绘制的当前秒刻度，仅移动自身以避免每秒重绘整个表盘刻度控件。
     */
    lv_obj_t* screensaver_second_marker_ = nullptr;
    /**
     * @brief 当前秒刻度使用的尺寸类型，0 为普通刻度，1 为五秒主刻度，-1 表示尚未配置。
     */
    int screensaver_second_marker_style_ = -1;
    /** @brief 上一次已经绘制的秒值，避免同一秒重复设置旋转和位置。 */
    int screensaver_last_rendered_second_ = -2;
    /** @brief 上一次已经写入日期、农历和星期的日期键。 */
    int screensaver_last_date_key_ = 0;
    /** @brief 上一次已经应用的电量告警颜色状态。 */
    bool screensaver_battery_attention_valid_ = false;
    bool screensaver_battery_attention_ = false;
    /** @brief 上一次已经应用的网络断开颜色状态。 */
    bool screensaver_network_disconnected_valid_ = false;
    bool screensaver_network_disconnected_ = false;
    lv_obj_t* screensaver_time_group_ = nullptr;
    lv_obj_t* screensaver_time_label_ = nullptr;
    lv_obj_t* screensaver_seconds_label_ = nullptr;
    lv_obj_t* screensaver_date_group_ = nullptr;
    lv_obj_t* screensaver_lunar_date_label_ = nullptr;
    lv_obj_t* screensaver_solar_date_label_ = nullptr;
    lv_obj_t* screensaver_weekday_label_ = nullptr;
    lv_obj_t* screensaver_weather_location_label_ = nullptr;
    lv_obj_t* screensaver_weather_group_ = nullptr;
    lv_obj_t* screensaver_weather_temperature_label_ = nullptr;
    lv_obj_t* screensaver_weather_description_label_ = nullptr;
    lv_obj_t* screensaver_weather_range_label_ = nullptr;
    /** @brief 屏保天气与日期专用的普惠体 Heavy 24px 字体。 */
    std::shared_ptr<LvglFont> screensaver_weather_font_;
    /**
     * @brief 位于时间下方、容纳三条弧形安全裁切带的透明固定视口。
     */
    lv_obj_t* screensaver_memo_viewport_ = nullptr;
    /**
     * @brief 从上到下排列的三条透明裁切带。
     * @details 三条裁切带宽度逐渐缩小，用分段圆弦近似圆屏下半部分的左右弧形。
     */
    lv_obj_t* screensaver_memo_row_viewports_[3] = {};
    /**
     * @brief 分别位于三条裁切带中的备忘录文本镜像。
     * @details 三个标签始终保存相同文本并执行相同纵向位移，各裁切带只显示属于自己的
     * 水平区域，组合后形成一段连续且顺应圆屏弧形的三行文本。
     */
    lv_obj_t* screensaver_memo_labels_[3] = {};
    /**
     * @brief 覆盖在备忘录首行上的日期时间粗体镜像。
     * @details 三个标签与正文镜像使用相同位置和裁切带，只绘制方括号包围的日期时间首行，
     * 从而在不加粗正文的前提下模拟点阵粗体。
     */
    lv_obj_t* screensaver_memo_date_labels_[3] = {};
    /**
     * @brief 驱动多条屏保备忘录循环切换的 LVGL 定时器。
     */
    lv_timer_t* screensaver_memo_timer_ = nullptr;
    /**
     * @brief 位于表盘 12 点位置、替代原主刻度并显示网络状态的图标标签。
     */
    lv_obj_t* screensaver_network_label_ = nullptr;
    /**
     * @brief 位于表盘 6 点位置、替代原主刻度并显示分级电量或充电状态的图标标签。
     */
    lv_obj_t* screensaver_battery_label_ = nullptr;
    /**
     * @brief 覆盖刻度内圆形安全区的自定义 MCP 清单透明视口。
     */
    lv_obj_t* screensaver_mcp_list_viewport_ = nullptr;
    /**
     * @brief 固定显示在 MCP 清单页面顶部的 30px 标题标签。
     */
    lv_obj_t* screensaver_mcp_list_title_label_ = nullptr;
    /**
     * @brief 承载当前单个 MCP 内容的圆角透明视口。
     */
    lv_obj_t* screensaver_mcp_list_item_viewport_ = nullptr;
    /**
     * @brief 在圆角视口中显示当前单个 MCP 名称和说明的文本标签。
     */
    lv_obj_t* screensaver_mcp_list_label_ = nullptr;
    /**
     * @brief 每 5 秒切换到下一项 MCP 的 LVGL 定时器。
     */
    lv_timer_t* screensaver_mcp_list_switch_timer_ = nullptr;
    /**
     * @brief 当前页面按后台清单顺序保存的 MCP 单项展示文本。
     */
    std::vector<std::string> custom_mcp_list_items_;
    /**
     * @brief 当前正在显示的 MCP 项目下标。
     */
    size_t custom_mcp_list_index_ = 0;
    /**
     * @brief 覆盖普通界面和屏保的设备绑定页面根容器。
     */
    lv_obj_t* binding_container_ = nullptr;
    /**
     * @brief 显示“设备绑定”的页面标题标签。
     */
    lv_obj_t* binding_title_label_ = nullptr;
    /**
     * @brief 承载大号绑定码并提供金属边框的固定尺寸容器。
     */
    lv_obj_t* binding_code_panel_ = nullptr;
    /**
     * @brief 显示可由用户输入网页端的短绑定码标签。
     */
    lv_obj_t* binding_code_label_ = nullptr;
    /**
     * @brief 显示绑定操作说明或流程结果的多行标签。
     */
    lv_obj_t* binding_message_label_ = nullptr;
    esp_timer_handle_t preview_timer_ = nullptr;
    std::unique_ptr<LvglImage> preview_image_cached_ = nullptr;
    bool hide_subtitle_ = false;  ///< true 时不在屏幕显示对话字幕。
    bool screensaver_active_ = false;  ///< true 时金属黑表盘覆盖正常对话界面。
    /** @brief true 时暂停表情、GIF 和文字滚动，为音频输出让出处理时间。 */
    bool audio_playback_mode_ = false;
    /**
     * @brief true 时表盘只显示外圈和自定义 MCP 清单。
     */
    std::atomic<bool> custom_mcp_list_active_{false};
    /**
     * @brief true 时设备绑定页面覆盖屏保和普通对话界面。
     */
    bool binding_active_ = false;
    /**
     * @brief 保存后端最近成功返回的最多 5 条备忘录正文副本。
     */
    std::vector<std::string> screensaver_memos_;
    /** @brief true 表示当前备忘录包含独立的日期时间行，日期显示在正文下方。 */
    bool screensaver_memo_has_reminder_line_ = false;
    /**
     * @brief 指向当前正在表盘时间下方全宽区域展示的备忘录下标。
     */
    size_t screensaver_memo_index_ = 0;

    /**
     * @brief 注册内置浅色/深色主题及资源主题。
     */
    void InitializeLcdThemes();
    /**
     * @brief 根据当前字幕的完整高度启动或停止纵向滚动动画。
     * @details 两行以内的字幕在容器中静止居中；超过两行时从顶部开始向上滚动，
     * 到达最后一行后停留并返回开头循环。调用者必须已经持有 LVGL 锁。
     */
    void UpdateSubtitleScroll();
    /**
     * @brief 创建金属黑圆形表盘及其刻度、时间、日期、天气和电量控件。
     * @details 控件只在 SetupUI() 中创建一次，后续通过隐藏标志切换，避免反复分配内存。
     */
    void CreateScreensaverUI();
    /**
     * @brief 创建圆屏设备绑定码覆盖页面。
     * @details 页面在 SetupUI() 中只创建一次，默认隐藏。后续仅更新标签与隐藏标志，
     * 避免绑定轮询期间重复创建 LVGL 对象和增加堆内存碎片。
     */
    void CreateDeviceBindingUI();
    /**
     * @brief 创建黑色背景上的 NOMI 风格双眼表情和低频动画定时器。
     */
    void CreateConversationFaceUI();
    /**
     * @brief 根据设备状态切换双眼的聆听、连接和播报动画。
     * @param status 当前设备状态文字。
     */
    void UpdateConversationFaceMode(const char* status);
    /**
     * @brief 根据云端情绪名称切换双眼表情。
     * @param emotion 云端下发的情绪名称。
     */
    void UpdateConversationFaceExpression(const char* emotion);
    /**
     * @brief 更新 NOMI 风格双眼的当前动画帧。
     * @details 仅在形态发生变化时更新两个眼睛对象，避免持续触发布局和整屏刷新。
     */
    void UpdateConversationFaceFrame();
    /**
     * @brief LVGL 定时器回调，刷新双眼的眨眼和状态动画。
     * @param timer user_data 指向当前 LcdDisplay 实例。
     */
    static void ConversationFaceTimerCallback(lv_timer_t* timer);
    /**
     * @brief 从资源分区加载屏保天气和日期专用字体。
     */
    void LoadScreensaverWeatherFont();
    /**
     * @brief 将当前主题的中文字体按绑定页面的信息层级应用到各标签。
     * @param font LVGL 字体对象；为空时保持现有字体不变。
     * @details 标题、六位码和说明文字分别换算为固定视觉字号，避免资源字体切换后页面
     * 突然放大或缩小。调用者必须已经持有 LVGL 锁。
     */
    void ApplyDeviceBindingFont(const lv_font_t* font);
    /**
     * @brief 将当前主题的完整中文字库应用到表盘的小号文本标签。
     * @param font LVGL 字体对象；为空时保持现有字体不变。
     * @details 资源字体通常为 30px，本方法会统一缩放到约 28px，既保证中文字符完整，
     * 又不改变圆形表盘已经确定的视觉布局。调用者必须已经持有 LVGL 锁。
     */
    void ApplyScreensaverTextFont(const lv_font_t* font);
    /**
     * @brief 将主题字体应用到天气位置名称，并保持单行水平居中。
     * @param font LVGL 字体对象；为空时保持现有字体不变。
     * @details 位置名称优先使用资源分区中的普惠体 Heavy 24px，资源不可用时回退到主题字体。
     * 字体不使用 LVGL 变换缩放。
     * 调用者必须已经持有 LVGL 锁。
     */
    void ApplyScreensaverLocationFont(const lv_font_t* font);
    /**
     * @brief 将屏保天气三列设置为普惠体 Heavy 24px，并保持固定列间距。
     * @param font LVGL 字体对象；为空时保持现有字体不变。
     * @details 天气三列优先使用资源分区中的普惠体 Heavy 24px，直接按原生字形绘制；
     * 三个子标签不单独缩放，避免列间距受文字长度影响。
     * 调用者必须已经持有 LVGL 锁。
     */
    void ApplyScreensaverWeatherFont(const lv_font_t* font);
    /**
     * @brief 将屏保日期三列设置为普惠体 Heavy 24px，并保持固定列间距。
     * @param font LVGL 字体对象；为空时保持现有字体不变。
     * @details 农历、公历和星期优先使用资源分区中的普惠体 Heavy 24px，直接按原生字形绘制，
     * 并由日期组作为整体水平居中。
     * 调用者必须已经持有 LVGL 锁。
     */
    void ApplyScreensaverDateFont(const lv_font_t* font);
    /**
     * @brief 将普通状态栏使用的图标字体同步到屏保网络和电量标签。
     * @param font Font Awesome 图标字体；为空时保持现有字体不变。
     * @details 调用者必须已经持有 LVGL 锁。
     */
    void ApplyScreensaverStatusIconFont(const lv_font_t* font);
    /**
     * @brief 将主题字体应用到自定义 MCP 清单，并保持固定的圆屏视觉字号和宽度。
     * @param font 当前主题的完整中文字库。
     */
    void ApplyCustomMcpListFont(const lv_font_t* font);
    /**
     * @brief 显示或隐藏普通表盘内容，同时反向切换自定义 MCP 清单视口。
     * @param visible true 显示天气、日期、时间、备忘录和状态图标。
     */
    void SetStandardScreensaverContentVisible(bool visible);
    /**
     * @brief 使用当前系统时间、网络状态和电池状态刷新表盘内容。
     * @details 调用者必须已经持有 LVGL 锁；农历由公历日期在本地换算，天气和待办区域
     * 在数据接口接入前显示占位内容。
     */
    void UpdateScreensaverContent();
    /**
     * @brief 根据当前下标刷新备忘录标签并计算下一条轮播等待时间。
     * @details 带时间首行的结构化备忘录固定显示三行并截断；兼容文本超过三行时继续纵向滚动。
     * 调用者必须持有 LVGL 锁。
     */
    void UpdateScreensaverMemo();
    /**
     * @brief 根据备忘录类型选择固定三行省略或兼容纵向滚动。
     * @details 带时间首行时最多显示“时间加两行正文”，不足三行会整体垂直居中；其他文本三行
     * 以内同样垂直居中，超过三行后三个镜像标签以普通字幕节奏同步滚动。两种模式均
     * 保留圆屏弧形安全边界。
     * 调用者必须持有 LVGL 锁。
     */
    void UpdateScreensaverMemoScroll();
    /**
     * @brief 使用同一个动画时钟同步更新三条裁切带中的备忘录纵向位移。
     * @param target 指向拥有三个备忘录镜像标签的 LcdDisplay 实例。
     * @param value 当前动画帧对应的 Y 轴物理像素位移。
     * @details 三个标签不再分别创建动画，避免独立动画在时间和整数像素取整上的微小差异
     * 造成裁切带交界处抖动。该回调由 LVGL 动画任务调用，调用期间已处于 LVGL 上下文。
     */
    static void ScreensaverMemoScrollAnimationCallback(void* target, int32_t value);
    /**
     * @brief 更新当前单个 MCP 文本并在剩余区域垂直居中。
     */
    void UpdateCustomMcpListItem();
    /**
     * @brief 每 5 秒切换到下一项 MCP。
     * @param timer user_data 指向当前 LcdDisplay 实例。
     */
    static void CustomMcpListSwitchTimerCallback(lv_timer_t* timer);
    /**
     * @brief LVGL 定时器回调，切换到下一条缓存备忘录。
     * @param timer user_data 指向当前 LcdDisplay 实例。
     */
    static void ScreensaverMemoTimerCallback(lv_timer_t* timer);
    /**
     * @brief 获取 LVGL 全局互斥锁。
     * @param timeout_ms 最大等待毫秒数。
     */
    virtual bool Lock(int timeout_ms = 0) override;
    /**
     * @brief 释放 LVGL 全局互斥锁。
     */
    virtual void Unlock() override;

protected:
    /**
     * @brief 保存面板句柄和逻辑尺寸，供具体总线子类完成 LVGL 注册。
     */
    LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width, int height);
    
public:
    ~LcdDisplay();
    /**
     * @brief 在静态 PNG 和 GIF 控制器之间切换表情。
     */
    virtual void SetEmotion(const char* emotion) override;
    /**
     * @brief 更新顶部状态并同步对话中心光环动画模式。
     * @param status UTF-8 状态文字。
     */
    virtual void SetStatus(const char* status) override;
    /**
     * @brief 在圆屏底部的两行可视窗口中显示对话字幕。
     * @param role 消息角色，默认圆屏布局中用于日志诊断。
     * @param content 完整 UTF-8 字幕内容；nullptr 或空字符串会隐藏字幕容器。
     * @details 两行以内的文本保持静止；超过两行的文本自动纵向滚动，确保完整答案
     * 可以依次显示，同时保持字幕容器尺寸固定，不遮挡中央表情。
     */
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void ClearChatMessages() override;
    /**
     * @brief 临时显示预览图并在 PREVIEW_IMAGE_DURATION_MS 后恢复表情。
     */
    virtual void SetPreviewImage(std::unique_ptr<LvglImage> image) override;
    /**
     * @brief 创建顶部状态、中央表情和底部两行字幕组成的圆屏布局。
     */
    virtual void SetupUI() override;
    /**
     * @brief 将主题颜色、字体、背景和表情集合应用到已创建控件。
     */
    virtual void SetTheme(Theme* theme) override;
    virtual bool SetThemeByName(const std::string& theme_name) override;
    virtual bool ApplyAssets(Assets& assets, cJSON* index, bool refresh_theme) override;
    virtual bool SetPreviewImageData(void* data, size_t size) override;
    /**
     * @brief 在资源分区即将重新映射前释放屏保专用字体并回退到主题字体。
     */
    void ReleaseAssetsForReload() override;
    /**
     * @brief 刷新普通状态栏，并在屏保显示期间同步刷新表盘时间和电量。
     * @param update_all true 强制刷新网络等全部状态；false 使用周期刷新策略。
     */
    virtual void UpdateStatusBar(bool update_all = false) override;
    /**
     * @brief 显示或隐藏金属黑时钟表盘屏保。
     * @param enabled true 进入屏保；false 退出屏保并恢复正常对话界面。
     */
    virtual void SetScreensaverMode(bool enabled) override;
    /**
     * @brief 暂停或恢复当前页面的非必要动画。
     * @param active true 进入音频优先模式，false 恢复当前页面所需动画。
     */
    void SetAudioPlaybackMode(bool active) override;
    /**
     * @brief 线程安全地更新表盘天气位置和天气三列。
     */
    virtual void SetScreensaverWeather(const std::string& location, int temperature,
                                       const std::string& weather,
                                       int low_temperature, int high_temperature) override;
    /**
     * @brief 线程安全地使用已格式化文本更新表盘天气区域。
     * @param location 天气位置或当前服务状态说明。
     * @param temperature 当前温度文本。
     * @param weather 天气描述文本。
     * @param temperature_range 最低和最高温度文本。
     */
    virtual void SetScreensaverWeatherText(const std::string& location,
                                           const std::string& temperature,
                                           const std::string& weather,
                                           const std::string& temperature_range) override;
    /**
     * @brief 线程安全地替换待办提醒缓存，并从第一条重新开始轮播。
     */
    virtual void SetScreensaverPendingReminders(const std::vector<std::string>& reminders) override;
    /**
     * @brief 显示只保留表盘外圈的自定义 MCP 工具清单页面。
     * @param title 固定显示在顶部的清单标题。
     * @param items 按 5 秒间隔轮播的 MCP 名称和说明数组。
     */
    virtual void ShowCustomMcpList(
        const std::string& title,
        const std::vector<std::string>& items) override;
    /**
     * @brief 退出自定义 MCP 清单页面并恢复普通对话界面。
     */
    virtual void HideCustomMcpList() override;
    /**
     * @brief 查询自定义 MCP 清单表盘当前是否处于激活显示状态。
     */
    virtual bool IsCustomMcpListActive() const override;
    /**
     * @brief 线程安全地显示设备绑定码页面并更新流程说明。
     * @param binding_code 用户需要输入网页端的短绑定码；为空时隐藏绑定码框。
     * @param message 页面底部显示的操作说明或流程结果。
     */
    virtual void ShowDeviceBinding(const std::string& binding_code,
                                   const std::string& message) override;
    /**
     * @brief 线程安全地隐藏设备绑定码页面并露出原有界面。
     */
    virtual void HideDeviceBinding() override;
    
    /**
     * @brief 设置是否隐藏字幕。
     * @param hide true 隐藏并清空当前字幕。
     */
    void SetHideSubtitle(bool hide) override;
};

/**
 * @brief SPI/QSPI LCD 构造器，创建 LVGL 显示并设置面板方向和偏移。
 */
class SpiLcdDisplay : public LcdDisplay {
public:
    /**
     * @param offset_x,offset_y 显存相对可见区偏移；mirror/swap 控制坐标变换。
     */
    SpiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                  int width, int height, int offset_x, int offset_y,
                  bool mirror_x, bool mirror_y, bool swap_xy);
};

/**
 * @brief 并行 RGB LCD 显示实现。
 */
class RgbLcdDisplay : public LcdDisplay {
public:
    RgbLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                  int width, int height, int offset_x, int offset_y,
                  bool mirror_x, bool mirror_y, bool swap_xy);
};

/**
 * @brief MIPI-DSI LCD 显示实现。
 */
class MipiLcdDisplay : public LcdDisplay {
public:
    MipiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                   int width, int height, int offset_x, int offset_y,
                   bool mirror_x, bool mirror_y, bool swap_xy);
};

#endif // LCD_DISPLAY_H
