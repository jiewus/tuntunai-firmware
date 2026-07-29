/**
 * @file lcd_screensaver.cc
 * @brief 金属表盘屏保、天气、日期和备忘录页面实现。
 */
#include "lcd_display.h"
#include "lcd_display_utils.h"
#include "fonts/ui_fonts.h"
#include "gif/lvgl_gif.h"
#include "system/settings.h"
#include "lvgl_theme.h"
#include "lvgl_font.h"
#include "assets/lang_config.h"
#include "assets.h"

#include <array>
#include <vector>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <font_awesome.h>
#include <esp_log.h>
#include <esp_err.h>
#include <esp_lvgl_port.h>
#include <esp_psram.h>
#include <cstring>
#include <src/misc/cache/lv_cache.h>

#include "board.h"

#define TAG "LcdDisplay"

using namespace display::internal;

/**
 * @brief 将主题字体应用到备忘录正文，并将备忘录日期设置为 Heavy24 原生字体。
 * @param font 需要使用的 LVGL 字体对象；为空时不执行任何修改。
 * @details 默认资源分区提供 30px 完整中文字库，正文根据字体实际行高计算缩放比例，
 *          将视觉高度保持在约 28px；日期标签优先使用普惠体 Heavy 24px，直接按原生字形
 *          绘制，不使用变换缩放。
 */
void LcdDisplay::ApplyScreensaverTextFont(const lv_font_t* font) {
    if (font == nullptr || font->line_height <= 0) {
        return;
    }

    const int text_scale = kScreensaverSmallTextPixelSize * 256 / font->line_height;
    /*
     * 标签自身会在绘制时缩放，但 LVGL 的自动换行依据缩放前的逻辑宽度计算。这里使用
     * 向下取整，确保缩放后的最终宽度不会因不足 1px 的取整误差越过 228px 裁切视口。
     */
    const int todo_logical_width = kScreensaverMemoWidth * 256 / text_scale;
    const int logical_line_spacing =
        (kScreensaverMemoLineSpacing * 256 + text_scale - 1) / text_scale;
    const lv_font_t* memo_date_font = font;
    int memo_date_scale = text_scale;
    int memo_date_width = todo_logical_width;
    if (screensaver_weather_font_ != nullptr
        && screensaver_weather_font_->font() != nullptr) {
        memo_date_font = screensaver_weather_font_->font();
        memo_date_scale = 256;
        memo_date_width = kScreensaverMemoWidth;
    }
    for (lv_obj_t* label : screensaver_memo_labels_) {
        if (label == nullptr) {
            continue;
        }
        lv_obj_set_style_text_font(label, font, 0);
        lv_obj_set_style_transform_scale(label, text_scale, 0);
        lv_obj_set_style_transform_pivot_x(label, LV_PCT(50), 0);
        lv_obj_set_style_transform_pivot_y(label, 0, 0);
        lv_obj_set_style_text_line_space(label, logical_line_spacing, 0);
    }
    for (lv_obj_t* label : screensaver_memo_date_labels_) {
        if (label == nullptr) {
            continue;
        }
        lv_obj_set_style_text_font(label, memo_date_font, 0);
        lv_obj_set_style_transform_scale(label, memo_date_scale, 0);
        lv_obj_set_style_transform_pivot_x(label, LV_PCT(50), 0);
        lv_obj_set_style_transform_pivot_y(label, 0, 0);
    }
    if (screensaver_memo_viewport_ != nullptr) {
        lv_obj_set_size(screensaver_memo_viewport_, kScreensaverMemoWidth,
                        kScreensaverMemoViewportHeight);
        lv_obj_align(screensaver_memo_viewport_, LV_ALIGN_CENTER, 0,
                     kScreensaverBottomSectionOffsetY);
    }
    for (int row = 0; row < kScreensaverMemoVisibleLines; ++row) {
        if (screensaver_memo_row_viewports_[row] != nullptr) {
            lv_obj_set_size(screensaver_memo_row_viewports_[row],
                            kScreensaverMemoRowWidths[row], kScreensaverMemoRowHeight);
            lv_obj_align(screensaver_memo_row_viewports_[row], LV_ALIGN_TOP_MID, 0,
                         row * kScreensaverMemoRowHeight);
        }
        if (screensaver_memo_labels_[row] != nullptr) {
            lv_obj_set_width(screensaver_memo_labels_[row], todo_logical_width);
            lv_obj_align(screensaver_memo_labels_[row], LV_ALIGN_TOP_MID, 0,
                         -row * kScreensaverMemoRowHeight);
        }
        if (screensaver_memo_date_labels_[row] != nullptr) {
            lv_obj_set_width(screensaver_memo_date_labels_[row], memo_date_width);
            lv_obj_align(screensaver_memo_date_labels_[row], LV_ALIGN_TOP_MID, 0,
                         -row * kScreensaverMemoRowHeight);
        }
    }
}



/**
 * @brief 在普通表盘内容和自定义 MCP 清单之间切换。
 * @param visible true 显示普通表盘内容；false 只显示外圈、刻度和 MCP 清单。
 */
