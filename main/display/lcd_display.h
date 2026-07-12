#ifndef LCD_DISPLAY_H
#define LCD_DISPLAY_H

#include "lvgl_display.h"
#include "gif/lvgl_gif.h"

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <font_emoji.h>

#include <atomic>
#include <memory>

#define PREVIEW_IMAGE_DURATION_MS 5000


/**
 * @file lcd_display.h
 * @brief 基于 esp_lcd 和 LVGL 的彩色 LCD 界面实现。
 */

/**
 * @brief 管理状态栏、表情、字幕、预览图和主题的 LCD 基类。
 */
class LcdDisplay : public LvglDisplay {
protected:
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
    lv_obj_t* chat_message_label_ = nullptr;
    lv_obj_t* screensaver_container_ = nullptr;
    lv_obj_t* screensaver_scale_ = nullptr;
    lv_obj_t* screensaver_time_group_ = nullptr;
    lv_obj_t* screensaver_time_label_ = nullptr;
    lv_obj_t* screensaver_seconds_label_ = nullptr;
    lv_obj_t* screensaver_date_group_ = nullptr;
    lv_obj_t* screensaver_lunar_date_label_ = nullptr;
    lv_obj_t* screensaver_solar_date_label_ = nullptr;
    lv_obj_t* screensaver_weekday_label_ = nullptr;
    lv_obj_t* screensaver_weather_group_ = nullptr;
    lv_obj_t* screensaver_weather_temperature_label_ = nullptr;
    lv_obj_t* screensaver_weather_description_label_ = nullptr;
    lv_obj_t* screensaver_weather_range_label_ = nullptr;
    lv_obj_t* screensaver_todo_label_ = nullptr;
    lv_obj_t* screensaver_status_icon_group_ = nullptr;
    lv_obj_t* screensaver_network_label_ = nullptr;
    lv_obj_t* screensaver_battery_label_ = nullptr;
    lv_scale_section_t* screensaver_second_section_ = nullptr;
    esp_timer_handle_t preview_timer_ = nullptr;
    std::unique_ptr<LvglImage> preview_image_cached_ = nullptr;
    bool hide_subtitle_ = false;  ///< true 时不在屏幕显示对话字幕。
    bool screensaver_active_ = false;  ///< true 时金属黑表盘覆盖正常对话界面。

    /**
     * @brief 注册内置浅色/深色主题及资源主题。
     */
    void InitializeLcdThemes();
    /**
     * @brief 根据当前字幕的完整高度启动或停止纵向滚动动画。
     * @details 两行以内的字幕在容器中静止居中；超过两行时从顶部开始向上滚动，
     *          到达最后一行后停留并返回开头循环。调用者必须已经持有 LVGL 锁。
     */
    void UpdateSubtitleScroll();
    /**
     * @brief 创建金属黑圆形表盘及其刻度、时间、日期、天气和电量控件。
     * @details 控件只在 SetupUI() 中创建一次，后续通过隐藏标志切换，避免反复分配内存。
     */
    void CreateScreensaverUI();
    /**
     * @brief 将当前主题的完整中文字库应用到表盘的小号文本标签。
     * @param font LVGL 字体对象；为空时保持现有字体不变。
     * @details 资源字体通常为 30px，本方法会统一缩放到约 20px，既保证中文字符完整，
     *          又不改变圆形表盘已经确定的视觉布局。调用者必须已经持有 LVGL 锁。
     */
    void ApplyScreensaverTextFont(const lv_font_t* font);
    /**
     * @brief 将主题字体应用到天气三列，并保持 22px 字号与 20px 可见列间距。
     * @param font LVGL 字体对象；为空时保持现有字体不变。
     * @details 天气组以整体方式缩放和居中，三个子标签不单独缩放，避免列间距受文字长度影响。
     *          调用者必须已经持有 LVGL 锁。
     */
    void ApplyScreensaverWeatherFont(const lv_font_t* font);
    /**
     * @brief 将主题字体应用到日期三列，并保持 22px 字号与 25px 可见列间距。
     * @param font LVGL 字体对象；为空时保持现有字体不变。
     * @details 农历、公历和星期始终单行显示，并由日期组作为整体水平居中。
     *          调用者必须已经持有 LVGL 锁。
     */
    void ApplyScreensaverDateFont(const lv_font_t* font);
    /**
     * @brief 将普通状态栏使用的图标字体同步到屏保网络和电量标签。
     * @param font Font Awesome 图标字体；为空时保持现有字体不变。
     * @details 调用者必须已经持有 LVGL 锁。
     */
    void ApplyScreensaverStatusIconFont(const lv_font_t* font);
    /**
     * @brief 使用当前系统时间、网络状态和电池状态刷新表盘内容。
     * @details 调用者必须已经持有 LVGL 锁；农历由公历日期在本地换算，天气和待办区域
     *          在数据接口接入前显示占位内容。
     */
    void UpdateScreensaverContent();
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
     * @brief 在圆屏底部的两行可视窗口中显示对话字幕。
     * @param role 消息角色，默认圆屏布局中用于日志诊断。
     * @param content 完整 UTF-8 字幕内容；nullptr 或空字符串会隐藏字幕容器。
     * @details 两行以内的文本保持静止；超过两行的文本自动纵向滚动，确保完整答案
     *          可以依次显示，同时保持字幕容器尺寸固定，不遮挡中央表情。
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
     * @brief 设置是否隐藏字幕。
     * @param hide true 隐藏并清空当前字幕。
     */
    void SetHideSubtitle(bool hide);
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
