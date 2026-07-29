/**
 * @file lcd_mcp_view.cc
 * @brief 屏保内自定义 MCP 工具列表页面实现。
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
 * @brief 将主题字体应用到自定义 MCP 固定标题和单项内容标签。
 * @param font 当前主题的完整中文字库。
 * @details 标题保持 30px，单项内容保持 25px；两者都按最终物理宽度反算逻辑宽度。
 */
void LcdDisplay::ApplyCustomMcpListFont(const lv_font_t* font) {
    if (font == nullptr || font->line_height <= 0
        || screensaver_mcp_list_title_label_ == nullptr
        || screensaver_mcp_list_label_ == nullptr) {
        return;
    }

    const int title_scale =
        kCustomMcpListTitleTextPixelSize * 256 / font->line_height;
    const int title_logical_width =
        kCustomMcpListTitleWidth * 256 / title_scale;
    lv_obj_set_width(screensaver_mcp_list_title_label_, title_logical_width);
    lv_obj_set_style_text_font(screensaver_mcp_list_title_label_, font, 0);
    lv_obj_set_style_transform_scale(
        screensaver_mcp_list_title_label_, title_scale, 0);
    lv_obj_set_style_transform_pivot_x(
        screensaver_mcp_list_title_label_, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(
        screensaver_mcp_list_title_label_, 0, 0);

    const int text_scale =
        kCustomMcpListTextPixelSize * 256 / font->line_height;
    const int logical_width =
        kCustomMcpListTextWidth * 256 / text_scale;
    const int logical_line_spacing =
        (kCustomMcpListLineSpacing * 256 + text_scale - 1) / text_scale;
    lv_obj_set_width(screensaver_mcp_list_label_, logical_width);
    lv_obj_set_style_text_font(screensaver_mcp_list_label_, font, 0);
    lv_obj_set_style_text_line_space(
        screensaver_mcp_list_label_, logical_line_spacing, 0);
    lv_obj_set_style_transform_scale(screensaver_mcp_list_label_, text_scale, 0);
    lv_obj_set_style_transform_pivot_x(
        screensaver_mcp_list_label_, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(screensaver_mcp_list_label_, 0, 0);
}



/**
 * @brief 显示仅保留表盘外圈的自定义 MCP 清单页面。
 * @param title 固定显示在顶部的清单标题。
 * @param items 按 5 秒间隔轮播的 MCP 中文名称和工具代码数组。
 */
void LcdDisplay::ShowCustomMcpList(
    const std::string& title,
    const std::vector<std::string>& items) {
    DisplayLockGuard lock(this);
    if (screensaver_container_ == nullptr
        || screensaver_mcp_list_viewport_ == nullptr
        || screensaver_mcp_list_title_label_ == nullptr
        || screensaver_mcp_list_label_ == nullptr) {
        return;
    }

    custom_mcp_list_active_.store(true);
    screensaver_active_ = true;
    if (conversation_face_timer_ != nullptr) {
        lv_timer_pause(conversation_face_timer_);
    }
    if (gif_controller_) {
        gif_controller_->Stop();
    }
    if (chat_message_label_ != nullptr) {
        lv_anim_delete(chat_message_label_, nullptr);
        lv_obj_set_style_translate_y(chat_message_label_, 0, 0);
    }
    if (screensaver_memo_timer_ != nullptr) {
        lv_timer_pause(screensaver_memo_timer_);
    }
    lv_anim_delete(this, ScreensaverMemoScrollAnimationCallback);
    SetStandardScreensaverContentVisible(false);

    SetLabelTextIfChanged(
        screensaver_mcp_list_title_label_,
        title.empty() ? "自定义 MCP" : title.c_str());
    custom_mcp_list_items_ = items;
    if (custom_mcp_list_items_.empty()) {
        custom_mcp_list_items_.push_back("暂无自定义 MCP");
    }
    custom_mcp_list_index_ = 0;
    if (screensaver_second_marker_ != nullptr) {
        lv_obj_add_flag(screensaver_second_marker_, LV_OBJ_FLAG_HIDDEN);
    }
    UpdateCustomMcpListItem();
    if (screensaver_mcp_list_switch_timer_ != nullptr) {
        if (custom_mcp_list_items_.size() > 1) {
            lv_timer_resume(screensaver_mcp_list_switch_timer_);
            lv_timer_reset(screensaver_mcp_list_switch_timer_);
        } else {
            lv_timer_pause(screensaver_mcp_list_switch_timer_);
        }
    }
    lv_obj_remove_flag(screensaver_container_, LV_OBJ_FLAG_HIDDEN);
    if (binding_active_ && binding_container_ != nullptr) {
        lv_obj_move_foreground(binding_container_);
    }
    lv_obj_invalidate(screensaver_container_);
}

/**
 * @brief 退出自定义 MCP 清单页面。
 * @details 复用屏保退出路径，统一停止列表动画、恢复普通内容并重新启用对话界面动画。
 */
void LcdDisplay::HideCustomMcpList() {
    if (!custom_mcp_list_active_.load()) {
        return;
    }
    SetScreensaverMode(false);
}

/**
 * @brief 更新当前 MCP 单项并在内容视口中垂直居中。
 * @details 超出单项安全高度时使用省略号截断，不启动滚动，确保语音播放期间屏幕保持低负载。
 */
void LcdDisplay::UpdateCustomMcpListItem() {
    if (screensaver_mcp_list_label_ == nullptr
        || screensaver_mcp_list_item_viewport_ == nullptr
        || custom_mcp_list_items_.empty()) {
        return;
    }

    if (custom_mcp_list_index_ >= custom_mcp_list_items_.size()) {
        custom_mcp_list_index_ = 0;
    }
    lv_label_set_long_mode(screensaver_mcp_list_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(screensaver_mcp_list_label_, LV_SIZE_CONTENT);
    lv_label_set_text(
        screensaver_mcp_list_label_,
        custom_mcp_list_items_[custom_mcp_list_index_].c_str());
    lv_obj_update_layout(screensaver_mcp_list_label_);

    const int text_scale =
        lv_obj_get_style_transform_scale_y_safe(
            screensaver_mcp_list_label_, LV_PART_MAIN);
    const int logical_content_height = lv_obj_get_height(screensaver_mcp_list_label_);
    const int visible_content_height =
        (logical_content_height * text_scale + 255) / 256;
    int visible_height = visible_content_height;
    if (visible_content_height > kCustomMcpListItemSafeHeight) {
        const int logical_safe_height =
            (kCustomMcpListItemSafeHeight * 256 + text_scale - 1) / text_scale;
        lv_label_set_long_mode(screensaver_mcp_list_label_, LV_LABEL_LONG_DOT);
        lv_obj_set_height(screensaver_mcp_list_label_, logical_safe_height);
        visible_height = kCustomMcpListItemSafeHeight;
    }
    lv_obj_align(
        screensaver_mcp_list_label_,
        LV_ALIGN_TOP_MID,
        0,
        (kCustomMcpListItemViewportHeight - visible_height) / 2);
    lv_obj_invalidate(screensaver_mcp_list_item_viewport_);
}

/**
 * @brief 每 5 秒切换到下一项 MCP。
 * @param timer user_data 指向当前 LcdDisplay 实例。
 */
void LcdDisplay::CustomMcpListSwitchTimerCallback(lv_timer_t* timer) {
    auto* display = static_cast<LcdDisplay*>(lv_timer_get_user_data(timer));
    if (display == nullptr || !display->screensaver_active_
        || !display->custom_mcp_list_active_.load()
        || display->custom_mcp_list_items_.size() <= 1) {
        lv_timer_pause(timer);
        return;
    }
    display->custom_mcp_list_index_ =
        (display->custom_mcp_list_index_ + 1)
        % display->custom_mcp_list_items_.size();
    display->UpdateCustomMcpListItem();
}