void LcdDisplay::SetStandardScreensaverContentVisible(bool visible) {
    lv_obj_t* standard_objects[] = {
        screensaver_weather_location_label_,
        screensaver_weather_group_,
        screensaver_date_group_,
        screensaver_time_group_,
        screensaver_memo_viewport_,
        screensaver_network_label_,
        screensaver_battery_label_,
    };
    for (lv_obj_t* object : standard_objects) {
        if (object == nullptr) {
            continue;
        }
        if (visible) {
            lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (screensaver_mcp_list_viewport_ != nullptr) {
        if (visible) {
            lv_obj_add_flag(screensaver_mcp_list_viewport_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(screensaver_mcp_list_viewport_, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/**
 * @brief 将完整主题字体应用到天气位置名称标签。
 * @param font 需要使用的 LVGL 字体对象；为空时不执行任何修改。
 * @details 位置名称优先使用资源分区中的普惠体 Heavy 24px，资源不可用时使用传入主题字体；
 *          固定 220px 可见宽度和单行裁切模式，不使用 LVGL 变换缩放。
 */
void LcdDisplay::ApplyScreensaverLocationFont(const lv_font_t* font) {
    if (font == nullptr || font->line_height <= 0 || screensaver_weather_location_label_ == nullptr) {
        return;
    }

    if (screensaver_weather_font_ != nullptr
        && screensaver_weather_font_->font() != nullptr) {
        font = screensaver_weather_font_->font();
    }
    lv_obj_set_width(screensaver_weather_location_label_, kScreensaverLocationWidth);
    lv_obj_set_style_text_font(screensaver_weather_location_label_, font, 0);
    lv_obj_align(screensaver_weather_location_label_, LV_ALIGN_CENTER, 0, -130);
}

/**
 * @brief 从资源分区加载屏保天气和日期专用的普惠体 Heavy 24px 字体。
 * @details 字体数据通过 assets 分区映射，不复制 700KB 字形数据到应用固件或运行内存；
 *          只为 LVGL 字体描述符和字形索引分配少量运行时内存。
 */
void LcdDisplay::LoadScreensaverWeatherFont() {
    if (screensaver_weather_font_ != nullptr) {
        return;
    }

    void* font_data = nullptr;
    size_t font_size = 0;
    if (!Assets::GetInstance().GetAssetData(
            "puhui3_heavy_24_2.bin", font_data, font_size)) {
        ESP_LOGW(TAG, "屏保 Heavy 24px 字体资源不存在，天气和日期使用主题字体");
        return;
    }

    auto heavy_font = std::make_shared<LvglCBinFont>(font_data);
    if (heavy_font->font() == nullptr) {
        ESP_LOGW(TAG, "屏保 Heavy 24px 字体加载失败，天气和日期使用主题字体");
        return;
    }
    screensaver_weather_font_ = std::move(heavy_font);
    ESP_LOGI(TAG, "屏保天气和日期已加载普惠体 Heavy 24px，资源大小=%u字节",
             static_cast<unsigned>(font_size));
}

/**
 * @brief 将完整主题字体应用到天气三列，并统一计算视觉字号和列间距。
 * @param font 需要使用的 LVGL 字体对象；为空时不执行任何修改。
 * @details 天气三列优先使用资源分区中的普惠体 Heavy 24px；本方法只设置字体和列间距，
 *          不改变字形大小。
 */
void LcdDisplay::ApplyScreensaverWeatherFont(const lv_font_t* font) {
    if (font == nullptr || font->line_height <= 0 || screensaver_weather_group_ == nullptr) {
        return;
    }

    if (screensaver_weather_font_ != nullptr
        && screensaver_weather_font_->font() != nullptr) {
        font = screensaver_weather_font_->font();
    }
    lv_obj_t* labels[] = {
        screensaver_weather_temperature_label_,
        screensaver_weather_description_label_,
        screensaver_weather_range_label_,
    };

    for (lv_obj_t* label : labels) {
        if (label != nullptr) {
            lv_obj_set_style_text_font(label, font, 0);
        }
    }

    lv_obj_set_style_pad_column(screensaver_weather_group_, kScreensaverWeatherColumnGap, 0);
    lv_obj_update_layout(screensaver_weather_group_);
    lv_obj_align(screensaver_weather_group_, LV_ALIGN_CENTER, 0, -96);
}

/**
 * @brief 将完整主题字体应用到农历、公历和星期三列。
 * @param font 需要使用的 LVGL 字体对象；为空时不执行任何修改。
 * @details 日期标签优先使用资源分区中的普惠体 Heavy 24px，三个标签使用自动内容宽度和裁切
 *          模式，不会自动换行。
 */
void LcdDisplay::ApplyScreensaverDateFont(const lv_font_t* font) {
    if (font == nullptr || font->line_height <= 0 || screensaver_date_group_ == nullptr) {
        return;
    }

    if (screensaver_weather_font_ != nullptr
        && screensaver_weather_font_->font() != nullptr) {
        font = screensaver_weather_font_->font();
    }
    lv_obj_t* labels[] = {
        screensaver_lunar_date_label_,
        screensaver_solar_date_label_,
        screensaver_weekday_label_,
    };

    for (lv_obj_t* label : labels) {
        if (label != nullptr) {
            lv_obj_set_style_text_font(label, font, 0);
        }
    }

    lv_obj_set_style_pad_column(screensaver_date_group_, kScreensaverDateColumnGap, 0);
    lv_obj_update_layout(screensaver_date_group_);
    lv_obj_align(screensaver_date_group_, LV_ALIGN_CENTER, 0, -60);
}

/**
 * @brief 将 Font Awesome 图标字体应用到表盘 12 点网络和 6 点电量标签。
 * @param font 普通页面状态栏正在使用的图标字体；为空时不执行任何修改。
 * @details 字体变化后重新对齐两个标签，使标签背景继续完整覆盖对应的主刻度。
 */
void LcdDisplay::ApplyScreensaverStatusIconFont(const lv_font_t* font) {
    if (font == nullptr) {
        return;
    }
    if (screensaver_network_label_ != nullptr) {
        lv_obj_set_style_text_font(screensaver_network_label_, font, 0);
    }
    if (screensaver_battery_label_ != nullptr) {
        lv_obj_set_style_text_font(screensaver_battery_label_, font, 0);
    }
    if (screensaver_network_label_ != nullptr) {
        lv_obj_update_layout(screensaver_network_label_);
        lv_obj_align(screensaver_network_label_, LV_ALIGN_TOP_MID, 0,
                     kScreensaverDialStatusInset);
    }
    if (screensaver_battery_label_ != nullptr) {
        lv_obj_update_layout(screensaver_battery_label_);
        lv_obj_align(screensaver_battery_label_, LV_ALIGN_BOTTOM_MID, 0,
                     -kScreensaverDialStatusInset);
    }
}

/**
 * @brief 创建参考运动手表风格的金属黑圆形屏保表盘。
 * @details 表盘使用两个深色圆形对象表现枪灰金属层次，使用 LVGL Scale 绘制 60 个刻度，
 *          不申请全屏 Canvas。农历由设备本地换算，天气由 BackendService 按屏保生命周期
 *          同步；待办区域在备忘录接口接入前显示占位内容。
 *          本方法只负责创建控件，调用者必须已经持有 LVGL 锁。
 */
void LcdDisplay::CreateScreensaverUI() {
    if (screensaver_container_ != nullptr) {
        return;
    }

    auto screen = lv_screen_active();
    const lv_font_t* text_font = &font_puhui_basic_20_4;
    const lv_font_t* icon_font = &BUILTIN_ICON_FONT;
    if (current_theme_ != nullptr) {
        auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
        if (lvgl_theme->text_font() != nullptr && lvgl_theme->text_font()->font() != nullptr) {
            text_font = lvgl_theme->text_font()->font();
        }
        if (lvgl_theme->icon_font() != nullptr && lvgl_theme->icon_font()->font() != nullptr) {
            icon_font = lvgl_theme->icon_font()->font();
        }
    }

    const lv_font_t* screensaver_weather_font = text_font;

    /*
     * 全屏根容器始终保持不透明，确保屏保不受普通主题背景、字幕和表情影响。
     * 物理屏幕为圆形，因此 360x360 方形容器的四角不会出现在可见区域。
     */
    screensaver_container_ = lv_obj_create(screen);
    lv_obj_set_size(screensaver_container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(screensaver_container_, 0, 0);
    lv_obj_set_style_bg_color(screensaver_container_, lv_color_hex(kScreensaverBackgroundColor), 0);
    lv_obj_set_style_bg_opa(screensaver_container_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(screensaver_container_, 0, 0);
    lv_obj_set_style_pad_all(screensaver_container_, 0, 0);
    lv_obj_remove_flag(screensaver_container_, LV_OBJ_FLAG_SCROLLABLE);

    /* 金属圆盘、内层和固定刻度在构建期预渲染，运行时只需复制 RGB565 像素。 */
    screensaver_dial_image_ = lv_image_create(screensaver_container_);
    lv_image_set_src(screensaver_dial_image_, &kScreensaverDialImage);
    lv_obj_center(screensaver_dial_image_);
    lv_obj_remove_flag(screensaver_dial_image_, LV_OBJ_FLAG_SCROLLABLE);

    /*
     * 秒刻度使用独立小对象。移动对象只会使旧、新位置失效，避免通过 Scale Section
     * 每秒重绘 330x330 的完整刻度盘。
     */
    screensaver_second_marker_ = lv_obj_create(screensaver_container_);
    lv_obj_set_size(screensaver_second_marker_, 3, 9);
    lv_obj_set_style_radius(screensaver_second_marker_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(
        screensaver_second_marker_, lv_color_hex(kScreensaverAccentColor), 0);
    lv_obj_set_style_bg_opa(screensaver_second_marker_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(screensaver_second_marker_, 0, 0);
    lv_obj_set_style_pad_all(screensaver_second_marker_, 0, 0);
    lv_obj_remove_flag(screensaver_second_marker_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(screensaver_second_marker_, LV_OBJ_FLAG_HIDDEN);

    /* 自定义 MCP 清单根容器只负责组合固定标题和单项轮播区域。 */
    screensaver_mcp_list_viewport_ = lv_obj_create(screensaver_container_);
    lv_obj_set_size(screensaver_mcp_list_viewport_, kCustomMcpListViewportWidth,
                    kCustomMcpListViewportHeight);
    lv_obj_set_style_bg_opa(screensaver_mcp_list_viewport_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(screensaver_mcp_list_viewport_, 0, 0);
    lv_obj_set_style_border_width(screensaver_mcp_list_viewport_, 0, 0);
    lv_obj_set_style_pad_all(screensaver_mcp_list_viewport_, 0, 0);
    lv_obj_set_scrollbar_mode(screensaver_mcp_list_viewport_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(screensaver_mcp_list_viewport_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(screensaver_mcp_list_viewport_, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_center(screensaver_mcp_list_viewport_);

    screensaver_mcp_list_title_label_ = lv_label_create(screensaver_mcp_list_viewport_);
    lv_obj_set_width(screensaver_mcp_list_title_label_, kCustomMcpListTitleWidth);
    lv_obj_set_height(screensaver_mcp_list_title_label_, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(screensaver_mcp_list_title_label_, text_font, 0);
    lv_obj_set_style_text_color(
        screensaver_mcp_list_title_label_, lv_color_white(), 0);
    lv_obj_set_style_text_align(
        screensaver_mcp_list_title_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(screensaver_mcp_list_title_label_, LV_LABEL_LONG_CLIP);
    lv_label_set_text(screensaver_mcp_list_title_label_, "自定义 MCP");
    lv_obj_align(
        screensaver_mcp_list_title_label_, LV_ALIGN_TOP_MID, 0,
        kCustomMcpListTitleTopOffset);

    screensaver_mcp_list_item_viewport_ = lv_obj_create(screensaver_mcp_list_viewport_);
    lv_obj_set_size(
        screensaver_mcp_list_item_viewport_,
        kCustomMcpListItemViewportWidth,
        kCustomMcpListItemViewportHeight);
    lv_obj_set_style_bg_opa(screensaver_mcp_list_item_viewport_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(
        screensaver_mcp_list_item_viewport_, kCustomMcpListViewportRadius, 0);
    lv_obj_set_style_border_width(screensaver_mcp_list_item_viewport_, 0, 0);
    lv_obj_set_style_pad_all(screensaver_mcp_list_item_viewport_, 0, 0);
    lv_obj_set_scrollbar_mode(
        screensaver_mcp_list_item_viewport_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(
        screensaver_mcp_list_item_viewport_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(
        screensaver_mcp_list_item_viewport_, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_align(
        screensaver_mcp_list_item_viewport_, LV_ALIGN_CENTER, 0,
        kCustomMcpListItemViewportOffsetY);

    screensaver_mcp_list_label_ = lv_label_create(screensaver_mcp_list_item_viewport_);
    lv_obj_set_width(screensaver_mcp_list_label_, kCustomMcpListTextWidth);
    lv_obj_set_height(screensaver_mcp_list_label_, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(screensaver_mcp_list_label_, text_font, 0);
    lv_obj_set_style_text_color(
        screensaver_mcp_list_label_, lv_color_hex(0xE8ECEF), 0);
    lv_obj_set_style_text_align(
        screensaver_mcp_list_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(
        screensaver_mcp_list_label_, kCustomMcpListLineSpacing, 0);
    lv_label_set_long_mode(screensaver_mcp_list_label_, LV_LABEL_LONG_WRAP);
    lv_label_set_text(screensaver_mcp_list_label_, "暂无自定义 MCP");
    lv_obj_center(screensaver_mcp_list_label_);

    screensaver_mcp_list_switch_timer_ = lv_timer_create(
        CustomMcpListSwitchTimerCallback,
        kCustomMcpListSwitchPeriodMs,
        this);
    lv_timer_pause(screensaver_mcp_list_switch_timer_);
    lv_obj_add_flag(screensaver_mcp_list_viewport_, LV_OBJ_FLAG_HIDDEN);

    /* 当前天气位置独占一行并位于天气三列上方，固定宽度保证文本始终以圆屏中心对齐。 */
    screensaver_weather_location_label_ = lv_label_create(screensaver_container_);
    lv_obj_set_width(screensaver_weather_location_label_, kScreensaverLocationWidth);
    lv_obj_set_style_text_font(screensaver_weather_location_label_, screensaver_weather_font, 0);
    lv_obj_set_style_text_color(screensaver_weather_location_label_,
                                lv_color_hex(kScreensaverSecondaryTextColor), 0);
    lv_obj_set_style_text_opa(screensaver_weather_location_label_, LV_OPA_80, 0);
    lv_obj_set_style_text_align(screensaver_weather_location_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(screensaver_weather_location_label_, LV_LABEL_LONG_CLIP);
    lv_label_set_text(screensaver_weather_location_label_, "天气位置待同步");
    lv_obj_align(screensaver_weather_location_label_, LV_ALIGN_CENTER, 0, -126);

    /*
     * 天气信息占据顶部同一行，并按照当前温度、天气描述、最低/最高温三列排列。
     * 三列放在自动宽度的 Flex 容器中，保持 12px 可见间距并作为一个整体水平居中。
     */
    screensaver_weather_group_ = lv_obj_create(screensaver_container_);
    lv_obj_set_size(screensaver_weather_group_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(screensaver_weather_group_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(screensaver_weather_group_, 0, 0);
    lv_obj_set_style_pad_all(screensaver_weather_group_, 0, 0);
    lv_obj_set_flex_flow(screensaver_weather_group_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(screensaver_weather_group_, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(screensaver_weather_group_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(screensaver_weather_group_, LV_ALIGN_CENTER, 0, -96);

    screensaver_weather_temperature_label_ = lv_label_create(screensaver_weather_group_);
    lv_obj_set_size(screensaver_weather_temperature_label_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(screensaver_weather_temperature_label_, screensaver_weather_font, 0);
    lv_obj_set_style_text_color(screensaver_weather_temperature_label_, lv_color_white(), 0);
    lv_obj_set_style_text_align(screensaver_weather_temperature_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(screensaver_weather_temperature_label_, "--℃");

    screensaver_weather_description_label_ = lv_label_create(screensaver_weather_group_);
    lv_obj_set_size(screensaver_weather_description_label_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(screensaver_weather_description_label_, screensaver_weather_font, 0);
    lv_obj_set_style_text_color(screensaver_weather_description_label_,
                                lv_color_hex(kScreensaverSecondaryTextColor), 0);
    lv_obj_set_style_text_align(screensaver_weather_description_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(screensaver_weather_description_label_, "待同步");

    screensaver_weather_range_label_ = lv_label_create(screensaver_weather_group_);
    lv_obj_set_size(screensaver_weather_range_label_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(screensaver_weather_range_label_, screensaver_weather_font, 0);
    lv_obj_set_style_text_color(screensaver_weather_range_label_, lv_color_white(), 0);
    lv_obj_set_style_text_align(screensaver_weather_range_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(screensaver_weather_range_label_, "--/--");

    /*
     * 日期三列使用一个自动宽度的横向组，最终可见间距为 16px，并由整个组统一居中。
     * 标签采用自动内容宽度和裁切模式，确保农历、公历、星期始终保持在同一行。
     */
    screensaver_date_group_ = lv_obj_create(screensaver_container_);
    lv_obj_set_size(screensaver_date_group_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(screensaver_date_group_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(screensaver_date_group_, 0, 0);
    lv_obj_set_style_pad_all(screensaver_date_group_, 0, 0);
    lv_obj_set_flex_flow(screensaver_date_group_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(screensaver_date_group_, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(screensaver_date_group_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(screensaver_date_group_, LV_ALIGN_CENTER, 0, -60);

    screensaver_lunar_date_label_ = lv_label_create(screensaver_date_group_);
    lv_obj_set_size(screensaver_lunar_date_label_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(screensaver_lunar_date_label_, screensaver_weather_font, 0);
    lv_obj_set_style_text_color(screensaver_lunar_date_label_, lv_color_white(), 0);
    lv_obj_set_style_text_opa(screensaver_lunar_date_label_, LV_OPA_80, 0);
    lv_obj_set_style_text_align(screensaver_lunar_date_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(screensaver_lunar_date_label_, LV_LABEL_LONG_CLIP);
    lv_label_set_text(screensaver_lunar_date_label_, "农历待同步");

    screensaver_solar_date_label_ = lv_label_create(screensaver_date_group_);
    lv_obj_set_size(screensaver_solar_date_label_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(screensaver_solar_date_label_, screensaver_weather_font, 0);
    lv_obj_set_style_text_color(screensaver_solar_date_label_, lv_color_white(), 0);
    lv_obj_set_style_text_opa(screensaver_solar_date_label_, LV_OPA_80, 0);
    lv_obj_set_style_text_align(screensaver_solar_date_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(screensaver_solar_date_label_, LV_LABEL_LONG_CLIP);
    lv_label_set_text(screensaver_solar_date_label_, "-- --");

    screensaver_weekday_label_ = lv_label_create(screensaver_date_group_);
    lv_obj_set_size(screensaver_weekday_label_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(screensaver_weekday_label_, screensaver_weather_font, 0);
    lv_obj_set_style_text_color(screensaver_weekday_label_, lv_color_white(), 0);
    lv_obj_set_style_text_opa(screensaver_weekday_label_, LV_OPA_80, 0);
    lv_obj_set_style_text_align(screensaver_weekday_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(screensaver_weekday_label_, LV_LABEL_LONG_CLIP);
    lv_label_set_text(screensaver_weekday_label_, "星期--");

    /*
     * 时分和秒放入同一个自动宽度的横向容器。容器自身负责整体居中，并在两个标签之间
     * 保留约 10px 的最终可见间距。整个容器统一放大，避免分别缩放和定位产生偏移。
     */
    screensaver_time_group_ = lv_obj_create(screensaver_container_);
    lv_obj_set_size(screensaver_time_group_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(screensaver_time_group_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(screensaver_time_group_, 0, 0);
    lv_obj_set_style_pad_all(screensaver_time_group_, 0, 0);
    lv_obj_set_style_pad_column(screensaver_time_group_, kScreensaverTimeColumnGap, 0);
    lv_obj_set_flex_flow(screensaver_time_group_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(screensaver_time_group_, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(screensaver_time_group_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(screensaver_time_group_, LV_ALIGN_CENTER, 0, 4);

    screensaver_time_label_ = lv_label_create(screensaver_time_group_);
    lv_obj_set_size(screensaver_time_label_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(screensaver_time_label_, &font_puhui_time_64, 0);
    lv_obj_set_style_text_color(screensaver_time_label_, lv_color_white(), 0);
    lv_obj_set_style_text_align(screensaver_time_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(screensaver_time_label_, "--:--");

    screensaver_seconds_label_ = lv_label_create(screensaver_time_group_);
    lv_obj_set_size(screensaver_seconds_label_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(screensaver_seconds_label_, &font_puhui_time_64, 0);
    lv_obj_set_style_text_color(screensaver_seconds_label_, lv_color_hex(kScreensaverAccentColor), 0);
    lv_obj_set_style_text_align(screensaver_seconds_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(screensaver_seconds_label_, "--");

    /*
     * 时间下方使用固定三行高度的透明根视口。根视口内部再建立三条由宽到窄的水平裁切带，
     * 用分段圆弦逼近圆屏下半部分的左右弧形，避免备忘录覆盖外圈刻度。每条裁切带持有一份
     * 相同文本，三个标签同步纵向移动后，视觉上仍是一段连续滚动的完整备忘录。
     */
    screensaver_memo_viewport_ = lv_obj_create(screensaver_container_);
    lv_obj_set_size(screensaver_memo_viewport_, kScreensaverMemoWidth,
                    kScreensaverMemoViewportHeight);
    lv_obj_set_style_bg_opa(screensaver_memo_viewport_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(screensaver_memo_viewport_, 0, 0);
    lv_obj_set_style_pad_all(screensaver_memo_viewport_, 0, 0);
    lv_obj_set_scrollbar_mode(screensaver_memo_viewport_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(screensaver_memo_viewport_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(screensaver_memo_viewport_, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_align(screensaver_memo_viewport_, LV_ALIGN_CENTER, 0,
                 kScreensaverBottomSectionOffsetY);

    for (int row = 0; row < kScreensaverMemoVisibleLines; ++row) {
        lv_obj_t* row_viewport = lv_obj_create(screensaver_memo_viewport_);
        screensaver_memo_row_viewports_[row] = row_viewport;
        lv_obj_set_size(row_viewport, kScreensaverMemoRowWidths[row],
                        kScreensaverMemoRowHeight);
        lv_obj_set_style_bg_opa(row_viewport, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row_viewport, 0, 0);
        lv_obj_set_style_pad_all(row_viewport, 0, 0);
        lv_obj_set_scrollbar_mode(row_viewport, LV_SCROLLBAR_MODE_OFF);
        lv_obj_remove_flag(row_viewport, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(row_viewport, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
        lv_obj_align(row_viewport, LV_ALIGN_TOP_MID, 0,
                     row * kScreensaverMemoRowHeight);

        lv_obj_t* memo_label = lv_label_create(row_viewport);
        screensaver_memo_labels_[row] = memo_label;
        lv_obj_set_width(memo_label, kScreensaverMemoWidth);
        lv_obj_set_style_text_font(memo_label, text_font, 0);
        lv_obj_set_style_text_color(memo_label, lv_color_hex(0xDDE1E4), 0);
        lv_obj_set_style_text_align(memo_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_line_space(memo_label, kScreensaverMemoLineSpacing, 0);
        lv_label_set_long_mode(memo_label, LV_LABEL_LONG_WRAP);
        lv_label_set_text(memo_label, "暂无待办");
        lv_obj_align(memo_label, LV_ALIGN_TOP_MID, 0,
                     -row * kScreensaverMemoRowHeight);

        lv_obj_t* date_label = lv_label_create(row_viewport);
        screensaver_memo_date_labels_[row] = date_label;
        lv_obj_set_width(date_label, kScreensaverMemoWidth);
        lv_obj_set_height(date_label, LV_SIZE_CONTENT);
        lv_obj_set_style_text_font(date_label, text_font, 0);
        lv_obj_set_style_text_color(date_label, lv_color_hex(0xDDE1E4), 0);
        lv_obj_set_style_text_align(date_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(date_label, LV_LABEL_LONG_CLIP);
        lv_label_set_text(date_label, "");
        lv_obj_align(date_label, LV_ALIGN_TOP_MID, 0,
                     -row * kScreensaverMemoRowHeight);
    }
    screensaver_memo_timer_ = lv_timer_create(
        ScreensaverMemoTimerCallback, 8000, this);
    lv_timer_pause(screensaver_memo_timer_);

    /*
     * Wi-Fi 图标放在已经隐藏主刻度的 12 点位置。网络连接、弱信号和断网状态继续使用
     * Board 返回的不同 Font Awesome 图标表达，透明背景保持金属外圈连续可见。
     */
    screensaver_network_label_ = lv_label_create(screensaver_container_);
    lv_obj_set_size(screensaver_network_label_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(screensaver_network_label_, icon_font, 0);
    lv_obj_set_style_text_color(screensaver_network_label_, lv_color_hex(0xDDE1E4), 0);
    lv_obj_set_style_text_align(screensaver_network_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_opa(screensaver_network_label_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(screensaver_network_label_, 0, 0);
    lv_label_set_text(screensaver_network_label_, FONT_AWESOME_WIFI_SLASH);
    lv_obj_align(screensaver_network_label_, LV_ALIGN_TOP_MID, 0,
                 kScreensaverDialStatusInset);

    /*
     * 电量图标位于已经隐藏主刻度的 6 点位置，使用空、四分之一、半格、四分之三、满格和
     * 充电闪电图标直接表达当前电池状态，不再占用时间下方的备忘录空间。
     */
    screensaver_battery_label_ = lv_label_create(screensaver_container_);
    lv_obj_set_size(screensaver_battery_label_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(screensaver_battery_label_, icon_font, 0);
    lv_obj_set_style_text_color(screensaver_battery_label_, lv_color_hex(0xDDE1E4), 0);
    lv_obj_set_style_text_align(screensaver_battery_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_opa(screensaver_battery_label_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(screensaver_battery_label_, 0, 0);
    lv_label_set_text(screensaver_battery_label_, FONT_AWESOME_BATTERY_EMPTY);
    lv_obj_align(screensaver_battery_label_, LV_ALIGN_BOTTOM_MID, 0,
                 -kScreensaverDialStatusInset);

    /* 使用当前主题字体，并把不同源字号换算为各信息层级对应的表盘视觉字号。 */
    ApplyScreensaverTextFont(text_font);
    ApplyCustomMcpListFont(text_font);
    ApplyScreensaverLocationFont(text_font);
    ApplyScreensaverWeatherFont(text_font);
    ApplyScreensaverDateFont(text_font);
    ApplyScreensaverStatusIconFont(icon_font);
    UpdateScreensaverMemo();

    lv_obj_add_flag(screensaver_container_, LV_OBJ_FLAG_HIDDEN);
}

/**
 * @brief 刷新屏保表盘中的时间、农历、公历、星期、当前秒刻度、网络和电量。
 * @details 系统时间尚未由网络校准时显示占位文本。农历由本地年份表换算，天气和待办内容
 *          由 BackendService 独立更新。本方法仅在标签内容变化时重绘，降低每秒刷新负担。
 */
void LcdDisplay::UpdateScreensaverContent() {
    if (!screensaver_active_ || screensaver_container_ == nullptr
        || custom_mcp_list_active_.load()) {
        return;
    }

    char time_text[8] = "--:--";
    char seconds_text[4] = "--";
    char lunar_date_text[32] = "农历--";
    /*
     * tm_mon 和 tm_mday 的类型都是 int。虽然正常值分别只有 1-12 和 1-31，
     * 编译器进行格式截断检查时仍会按照两个完整 int 的最坏长度计算，因此预留
     * 足以容纳两个 32 位十进制整数、分隔符和字符串结束符的缓冲区。
     */
    char solar_date_text[32] = "-- --";
    char weekday_text[16] = "星期--";
    int current_second = 0;

    const time_t now = time(nullptr);
    struct tm local_time = {};
    if (localtime_r(&now, &local_time) != nullptr && local_time.tm_year >= 2025 - 1900) {
        static const char* weekdays[] = {
            "星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"
        };
        snprintf(time_text, sizeof(time_text), "%02d:%02d", local_time.tm_hour, local_time.tm_min);
        snprintf(seconds_text, sizeof(seconds_text), "%02d", local_time.tm_sec);
        snprintf(solar_date_text, sizeof(solar_date_text), "%d-%d",
                 local_time.tm_mon + 1, local_time.tm_mday);
        snprintf(weekday_text, sizeof(weekday_text), "%s", weekdays[local_time.tm_wday]);
        FormatLunarDate(local_time.tm_year + 1900,
                        static_cast<unsigned>(local_time.tm_mon + 1),
                        static_cast<unsigned>(local_time.tm_mday),
                        lunar_date_text, sizeof(lunar_date_text));
        current_second = local_time.tm_sec;
    }

    SetLabelTextIfChanged(screensaver_time_label_, time_text);
    SetLabelTextIfChanged(screensaver_seconds_label_, seconds_text);
    const int date_key = local_time.tm_year >= 2025 - 1900
        ? (local_time.tm_year + 1900) * 10000
            + (local_time.tm_mon + 1) * 100
            + local_time.tm_mday
        : 0;
    if (date_key != screensaver_last_date_key_) {
        SetLabelTextIfChanged(screensaver_lunar_date_label_, lunar_date_text);
        SetLabelTextIfChanged(screensaver_solar_date_label_, solar_date_text);
        SetLabelTextIfChanged(screensaver_weekday_label_, weekday_text);
        screensaver_last_date_key_ = date_key;
    }

    if (screensaver_second_marker_ != nullptr) {
        const auto& marker_layout = GetSecondMarkerLayouts()[current_second];
        const bool marker_visible =
            local_time.tm_year >= 2025 - 1900 && marker_layout.visible;
        const int rendered_second = local_time.tm_year >= 2025 - 1900
            ? current_second : -1;
        if (rendered_second != screensaver_last_rendered_second_) {
            if (!marker_visible) {
                lv_obj_add_flag(screensaver_second_marker_, LV_OBJ_FLAG_HIDDEN);
            } else {
                const int marker_style = marker_layout.major ? 1 : 0;
                if (screensaver_second_marker_style_ != marker_style) {
                    lv_obj_set_size(
                        screensaver_second_marker_, marker_layout.width, marker_layout.height);
                    lv_obj_set_style_transform_pivot_x(
                        screensaver_second_marker_, marker_layout.width / 2, 0);
                    lv_obj_set_style_transform_pivot_y(
                        screensaver_second_marker_, marker_layout.height / 2, 0);
                    screensaver_second_marker_style_ = marker_style;
                }
                lv_obj_set_style_transform_rotation(
                    screensaver_second_marker_, marker_layout.rotation, 0);
                lv_obj_set_pos(
                    screensaver_second_marker_, marker_layout.x, marker_layout.y);
                lv_obj_remove_flag(screensaver_second_marker_, LV_OBJ_FLAG_HIDDEN);
            }
            screensaver_last_rendered_second_ = rendered_second;
        }
    }

    int battery_level = 0;
    bool charging = false;
    bool discharging = false;
    const char* battery_icon = FONT_AWESOME_BATTERY_EMPTY;
    const bool battery_available =
        Board::GetInstance().GetBatteryLevel(battery_level, charging, discharging);
    if (battery_available) {
        (void)discharging;
        if (charging) {
            battery_icon = FONT_AWESOME_BATTERY_BOLT;
        } else if (battery_level >= 80) {
            battery_icon = FONT_AWESOME_BATTERY_FULL;
        } else if (battery_level >= 60) {
            battery_icon = FONT_AWESOME_BATTERY_THREE_QUARTERS;
        } else if (battery_level >= 40) {
            battery_icon = FONT_AWESOME_BATTERY_HALF;
        } else if (battery_level >= 20) {
            battery_icon = FONT_AWESOME_BATTERY_QUARTER;
        }
    }
    SetLabelTextIfChanged(screensaver_battery_label_, battery_icon);
    if (screensaver_battery_label_ != nullptr) {
        const bool battery_needs_attention =
            battery_available && (charging || battery_level < 20);
        if (!screensaver_battery_attention_valid_
            || screensaver_battery_attention_ != battery_needs_attention) {
            lv_obj_set_style_text_color(
                screensaver_battery_label_,
                lv_color_hex(battery_needs_attention ? kScreensaverAccentColor : 0xDDE1E4), 0);
            screensaver_battery_attention_ = battery_needs_attention;
            screensaver_battery_attention_valid_ = true;
        }
    }

    /*
     * Board 返回的网络图标与普通页面状态栏使用相同来源，已经综合当前连接类型、
     * 连接状态和信号强度。返回空指针时使用断网图标作为稳定回退。
     */
    const char* network_icon = Board::GetInstance().GetNetworkStateIcon();
    const char* visible_network_icon =
        network_icon != nullptr ? network_icon : FONT_AWESOME_WIFI_SLASH;
    SetLabelTextIfChanged(screensaver_network_label_, visible_network_icon);
    if (screensaver_network_label_ != nullptr) {
        const bool network_disconnected =
            strcmp(visible_network_icon, FONT_AWESOME_WIFI_SLASH) == 0;
        if (!screensaver_network_disconnected_valid_
            || screensaver_network_disconnected_ != network_disconnected) {
            lv_obj_set_style_text_color(
                screensaver_network_label_,
                lv_color_hex(network_disconnected ? kScreensaverAccentColor : 0xDDE1E4), 0);
            screensaver_network_disconnected_ = network_disconnected;
            screensaver_network_disconnected_valid_ = true;
        }
    }
}

/**
 * @brief 刷新普通状态栏以及当前可见的屏保表盘。
 * @param update_all true 强制刷新网络等全部状态；false 使用普通周期策略。
 */
void LcdDisplay::UpdateStatusBar(bool update_all) {
    LvglDisplay::UpdateStatusBar(update_all);

    DisplayLockGuard lock(this);
    UpdateScreensaverContent();
}

/**
 * @brief 显示或隐藏金属黑表盘屏保。
 * @param enabled true 显示表盘；false 恢复进入屏保前的正常界面。
 * @details 进入屏保时停止 GIF 和字幕滚动以降低后台刷新；退出时恢复仍然有效的表情动画和字幕滚动。
 */
void LcdDisplay::SetScreensaverMode(bool enabled) {
    DisplayLockGuard lock(this);
    if (screensaver_container_ == nullptr || screensaver_active_ == enabled) {
        return;
    }

    screensaver_active_ = enabled;
    if (enabled) {
        if (conversation_face_timer_ != nullptr) {
            lv_timer_pause(conversation_face_timer_);
        }
        screensaver_last_rendered_second_ = -2;
        SetStandardScreensaverContentVisible(true);
        if (gif_controller_) {
            gif_controller_->Stop();
        }
        if (chat_message_label_ != nullptr) {
            lv_anim_delete(chat_message_label_, nullptr);
            lv_obj_set_style_translate_y(chat_message_label_, 0, 0);
        }
        UpdateScreensaverContent();
        /*
         * 结构化备忘录在数据写入时已经完成文本和三行布局，屏保隐藏不会销毁这些状态。
         * 只有没有日期行的兼容长文本需要在动画被停止后恢复滚动布局。
         */
        if (screensaver_memo_labels_[0] != nullptr) {
            const char* memo_text = lv_label_get_text(screensaver_memo_labels_[0]);
            if (memo_text != nullptr && std::strchr(memo_text, '\n') == nullptr) {
                UpdateScreensaverMemoScroll();
            }
        }
        if (screensaver_memo_timer_ != nullptr && screensaver_memos_.size() > 1) {
            lv_timer_resume(screensaver_memo_timer_);
            lv_timer_reset(screensaver_memo_timer_);
        }
        lv_obj_remove_flag(screensaver_container_, LV_OBJ_FLAG_HIDDEN);
        if (binding_active_ && binding_container_ != nullptr) {
            lv_obj_move_foreground(binding_container_);
        }
    } else {
        custom_mcp_list_active_.store(false);
        SetStandardScreensaverContentVisible(true);
        if (screensaver_mcp_list_switch_timer_ != nullptr) {
            lv_timer_pause(screensaver_mcp_list_switch_timer_);
        }
        if (screensaver_memo_timer_ != nullptr) {
            lv_timer_pause(screensaver_memo_timer_);
        }
        lv_anim_delete(this, ScreensaverMemoScrollAnimationCallback);
        for (lv_obj_t* memo_label : screensaver_memo_labels_) {
            if (memo_label != nullptr) {
                lv_obj_set_style_translate_y(memo_label, 0, 0);
            }
        }
        lv_obj_add_flag(screensaver_container_, LV_OBJ_FLAG_HIDDEN);
        if (conversation_face_timer_ != nullptr) {
            lv_timer_resume(conversation_face_timer_);
            lv_timer_reset(conversation_face_timer_);
        }
        UpdateConversationFaceFrame();
        if (gif_controller_) {
            gif_controller_->Start();
        }
        UpdateSubtitleScroll();
    }
}

/**
 * @brief 更新表盘天气位置、当前温度、天气描述和最低/最高温三列。
 * @param location 当前天气位置名称，例如“上海市松江区”。
 * @param temperature 当前摄氏温度。
 * @param weather 中文天气描述。
 * @param low_temperature 当天最低摄氏温度。
 * @param high_temperature 当天最高摄氏温度。
 * @details 方法内部获取 LVGL 锁并复制所有文本，调用结束后不再引用传入字符串。
 */
void LcdDisplay::SetScreensaverWeather(
    const std::string& location,
    int temperature,
    const std::string& weather,
    int low_temperature,
    int high_temperature) {
    char temperature_text[32];
    char range_text[64];
    snprintf(temperature_text, sizeof(temperature_text), "%d℃", temperature);
    snprintf(range_text, sizeof(range_text), "%d/%d", low_temperature, high_temperature);
    SetScreensaverWeatherText(location, temperature_text, weather, range_text);
}

/**
 * @brief 使用已经格式化的文本更新表盘天气位置和三列内容。
 * @param location 天气位置或当前天气服务状态说明。
 * @param temperature 当前温度文本，例如“21℃”或“--℃”。
 * @param weather 中文天气描述或占位文本。
 * @param temperature_range 最低和最高温度文本，例如“18/26”或“--/--”。
 * @details 方法内部获取 LVGL 锁并复制全部文本，适用于后端尚未接入、请求加载中或
 *          返回值无效等无法构造整数温度的界面状态。
 */
void LcdDisplay::SetScreensaverWeatherText(
    const std::string& location,
    const std::string& temperature,
    const std::string& weather,
    const std::string& temperature_range) {
    DisplayLockGuard lock(this);
    SetLabelTextIfChanged(screensaver_weather_location_label_, location.c_str());
    SetLabelTextIfChanged(screensaver_weather_temperature_label_, temperature.c_str());
    SetLabelTextIfChanged(screensaver_weather_description_label_, weather.c_str());
    SetLabelTextIfChanged(screensaver_weather_range_label_, temperature_range.c_str());
}

/**
 * @brief 替换屏保备忘录数组并立即从第一条重新展示。
 * @param memos 后端按提醒时间排序的备忘录正文，最多采用前 5 条。
 */
void LcdDisplay::SetScreensaverMemos(const std::vector<std::string>& memos) {
    DisplayLockGuard lock(this);
    const size_t count = std::min<size_t>(memos.size(), 5);
    if (screensaver_memos_.size() == count
        && std::equal(screensaver_memos_.begin(), screensaver_memos_.end(), memos.begin())) {
        return;
    }
    screensaver_memos_.assign(memos.begin(), memos.begin() + count);
    screensaver_memo_index_ = 0;
    UpdateScreensaverMemo();
    if (screensaver_memo_timer_ != nullptr) {
        if (screensaver_active_ && screensaver_memos_.size() > 1) {
            lv_timer_resume(screensaver_memo_timer_);
            lv_timer_reset(screensaver_memo_timer_);
        } else {
            lv_timer_pause(screensaver_memo_timer_);
        }
    }
}

/**
 * @brief 刷新当前备忘录文本并重新计算三行视口布局。
 * @details 结构化屏保文本使用“时间换行正文”格式，正文最多显示两行，日期时间显示在正文
 *          下方并以第三行作为独立显示行。没有时间首行的加载状态和兼容文本继续使用原有高度判断。
 */
void LcdDisplay::UpdateScreensaverMemo() {
    if (screensaver_memo_labels_[0] == nullptr) {
        return;
    }
    const bool showing_empty_state = screensaver_memos_.empty();
    if (showing_empty_state && screensaver_weather_font_ != nullptr
        && screensaver_weather_font_->font() != nullptr) {
        const lv_font_t* heavy_font = screensaver_weather_font_->font();
        for (lv_obj_t* memo_label : screensaver_memo_labels_) {
            if (memo_label == nullptr) {
                continue;
            }
            lv_obj_set_style_text_font(memo_label, heavy_font, 0);
            lv_obj_set_style_transform_scale(memo_label, 256, 0);
            lv_obj_set_width(memo_label, kScreensaverMemoWidth);
            lv_obj_set_style_text_line_space(memo_label, kScreensaverMemoLineSpacing, 0);
        }
    } else if (!showing_empty_state && current_theme_ != nullptr) {
        auto* theme = static_cast<LvglTheme*>(current_theme_);
        if (theme->text_font() != nullptr && theme->text_font()->font() != nullptr) {
            ApplyScreensaverTextFont(theme->text_font()->font());
        }
    }
    const char* visible_text = "暂无待办";
    if (screensaver_memos_.empty()) {
        screensaver_memo_index_ = 0;
    } else {
        if (screensaver_memo_index_ >= screensaver_memos_.size()) {
            screensaver_memo_index_ = 0;
        }
        visible_text = screensaver_memos_[screensaver_memo_index_].c_str();
    }

    const char* line_break = std::strchr(visible_text, '\n');
    const bool has_reminder_line = line_break != nullptr
        && visible_text[0] == '['
        && line_break > visible_text + 1
        && *(line_break - 1) == ']';
    const char* content_text = visible_text;
    if (has_reminder_line) {
        content_text = line_break + 1;
        screensaver_memo_has_reminder_line_ = true;
    } else {
        screensaver_memo_has_reminder_line_ = false;
    }

    for (lv_obj_t* memo_label : screensaver_memo_labels_) {
        if (memo_label != nullptr) {
            lv_label_set_long_mode(memo_label, LV_LABEL_LONG_WRAP);
            lv_label_set_text(memo_label, content_text);
        }
    }
    const std::string date_text = !has_reminder_line
        ? std::string()
        : std::string(visible_text, static_cast<size_t>(line_break - visible_text));
    for (lv_obj_t* date_label : screensaver_memo_date_labels_) {
        if (date_label != nullptr) {
            lv_label_set_text(date_label, date_text.c_str());
        }
    }
    UpdateScreensaverMemoScroll();
}

/**
 * @brief 根据当前备忘录文本类型配置固定三行或兼容滚动布局。
 * @details 包含显式换行的结构化备忘录正文最多占用两行，日期时间紧接正文显示在下一行；
 *          内容不足两行时，正文和日期整体垂直居中。没有显式时间行的状态文本仍按实际高度居中，
 *          兼容的超长文本继续使用原有纵向滚动逻辑。
 */
void LcdDisplay::UpdateScreensaverMemoScroll() {
    lv_obj_t* measurement_label = screensaver_memo_labels_[0];
    if (screensaver_memo_viewport_ == nullptr || measurement_label == nullptr) {
        return;
    }

    lv_anim_delete(this, ScreensaverMemoScrollAnimationCallback);
    for (lv_obj_t* memo_label : screensaver_memo_labels_) {
        if (memo_label == nullptr) {
            continue;
        }
        lv_obj_set_style_translate_y(memo_label, 0, 0);
        lv_label_set_long_mode(memo_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_height(memo_label, LV_SIZE_CONTENT);
        lv_obj_update_layout(memo_label);
    }

    const int text_scale =
        lv_obj_get_style_transform_scale_y_safe(measurement_label, LV_PART_MAIN);
    const int logical_content_height = lv_obj_get_height(measurement_label);
    const int visible_content_height =
        (logical_content_height * text_scale + 255) / 256;
    const bool has_reminder_line = screensaver_memo_has_reminder_line_;
    if (has_reminder_line) {
        const bool content_fits =
            visible_content_height <= kScreensaverMemoRowHeight * 2;
        const int content_rows = content_fits
            ? std::max(1, std::min(2,
                (visible_content_height + kScreensaverMemoRowHeight - 1)
                    / kScreensaverMemoRowHeight))
            : 2;
        const int content_block_height =
            (content_rows + 1) * kScreensaverMemoRowHeight;
        const int top_offset =
            std::max(0, (kScreensaverMemoViewportHeight - content_block_height) / 2);
        const int logical_viewport_height =
            (kScreensaverMemoRowHeight * 2 * 256 + text_scale - 1) / text_scale;
        for (int row = 0; row < kScreensaverMemoVisibleLines; ++row) {
            lv_obj_t* memo_label = screensaver_memo_labels_[row];
            if (memo_label == nullptr) {
                continue;
            }
            lv_label_set_long_mode(
                memo_label,
                content_fits ? LV_LABEL_LONG_WRAP : LV_LABEL_LONG_DOT);
            lv_obj_set_height(
                memo_label,
                content_fits ? logical_content_height : logical_viewport_height);
            lv_obj_align(memo_label, LV_ALIGN_TOP_MID, 0,
                         top_offset - row * kScreensaverMemoRowHeight);
            lv_obj_update_layout(memo_label);
            lv_obj_t* date_label = screensaver_memo_date_labels_[row];
            if (date_label != nullptr) {
                lv_obj_align(date_label, LV_ALIGN_TOP_MID, 0,
                             top_offset + content_rows * kScreensaverMemoRowHeight
                                 - row * kScreensaverMemoRowHeight);
                lv_obj_update_layout(date_label);
            }
        }
        if (screensaver_memo_timer_ != nullptr) {
            lv_timer_set_period(screensaver_memo_timer_, 8000);
        }
        return;
    }

    if (visible_content_height <= kScreensaverMemoViewportHeight) {
        const int top_offset =
            (kScreensaverMemoViewportHeight - visible_content_height) / 2;
        for (int row = 0; row < kScreensaverMemoVisibleLines; ++row) {
            if (screensaver_memo_labels_[row] != nullptr) {
                lv_obj_align(screensaver_memo_labels_[row], LV_ALIGN_TOP_MID, 0,
                             top_offset - row * kScreensaverMemoRowHeight);
            }
        }
        if (screensaver_memo_timer_ != nullptr) {
            lv_timer_set_period(screensaver_memo_timer_, 8000);
        }
        return;
    }

    for (int row = 0; row < kScreensaverMemoVisibleLines; ++row) {
        if (screensaver_memo_labels_[row] != nullptr) {
            lv_obj_align(screensaver_memo_labels_[row], LV_ALIGN_TOP_MID, 0,
                         -row * kScreensaverMemoRowHeight);
        }
    }
    const int scroll_distance = visible_content_height - kScreensaverMemoViewportHeight;
    const int calculated_duration =
        scroll_distance * 1000 / kScreensaverMemoScrollPixelsPerSecond;
    const int scroll_duration = std::min(30000, std::max(1500, calculated_duration));
    const uint32_t display_period = static_cast<uint32_t>(
        kScreensaverMemoScrollStartDelayMs + scroll_duration + kScreensaverMemoScrollEndDelayMs);
    if (screensaver_memo_timer_ != nullptr) {
        lv_timer_set_period(screensaver_memo_timer_, display_period);
    }

    if (!screensaver_active_) {
        return;
    }

    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, this);
    lv_anim_set_exec_cb(&animation, ScreensaverMemoScrollAnimationCallback);
    lv_anim_set_values(&animation, 0, -scroll_distance);
    lv_anim_set_duration(&animation, scroll_duration);
    lv_anim_set_delay(&animation, kScreensaverMemoScrollStartDelayMs);
    lv_anim_set_repeat_delay(&animation, kScreensaverMemoScrollEndDelayMs);
    lv_anim_set_repeat_count(
        &animation,
        screensaver_memos_.size() > 1 ? 0 : LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&animation, lv_anim_path_linear);
    lv_anim_start(&animation);
}

/**
 * @brief 在同一动画帧内同步移动三条裁切带中的备忘录镜像标签。
 * @param target 指向当前 LcdDisplay 实例。
 * @param value 相对各标签初始位置的 Y 轴物理像素位移。
 * @details 单个 LVGL 动画只计算一次时间和像素位置，本方法再把结果写入三个标签。这样
 *          三条裁切带交界处看到的是同一份文本的同一条扫描行，滚动连续性与单标签聊天
 *          字幕一致，同时仍保留圆屏需要的分段弧形裁切。
 */
void LcdDisplay::ScreensaverMemoScrollAnimationCallback(void* target, int32_t value) {
    auto* display = static_cast<LcdDisplay*>(target);
    if (display == nullptr) {
        return;
    }

    for (lv_obj_t* memo_label : display->screensaver_memo_labels_) {
        if (memo_label != nullptr) {
            lv_obj_set_style_translate_y(memo_label, value, 0);
        }
    }
}

/**
 * @brief 在 LVGL 定时器上下文中切换到下一条备忘录。
 * @param timer user_data 必须是仍然有效的 LcdDisplay 指针。
 */
void LcdDisplay::ScreensaverMemoTimerCallback(lv_timer_t* timer) {
    auto* display = static_cast<LcdDisplay*>(lv_timer_get_user_data(timer));
    if (display == nullptr || !display->screensaver_active_
        || display->screensaver_memos_.empty()) {
        return;
    }
    display->screensaver_memo_index_ =
        (display->screensaver_memo_index_ + 1) % display->screensaver_memos_.size();
    display->UpdateScreensaverMemo();
    lv_timer_reset(timer);
}
