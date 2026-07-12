#pragma once

#include "display.h"
#include "lvgl_image.h"
#include "lvgl_font.h"
#include "emoji_collection.h"

#include <lvgl.h>
#include <memory>
#include <map>
#include <string>


/**
 * @file lvgl_theme.h
 * @brief LVGL 颜色、字体、背景和表情资源主题。
 */

/**
 * @brief 一组可以统一应用到 LCD UI 的视觉资源。
 */
class LvglTheme : public Theme {
public:
    /**
     * @brief 解析 #RRGGBB 颜色文本。
     * @return LVGL 颜色值，非法输入回退黑色。
     */
    static lv_color_t ParseColor(const std::string& color);

    /**
     * @brief 以名称创建主题并填充安全默认值。
     */
    LvglTheme(const std::string& name);

    // 以下访问器读取主题属性，不转移图片、字体或表情集合的共享所有权。
    inline lv_color_t background_color() const { return background_color_; }
    inline lv_color_t text_color() const { return text_color_; }
    inline lv_color_t chat_background_color() const { return chat_background_color_; }
    inline lv_color_t user_bubble_color() const { return user_bubble_color_; }
    inline lv_color_t assistant_bubble_color() const { return assistant_bubble_color_; }
    inline lv_color_t system_bubble_color() const { return system_bubble_color_; }
    inline lv_color_t system_text_color() const { return system_text_color_; }
    inline lv_color_t border_color() const { return border_color_; }
    inline lv_color_t low_battery_color() const { return low_battery_color_; }
    inline std::shared_ptr<LvglImage> background_image() const { return background_image_; }
    inline std::shared_ptr<EmojiCollection> emoji_collection() const { return emoji_collection_; }
    inline std::shared_ptr<LvglFont> text_font() const { return text_font_; }
    inline std::shared_ptr<LvglFont> icon_font() const { return icon_font_; }
    inline std::shared_ptr<LvglFont> large_icon_font() const { return large_icon_font_; }
    inline int spacing(int scale) const { return spacing_ * scale; }

    inline void set_background_color(lv_color_t background) { background_color_ = background; }
    inline void set_text_color(lv_color_t text) { text_color_ = text; }
    inline void set_chat_background_color(lv_color_t chat_background) { chat_background_color_ = chat_background; }
    inline void set_user_bubble_color(lv_color_t user_bubble) { user_bubble_color_ = user_bubble; }
    inline void set_assistant_bubble_color(lv_color_t assistant_bubble) { assistant_bubble_color_ = assistant_bubble; }
    inline void set_system_bubble_color(lv_color_t system_bubble) { system_bubble_color_ = system_bubble; }
    inline void set_system_text_color(lv_color_t system_text) { system_text_color_ = system_text; }
    inline void set_border_color(lv_color_t border) { border_color_ = border; }
    inline void set_low_battery_color(lv_color_t low_battery) { low_battery_color_ = low_battery; }
    inline void set_background_image(std::shared_ptr<LvglImage> background_image) { background_image_ = background_image; }
    inline void set_emoji_collection(std::shared_ptr<EmojiCollection> emoji_collection) { emoji_collection_ = emoji_collection; }
    inline void set_text_font(std::shared_ptr<LvglFont> text_font) { text_font_ = text_font; }
    inline void set_icon_font(std::shared_ptr<LvglFont> icon_font) { icon_font_ = icon_font; }
    inline void set_large_icon_font(std::shared_ptr<LvglFont> large_icon_font) { large_icon_font_ = large_icon_font; }

private:
    int spacing_ = 2;

    // Colors
    lv_color_t background_color_;
    lv_color_t text_color_;
    lv_color_t chat_background_color_;
    lv_color_t user_bubble_color_;
    lv_color_t assistant_bubble_color_;
    lv_color_t system_bubble_color_;
    lv_color_t system_text_color_;
    lv_color_t border_color_;
    lv_color_t low_battery_color_;

    // Background image
    std::shared_ptr<LvglImage> background_image_ = nullptr;

    // fonts
    std::shared_ptr<LvglFont> text_font_ = nullptr;
    std::shared_ptr<LvglFont> icon_font_ = nullptr;
    std::shared_ptr<LvglFont> large_icon_font_ = nullptr;

    // Emoji collection
    std::shared_ptr<EmojiCollection> emoji_collection_ = nullptr;
};


/**
 * @brief 全局主题注册表，负责创建默认主题并按名称查找。
 */
class LvglThemeManager {
public:
    static LvglThemeManager& GetInstance() {
        static LvglThemeManager instance;
        return instance;
    }

    /**
     * @brief 注册或替换主题。
     * @param theme_name 查找键。
     * @param theme 管理器不负责释放。
     */
    void RegisterTheme(const std::string& theme_name, LvglTheme* theme);
    /**
     * @brief 按名称查找主题。
     * @return 不存在时返回默认主题。
     */
    LvglTheme* GetTheme(const std::string& theme_name);

private:
    LvglThemeManager();
    void InitializeDefaultThemes();

    std::map<std::string, LvglTheme*> themes_;
};
