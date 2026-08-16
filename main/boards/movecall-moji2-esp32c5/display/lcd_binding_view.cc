/**
 * @file lcd_binding_view.cc
 * @brief 设备绑定码覆盖页面实现。
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
 * @brief 将当前主题字体应用到设备绑定页面，并保持固定的最终视觉尺寸。
 * @param font 主题中文字库；为空或行高无效时不改变页面。
 */
void LcdDisplay::ApplyDeviceBindingFont(const lv_font_t* font) {
    if (font == nullptr || font->line_height <= 0) {
        return;
    }

    const bool has_native_heavy_font = screensaver_weather_font_ != nullptr
        && screensaver_weather_font_->font() != nullptr;
    const lv_font_t* binding_text_font = font;
    if (has_native_heavy_font) {
        binding_text_font = screensaver_weather_font_->font();
    }
    const int title_scale = kBindingTitleTextPixelSize * 256 / binding_text_font->line_height;
    const int message_scale = has_native_heavy_font
        ? 256
        : kBindingMessageTextPixelSize * 256 / binding_text_font->line_height;

    if (binding_title_label_ != nullptr) {
        lv_obj_set_style_text_font(binding_title_label_, binding_text_font, 0);
        lv_obj_set_width(binding_title_label_,
                         kBindingMessageWidth * 256 / title_scale);
        lv_obj_set_style_transform_scale(binding_title_label_, title_scale, 0);
        lv_obj_set_style_transform_pivot_x(binding_title_label_, LV_PCT(50), 0);
        lv_obj_set_style_transform_pivot_y(binding_title_label_, LV_PCT(50), 0);
        lv_obj_align(binding_title_label_, LV_ALIGN_TOP_MID, 0,
                     kBindingTitleOffsetY);
    }
    if (binding_code_label_ != nullptr) {
        lv_obj_set_style_text_font(binding_code_label_, &font_puhui_time_64, 0);
        lv_obj_set_style_transform_scale(binding_code_label_, 256, 0);
        lv_obj_center(binding_code_label_);
    }
    if (binding_message_label_ != nullptr) {
        lv_obj_set_style_text_font(binding_message_label_, binding_text_font, 0);
        lv_obj_set_width(binding_message_label_,
                         kBindingMessageWidth * 256 / message_scale);
        lv_obj_set_style_transform_scale(binding_message_label_, message_scale, 0);
        lv_obj_set_style_transform_pivot_x(binding_message_label_, LV_PCT(50), 0);
        lv_obj_set_style_transform_pivot_y(binding_message_label_, LV_PCT(50), 0);
        if (binding_code_panel_ != nullptr
            && !lv_obj_has_flag(binding_code_panel_, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_align(binding_message_label_, LV_ALIGN_BOTTOM_MID, 0,
                         -kBindingMessageBottomOffset);
        } else {
            lv_obj_align(binding_message_label_, LV_ALIGN_CENTER, 0, 16);
        }
    }
}

/**
 * @brief 创建金属黑风格的圆屏设备绑定码页面。
 * @details 绑定页是屏幕根对象的独立子对象，并在创建后位于屏保对象之后。显示时再次移动到
 * 最前方，可确保语音会话结束触发屏保后，绑定码仍持续可见。
 */
void LcdDisplay::CreateDeviceBindingUI() {
    if (binding_container_ != nullptr) {
        return;
    }

    auto screen = lv_screen_active();
    const lv_font_t* text_font = &font_puhui_basic_20_4;
    if (current_theme_ != nullptr) {
        auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
        if (lvgl_theme->text_font() != nullptr
            && lvgl_theme->text_font()->font() != nullptr) {
            text_font = lvgl_theme->text_font()->font();
        }
    }

    binding_container_ = lv_obj_create(screen);
    lv_obj_set_size(binding_container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(binding_container_, 0, 0);
    lv_obj_set_style_bg_color(binding_container_,
                              lv_color_hex(kScreensaverBackgroundColor), 0);
    lv_obj_set_style_bg_opa(binding_container_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(binding_container_, 0, 0);
    lv_obj_set_style_pad_all(binding_container_, 0, 0);
    lv_obj_remove_flag(binding_container_, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* outer_metal = lv_obj_create(binding_container_);
    lv_obj_set_size(outer_metal, kScreensaverDialSize, kScreensaverDialSize);
    lv_obj_set_style_radius(outer_metal, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(outer_metal,
                              lv_color_hex(kScreensaverOuterMetalColor), 0);
    lv_obj_set_style_bg_opa(outer_metal, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(outer_metal,
                                  lv_color_hex(kScreensaverMetalBorderColor), 0);
    lv_obj_set_style_border_width(outer_metal, 2, 0);
    lv_obj_set_style_pad_all(outer_metal, 0, 0);
    lv_obj_remove_flag(outer_metal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(outer_metal);

    lv_obj_t* inner_metal = lv_obj_create(outer_metal);
    lv_obj_set_size(inner_metal, kScreensaverScaleSize, kScreensaverScaleSize);
    lv_obj_set_style_radius(inner_metal, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(inner_metal,
                              lv_color_hex(kScreensaverInnerMetalColor), 0);
    lv_obj_set_style_bg_opa(inner_metal, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(inner_metal, 0, 0);
    lv_obj_set_style_pad_all(inner_metal, 0, 0);
    lv_obj_remove_flag(inner_metal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(inner_metal);

    binding_title_label_ = lv_label_create(binding_container_);
    lv_label_set_text(binding_title_label_, "设备绑定");
    lv_obj_set_style_text_color(binding_title_label_, lv_color_hex(0xDDE1E4), 0);
    lv_obj_set_style_text_align(binding_title_label_, LV_TEXT_ALIGN_CENTER, 0);

    binding_code_panel_ = lv_obj_create(binding_container_);
    lv_obj_set_size(binding_code_panel_, kBindingCodePanelWidth,
                    kBindingCodePanelHeight);
    lv_obj_set_style_radius(binding_code_panel_, 8, 0);
    lv_obj_set_style_bg_color(binding_code_panel_,
                              lv_color_hex(kScreensaverBackgroundColor), 0);
    lv_obj_set_style_bg_opa(binding_code_panel_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(binding_code_panel_,
                                  lv_color_hex(kScreensaverMetalBorderColor), 0);
    lv_obj_set_style_border_width(binding_code_panel_, 2, 0);
    lv_obj_set_style_pad_all(binding_code_panel_, 0, 0);
    lv_obj_remove_flag(binding_code_panel_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(binding_code_panel_, LV_ALIGN_CENTER, 0, kBindingCodeOffsetY);

    binding_code_label_ = lv_label_create(binding_code_panel_);
    lv_label_set_text(binding_code_label_, "------");
    lv_obj_set_size(binding_code_label_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_text_color(binding_code_label_,
                                lv_color_hex(kScreensaverAccentColor), 0);
    lv_obj_set_style_text_align(binding_code_label_, LV_TEXT_ALIGN_CENTER, 0);

    binding_message_label_ = lv_label_create(binding_container_);
    lv_label_set_text(binding_message_label_, "正在获取绑定码...");
    lv_label_set_long_mode(binding_message_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(binding_message_label_,
                                lv_color_hex(kScreensaverSecondaryTextColor), 0);
    lv_obj_set_style_text_align(binding_message_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(binding_message_label_, 5, 0);

    ApplyDeviceBindingFont(text_font);
    lv_obj_add_flag(binding_container_, LV_OBJ_FLAG_HIDDEN);
}

/**
 * @brief 显示设备绑定页，并根据绑定码是否存在切换页面布局。
 * @param binding_code 用户输入网页端的短绑定码；空字符串表示加载、成功或失败状态。
 * @param message 页面说明文字。
 */
void LcdDisplay::ShowDeviceBinding(const std::string& binding_code,
                                   const std::string& message) {
    DisplayLockGuard lock(this);
    if (binding_container_ == nullptr || binding_code_panel_ == nullptr
        || binding_code_label_ == nullptr || binding_message_label_ == nullptr) {
        ESP_LOGW(TAG, "设备绑定界面尚未初始化");
        return;
    }

    binding_active_ = true;
    if (conversation_face_timer_ != nullptr) {
        lv_timer_pause(conversation_face_timer_);
    }
    SetLabelTextIfChanged(binding_code_label_, binding_code.c_str());
    SetLabelTextIfChanged(binding_message_label_, message.c_str());
    if (binding_code.empty()) {
        lv_obj_add_flag(binding_code_panel_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(binding_message_label_, LV_ALIGN_CENTER, 0, 16);
    } else {
        lv_obj_remove_flag(binding_code_panel_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(binding_message_label_, LV_ALIGN_BOTTOM_MID, 0,
                     -kBindingMessageBottomOffset);
    }
    lv_obj_remove_flag(binding_container_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(binding_container_);
    lv_obj_invalidate(binding_container_);
}

/**
 * @brief 隐藏设备绑定页，保留页面下方原有界面的状态。
 */
void LcdDisplay::HideDeviceBinding() {
    DisplayLockGuard lock(this);
    binding_active_ = false;
    if (binding_container_ != nullptr) {
        lv_obj_add_flag(binding_container_, LV_OBJ_FLAG_HIDDEN);
    }
    if (!screensaver_active_ && !audio_playback_mode_
        && conversation_face_timer_ != nullptr) {
        lv_timer_resume(conversation_face_timer_);
        lv_timer_reset(conversation_face_timer_);
        conversation_face_last_frame_key_ = UINT32_MAX;
        UpdateConversationFaceFrame();
    }
}
