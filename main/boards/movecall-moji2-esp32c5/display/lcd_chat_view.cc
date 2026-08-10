/**
 * @file lcd_chat_view.cc
 * @brief NOMI 风格对话表情、聊天布局、主题和字幕实现。
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
 * @brief 创建黑色背景上的 NOMI 风格双眼表情和低频动画定时器。
 */
void LcdDisplay::CreateConversationFaceUI() {
    if (conversation_face_ != nullptr) {
        return;
    }

    conversation_face_ = lv_obj_create(lv_screen_active());
    lv_obj_set_size(conversation_face_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(conversation_face_, 0, 0);
    lv_obj_set_style_radius(conversation_face_, 0, 0);
    lv_obj_set_style_bg_color(
        conversation_face_, lv_color_hex(kConversationFaceBackgroundColor), 0);
    lv_obj_set_style_bg_opa(conversation_face_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(conversation_face_, 0, 0);
    lv_obj_set_style_pad_all(conversation_face_, 0, 0);
    lv_obj_remove_flag(conversation_face_, LV_OBJ_FLAG_SCROLLABLE);

    auto create_eye = [this]() {
        auto* eye = lv_obj_create(conversation_face_);
        lv_obj_set_style_bg_opa(eye, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(eye, 0, 0);
        lv_obj_set_style_pad_all(eye, 0, 0);
        lv_obj_set_style_shadow_width(eye, 0, 0);
        lv_obj_set_style_transform_pivot_x(eye, LV_PCT(50), 0);
        lv_obj_set_style_transform_pivot_y(eye, LV_PCT(50), 0);
        lv_obj_remove_flag(eye, LV_OBJ_FLAG_SCROLLABLE);
        return eye;
    };
    conversation_left_eye_ = create_eye();
    conversation_right_eye_ = create_eye();

    conversation_face_timer_ = lv_timer_create(
        ConversationFaceTimerCallback,
        kConversationFaceTimerPeriodMs,
        this);
    conversation_face_tick_ = 2;
    UpdateConversationFaceMode(Lang::Strings::STANDBY);
}

/**
 * @brief 根据设备状态切换双眼的聆听、连接和播报动画。
 * @param status 当前设备状态文字。
 */
void LcdDisplay::UpdateConversationFaceMode(const char* status) {
    if (conversation_face_ == nullptr || status == nullptr) {
        return;
    }

    if (std::strcmp(status, Lang::Strings::LISTENING) == 0) {
        conversation_face_mode_ = ConversationFaceMode::Listening;
    } else if (std::strcmp(status, Lang::Strings::SPEAKING) == 0) {
        conversation_face_mode_ = ConversationFaceMode::Speaking;
    } else if (std::strcmp(status, Lang::Strings::CONNECTING) == 0
               || std::strcmp(status, Lang::Strings::PLEASE_WAIT) == 0) {
        conversation_face_mode_ = ConversationFaceMode::Connecting;
    } else {
        conversation_face_mode_ = ConversationFaceMode::Idle;
    }

    conversation_face_tick_ = 2;
    conversation_face_last_frame_key_ = UINT32_MAX;
    if (conversation_face_timer_ != nullptr) {
        if (screensaver_active_ || audio_playback_mode_) {
            lv_timer_pause(conversation_face_timer_);
        } else {
            lv_timer_resume(conversation_face_timer_);
            lv_timer_reset(conversation_face_timer_);
        }
    }
    UpdateConversationFaceFrame();
}

/**
 * @brief 根据云端情绪名称切换双眼表情。
 * @param emotion 云端下发的情绪名称。
 */
void LcdDisplay::UpdateConversationFaceExpression(const char* emotion) {
    if (emotion == nullptr) {
        conversation_face_expression_ = ConversationFaceExpression::Neutral;
    } else if (std::strcmp(emotion, "happy") == 0
               || std::strcmp(emotion, "laughing") == 0
               || std::strcmp(emotion, "loving") == 0
               || std::strcmp(emotion, "delicious") == 0
               || std::strcmp(emotion, "confident") == 0) {
        conversation_face_expression_ = ConversationFaceExpression::Happy;
    } else if (std::strcmp(emotion, "sad") == 0
               || std::strcmp(emotion, "crying") == 0
               || std::strcmp(emotion, "embarrassed") == 0) {
        conversation_face_expression_ = ConversationFaceExpression::Sad;
    } else if (std::strcmp(emotion, "angry") == 0) {
        conversation_face_expression_ = ConversationFaceExpression::Angry;
    } else if (std::strcmp(emotion, "thinking") == 0
               || std::strcmp(emotion, "confused") == 0) {
        conversation_face_expression_ = ConversationFaceExpression::Thinking;
    } else if (std::strcmp(emotion, "surprised") == 0
               || std::strcmp(emotion, "shocked") == 0) {
        conversation_face_expression_ = ConversationFaceExpression::Surprised;
    } else if (std::strcmp(emotion, "relaxed") == 0) {
        conversation_face_expression_ = ConversationFaceExpression::Relaxed;
    } else if (std::strcmp(emotion, "sleepy") == 0) {
        conversation_face_expression_ = ConversationFaceExpression::Sleepy;
    } else {
        conversation_face_expression_ = ConversationFaceExpression::Neutral;
    }

    conversation_face_tick_ = 2;
    conversation_face_last_frame_key_ = UINT32_MAX;
    UpdateConversationFaceFrame();
}

/**
 * @brief 更新 NOMI 风格双眼的当前动画帧。
 * @details 仅在形态发生变化时更新两个眼睛对象，避免持续触发布局和整屏刷新。
 */
void LcdDisplay::UpdateConversationFaceFrame() {
    if (conversation_face_ == nullptr || conversation_left_eye_ == nullptr
        || conversation_right_eye_ == nullptr || screensaver_active_
        || audio_playback_mode_
        || lv_obj_has_flag(conversation_face_, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }

    int left_width = 64;
    int left_height = 50;
    int right_width = 64;
    int right_height = 50;
    int left_y = 0;
    int right_y = 0;
    int left_rotation = 0;
    int right_rotation = 0;
    int horizontal_shift = 0;
    uint32_t phase = 0;
    uint32_t eye_color = kConversationFaceEyeColor;

    switch (conversation_face_expression_) {
        case ConversationFaceExpression::Happy:
            left_width = 68;
            right_width = 68;
            left_height = 40;
            right_height = 40;
            left_y = 3;
            right_y = 3;
            left_rotation = -50;
            right_rotation = 50;
            break;
        case ConversationFaceExpression::Sad:
            left_width = 62;
            right_width = 62;
            left_height = 36;
            right_height = 36;
            left_y = 6;
            right_y = 6;
            left_rotation = 70;
            right_rotation = -70;
            break;
        case ConversationFaceExpression::Angry:
            left_width = 62;
            right_width = 62;
            left_height = 36;
            right_height = 36;
            left_rotation = -80;
            right_rotation = 80;
            break;
        case ConversationFaceExpression::Thinking:
            left_width = 66;
            left_height = 52;
            right_width = 46;
            right_height = 36;
            right_y = -5;
            break;
        case ConversationFaceExpression::Surprised:
            left_width = 52;
            right_width = 52;
            left_height = 66;
            right_height = 66;
            break;
        case ConversationFaceExpression::Relaxed:
            left_width = 66;
            right_width = 66;
            left_height = 34;
            right_height = 34;
            left_y = 3;
            right_y = 3;
            break;
        case ConversationFaceExpression::Sleepy:
            left_width = 58;
            right_width = 58;
            left_height = 8;
            right_height = 8;
            left_y = 5;
            right_y = 5;
            break;
        case ConversationFaceExpression::Neutral:
        default:
            break;
    }

    switch (conversation_face_mode_) {
        case ConversationFaceMode::Connecting:
            phase = (conversation_face_tick_ / 6U) % 3U;
            horizontal_shift = phase == 0U ? -8 : (phase == 2U ? 8 : 0);
            left_width = 60;
            right_width = 60;
            left_height = 46;
            right_height = 46;
            left_rotation = 0;
            right_rotation = 0;
            break;
        case ConversationFaceMode::Listening:
            phase = (conversation_face_tick_ / 5U) % 2U;
            left_width = phase == 0U ? 66 : 72;
            right_width = left_width;
            left_height = phase == 0U ? 58 : 64;
            right_height = left_height;
            left_y = -2;
            right_y = -2;
            left_rotation = 0;
            right_rotation = 0;
            break;
        case ConversationFaceMode::Speaking: {
            phase = (conversation_face_tick_ / 2U) % 4U;
            left_width = phase == 2U ? 66 : 62;
            right_width = left_width;
            left_height = phase == 0U ? 44 : (phase == 2U ? 56 : 50);
            right_height = left_height;
            left_y = 0;
            right_y = 0;
            left_rotation = 0;
            right_rotation = 0;
            break;
        }
        case ConversationFaceMode::Idle:
        default: {
            const uint32_t blink_phase = conversation_face_tick_ % 48U;
            const bool blinking = conversation_face_tick_ > 2U && blink_phase <= 1U;
            phase = blinking ? 1U : 0U;
            if (blinking) {
                left_height = 5;
                right_height = 5;
                left_rotation = 0;
                right_rotation = 0;
            }
            break;
        }
    }

    const uint32_t frame_key =
        (static_cast<uint32_t>(conversation_face_mode_) << 24U)
        | (static_cast<uint32_t>(conversation_face_expression_) << 16U)
        | (phase << 8U)
        | static_cast<uint32_t>(horizontal_shift + 8);
    ++conversation_face_tick_;
    if (frame_key == conversation_face_last_frame_key_) {
        return;
    }
    conversation_face_last_frame_key_ = frame_key;

    const int center_x = LV_HOR_RES / 2;
    const int center_y = LV_VER_RES / 2 + kConversationFaceCenterOffsetY;
    const lv_color_t color = lv_color_hex(eye_color);
    const auto configure_eye = [center_y, color](lv_obj_t* eye, int width, int height,
                                                  int center_x_position, int y_offset,
                                                  int rotation) {
        lv_obj_set_size(eye, width, height);
        lv_obj_set_pos(eye, center_x_position - width / 2, center_y + y_offset - height / 2);
        lv_obj_set_style_radius(eye, height / 2, 0);
        lv_obj_set_style_bg_color(eye, color, 0);
        lv_obj_set_style_transform_rotation(eye, rotation, 0);
    };
    configure_eye(
        conversation_left_eye_, left_width, left_height,
        center_x - kConversationFaceEyeCenterOffsetX + horizontal_shift,
        left_y, left_rotation);
    configure_eye(
        conversation_right_eye_, right_width, right_height,
        center_x + kConversationFaceEyeCenterOffsetX + horizontal_shift,
        right_y, right_rotation);
}

/**
 * @brief LVGL 定时器回调，刷新双眼的眨眼和状态动画。
 * @param timer user_data 指向当前 LcdDisplay 实例。
 */
void LcdDisplay::ConversationFaceTimerCallback(lv_timer_t* timer) {
    auto* display = static_cast<LcdDisplay*>(lv_timer_get_user_data(timer));
    if (display != nullptr) {
        display->UpdateConversationFaceFrame();
    }
}

#if CONFIG_USE_WECHAT_MESSAGE_STYLE
/**
 * @brief 创建微信气泡消息风格的 LVGL 界面。
 * @details 保留可纵向滚动的多条消息气泡，用于显式选择微信消息样式时的兼容布局。
 */
void LcdDisplay::SetupUI() {
    // Prevent duplicate calls - if already called, return early
    if (setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() 被重复调用，已跳过重复初始化");
        return;
    }

    Display::SetupUI();  // Mark SetupUI as called
    DisplayLockGuard lock(this);

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);
    lv_obj_set_style_bg_color(screen, lvgl_theme->background_color(), 0);

    /* Container */
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(container_, 0, 0);
    lv_obj_set_flex_flow(container_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_pad_row(container_, 0, 0);
    lv_obj_set_style_bg_color(container_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_color(container_, lvgl_theme->border_color(), 0);

    /* Layer 1: Top bar - for status icons */
    top_bar_ = lv_obj_create(container_);
    lv_obj_set_size(top_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(top_bar_, 0, 0);
    lv_obj_set_style_bg_opa(top_bar_, LV_OPA_50, 0);  // 50% opacity background
    lv_obj_set_style_bg_color(top_bar_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_width(top_bar_, 0, 0);
    lv_obj_set_style_pad_all(top_bar_, 0, 0);
    lv_obj_set_style_pad_top(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_left(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_pad_right(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_flex_flow(top_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(top_bar_, LV_SCROLLBAR_MODE_OFF);

    // Left icon
    network_label_ = lv_label_create(top_bar_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, icon_font, 0);
    lv_obj_set_style_text_color(network_label_, lvgl_theme->text_color(), 0);

    // Right icons container
    lv_obj_t* right_icons = lv_obj_create(top_bar_);
    lv_obj_set_size(right_icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right_icons, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_icons, 0, 0);
    lv_obj_set_style_pad_all(right_icons, 0, 0);
    lv_obj_set_flex_flow(right_icons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right_icons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    mute_label_ = lv_label_create(right_icons);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, icon_font, 0);
    lv_obj_set_style_text_color(mute_label_, lvgl_theme->text_color(), 0);

    battery_label_ = lv_label_create(right_icons);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, icon_font, 0);
    lv_obj_set_style_text_color(battery_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_margin_left(battery_label_, lvgl_theme->spacing(2), 0);

    /* Layer 2: Status bar - for center text labels */
    status_bar_ = lv_obj_create(screen);
    lv_obj_set_size(status_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(status_bar_, 0, 0);
    lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);  // Transparent background
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_style_pad_top(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_scrollbar_mode(status_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_layout(status_bar_, LV_LAYOUT_NONE, 0);  // Use absolute positioning
    lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, 0);  // Overlap with top_bar_

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(notification_label_, LV_HOR_RES * 0.8);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(notification_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_align(notification_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(status_label_, LV_HOR_RES * 0.8);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);
    lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 0);

    /* Content - Chat area */
    content_ = lv_obj_create(container_);
    lv_obj_set_style_radius(content_, 0, 0);
    lv_obj_set_width(content_, LV_HOR_RES);
    lv_obj_set_flex_grow(content_, 1);
    lv_obj_set_style_pad_all(content_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_border_width(content_, 0, 0);
    lv_obj_set_style_bg_color(content_, lvgl_theme->chat_background_color(), 0); // Background for chat area

    // Enable scrolling for chat content
    lv_obj_set_scrollbar_mode(content_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(content_, LV_DIR_VER);

    // Create a flex container for chat messages
    lv_obj_set_flex_flow(content_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(content_, lvgl_theme->spacing(4), 0); // Space between messages

    // We'll create chat messages dynamically in SetChatMessage
    chat_message_label_ = nullptr;

    low_battery_popup_ = lv_obj_create(screen);
    lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(low_battery_popup_, LV_HOR_RES * 0.9, text_font->line_height * 2);
    lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, -lvgl_theme->spacing(4));
    lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);
    lv_obj_set_style_radius(low_battery_popup_, lvgl_theme->spacing(4), 0);
    low_battery_label_ = lv_label_create(low_battery_popup_);
    lv_label_set_text(low_battery_label_, Lang::Strings::BATTERY_NEED_CHARGE);
    lv_obj_set_style_text_color(low_battery_label_, lv_color_white(), 0);
    lv_obj_center(low_battery_label_);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);

    emoji_image_ = lv_img_create(screen);
    lv_obj_align(emoji_image_, LV_ALIGN_TOP_MID, 0, text_font->line_height + lvgl_theme->spacing(8));

    // Display AI logo while booting
    emoji_label_ = lv_label_create(screen);
    lv_obj_center(emoji_label_);
    lv_obj_set_style_text_font(emoji_label_, large_icon_font, 0);
    lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(emoji_label_, FONT_AWESOME_MICROCHIP_AI);

    CreateScreensaverUI();
    CreateDeviceBindingUI();
}
#if CONFIG_IDF_TARGET_ESP32P4
#define  MAX_MESSAGES 40
#else
#define  MAX_MESSAGES 20
#endif
/**
 * @brief 更新对应配置并同步到底层资源。
 * @details 实现会维护 LcdDisplay 的内部一致性；发生错误时记录日志，并避免向后续流程传播无效资源。
 */
void LcdDisplay::SetChatMessage(const char* role, const char* content) {
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() 尚未完成，聊天消息将无法显示，角色=%s，内容=%s", role, content);
    }
    DisplayLockGuard lock(this);
    if (content_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(TAG, "聊天消息显示失败，内容容器为空，角色=%s，内容=%s", role, content);
        }
        return;
    }

    // Check if message count exceeds limit
    uint32_t child_count = lv_obj_get_child_cnt(content_);
    if (child_count >= MAX_MESSAGES) {
        // Delete the oldest message (first child object)
        lv_obj_t* first_child = lv_obj_get_child(content_, 0);
        if (first_child != nullptr) {
            lv_obj_del(first_child);
            // Refresh child count after deletion
            child_count = lv_obj_get_child_cnt(content_);
        }
        // Scroll to the last message immediately (get last_child after deletion)
        if (child_count > 0) {
            lv_obj_t* last_child = lv_obj_get_child(content_, child_count - 1);
            if (last_child != nullptr && lv_obj_is_valid(last_child)) {
                lv_obj_scroll_to_view_recursive(last_child, LV_ANIM_OFF);
            }
        }
    }

    // Collapse system messages (if it's a system message, check if the last message is also a system message)
    if (strcmp(role, "system") == 0) {
        // Refresh child count to get accurate count after potential deletion above
        child_count = lv_obj_get_child_cnt(content_);
        if (child_count > 0) {
            // Get the last message container
            lv_obj_t* last_container = lv_obj_get_child(content_, child_count - 1);
            if (last_container != nullptr && lv_obj_is_valid(last_container) && lv_obj_get_child_cnt(last_container) > 0) {
                // Get the bubble inside the container
                lv_obj_t* last_bubble = lv_obj_get_child(last_container, 0);
                if (last_bubble != nullptr && lv_obj_is_valid(last_bubble)) {
                    // Check if bubble type is system message
                    void* bubble_type_ptr = lv_obj_get_user_data(last_bubble);
                    if (bubble_type_ptr != nullptr && strcmp((const char*)bubble_type_ptr, "system") == 0) {
                        // If the last message is also a system message, delete it
                        lv_obj_del(last_container);
                    }
                }
            }
        }
    } else {
        // Hide the centered AI logo
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }

    // Avoid empty message boxes
    if(strlen(content) == 0) {
        return;
    }

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);

    // Create a message bubble
    lv_obj_t* msg_bubble = lv_obj_create(content_);
    lv_obj_set_style_radius(msg_bubble, 8, 0);
    lv_obj_set_scrollbar_mode(msg_bubble, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_border_width(msg_bubble, 0, 0);
    lv_obj_set_style_pad_all(msg_bubble, lvgl_theme->spacing(4), 0);

    // Create the message text
    lv_obj_t* msg_text = lv_label_create(msg_bubble);
    lv_label_set_text(msg_text, content);

    // Calculate bubble width constraints
    lv_coord_t max_width = LV_HOR_RES * 85 / 100 - 16;  // 85% of screen width
    lv_coord_t min_width = 20;

    // Let LVGL calculate the natural text width first
    lv_obj_set_width(msg_text, LV_SIZE_CONTENT);
    lv_obj_update_layout(msg_text);
    lv_coord_t text_width = lv_obj_get_width(msg_text);

    // Ensure text width is not less than minimum width
    if (text_width < min_width) {
        text_width = min_width;
    }

    // Constrain to max width
    lv_coord_t bubble_width = (text_width < max_width) ? text_width : max_width;

    // Set message text width
    lv_obj_set_width(msg_text, bubble_width);
    lv_label_set_long_mode(msg_text, LV_LABEL_LONG_WRAP);

    // Set bubble width
    lv_obj_set_width(msg_bubble, bubble_width);
    lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);

    // Set alignment and style based on message role
    if (strcmp(role, "user") == 0) {
        // User messages are right-aligned with green background
        lv_obj_set_style_bg_color(msg_bubble, lvgl_theme->user_bubble_color(), 0);
        lv_obj_set_style_bg_opa(msg_bubble, LV_OPA_70, 0);
        // Set text color for contrast
        lv_obj_set_style_text_color(msg_text, lvgl_theme->text_color(), 0);

        // Set custom attribute to mark bubble type
        lv_obj_set_user_data(msg_bubble, (void*)"user");

        // Set appropriate width for content
        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);

        // Don't grow
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    } else if (strcmp(role, "assistant") == 0) {
        // Assistant messages are left-aligned with white background
        lv_obj_set_style_bg_color(msg_bubble, lvgl_theme->assistant_bubble_color(), 0);
        lv_obj_set_style_bg_opa(msg_bubble, LV_OPA_70, 0);
        // Set text color for contrast
        lv_obj_set_style_text_color(msg_text, lvgl_theme->text_color(), 0);

        // Set custom attribute to mark bubble type
        lv_obj_set_user_data(msg_bubble, (void*)"assistant");

        // Set appropriate width for content
        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);

        // Don't grow
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    } else if (strcmp(role, "system") == 0) {
        // System messages are center-aligned with light gray background
        lv_obj_set_style_bg_color(msg_bubble, lvgl_theme->system_bubble_color(), 0);
        lv_obj_set_style_bg_opa(msg_bubble, LV_OPA_70, 0);
        // Set text color for contrast
        lv_obj_set_style_text_color(msg_text, lvgl_theme->system_text_color(), 0);

        // Set custom attribute to mark bubble type
        lv_obj_set_user_data(msg_bubble, (void*)"system");

        // Set appropriate width for content
        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);

        // Don't grow
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    }

    // Create a full-width container for user messages to ensure right alignment
    if (strcmp(role, "user") == 0) {
        // Create a full-width container
        lv_obj_t* container = lv_obj_create(content_);
        lv_obj_set_width(container, LV_HOR_RES);
        lv_obj_set_height(container, LV_SIZE_CONTENT);

        // Make container transparent and borderless
        lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(container, 0, 0);
        lv_obj_set_style_pad_all(container, 0, 0);

        // Move the message bubble into this container
        lv_obj_set_parent(msg_bubble, container);

        // Right align the bubble in the container
        lv_obj_align(msg_bubble, LV_ALIGN_RIGHT_MID, -25, 0);

        // Auto-scroll to this container
        lv_obj_scroll_to_view_recursive(container, LV_ANIM_ON);
    } else if (strcmp(role, "system") == 0) {
        // Create full-width container for system messages to ensure center alignment
        lv_obj_t* container = lv_obj_create(content_);
        lv_obj_set_width(container, LV_HOR_RES);
        lv_obj_set_height(container, LV_SIZE_CONTENT);

        lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(container, 0, 0);
        lv_obj_set_style_pad_all(container, 0, 0);

        lv_obj_set_parent(msg_bubble, container);
        lv_obj_align(msg_bubble, LV_ALIGN_CENTER, 0, 0);
        lv_obj_scroll_to_view_recursive(container, LV_ANIM_ON);
    } else {
        // For assistant messages
        // Left align assistant messages
        lv_obj_align(msg_bubble, LV_ALIGN_LEFT_MID, 0, 0);

        // Auto-scroll to the message bubble
        lv_obj_scroll_to_view_recursive(msg_bubble, LV_ANIM_ON);
    }

    // Store reference to the latest message label
    chat_message_label_ = msg_text;
}

/**
 * @brief 临时显示预览图并在 PREVIEW_IMAGE_DURATION_MS 后恢复表情。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void LcdDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    DisplayLockGuard lock(this);
    if (content_ == nullptr) {
        return;
    }

    if (image == nullptr) {
        return;
    }

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    // Create a message bubble for image preview
    lv_obj_t* img_bubble = lv_obj_create(content_);
    lv_obj_set_style_radius(img_bubble, 8, 0);
    lv_obj_set_scrollbar_mode(img_bubble, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_border_width(img_bubble, 0, 0);
    lv_obj_set_style_pad_all(img_bubble, lvgl_theme->spacing(4), 0);

    // Set image bubble background color (similar to system message)
    lv_obj_set_style_bg_color(img_bubble, lvgl_theme->assistant_bubble_color(), 0);
    lv_obj_set_style_bg_opa(img_bubble, LV_OPA_70, 0);

    // Set custom attribute to mark bubble type
    lv_obj_set_user_data(img_bubble, (void*)"image");

    // Create the image object inside the bubble
    lv_obj_t* preview_image = lv_image_create(img_bubble);

    // Calculate appropriate size for the image
    lv_coord_t max_width = LV_HOR_RES * 70 / 100;  // 70% of screen width
    lv_coord_t max_height = LV_VER_RES * 50 / 100; // 50% of screen height

    // Calculate zoom factor to fit within maximum dimensions
    auto img_dsc = image->image_dsc();
    lv_coord_t img_width = img_dsc->header.w;
    lv_coord_t img_height = img_dsc->header.h;
    if (img_width == 0 || img_height == 0) {
        img_width = max_width;
        img_height = max_height;
        ESP_LOGW(TAG, "图片尺寸无效：%ld x %ld，改用默认尺寸：%ld x %ld", img_width, img_height, max_width, max_height);
    }

    lv_coord_t zoom_w = (max_width * 256) / img_width;
    lv_coord_t zoom_h = (max_height * 256) / img_height;
    lv_coord_t zoom = (zoom_w < zoom_h) ? zoom_w : zoom_h;

    // Ensure zoom doesn't exceed 256 (100%)
    if (zoom > 256) zoom = 256;

    // Set image properties
    lv_image_set_src(preview_image, img_dsc);
    lv_image_set_scale(preview_image, zoom);

    // Add event handler to clean up LvglImage when image is deleted
    // We need to transfer ownership of the unique_ptr to the event callback
    LvglImage* raw_image = image.release(); // Release ownership of smart pointer
    lv_obj_add_event_cb(preview_image, [](lv_event_t* e) {
        LvglImage* img = (LvglImage*)lv_event_get_user_data(e);
        if (img != nullptr) {
            delete img; // Properly release memory by deleting LvglImage object
        }
    }, LV_EVENT_DELETE, (void*)raw_image);

    // Calculate actual scaled image dimensions
    lv_coord_t scaled_width = (img_width * zoom) / 256;
    lv_coord_t scaled_height = (img_height * zoom) / 256;

    // Set bubble size to be 16 pixels larger than the image (8 pixels on each side)
    lv_obj_set_width(img_bubble, scaled_width + 16);
    lv_obj_set_height(img_bubble, scaled_height + 16);

    // Don't grow in flex layout
    lv_obj_set_style_flex_grow(img_bubble, 0, 0);

    // Center the image within the bubble
    lv_obj_center(preview_image);

    // Left align the image bubble like assistant messages
    lv_obj_align(img_bubble, LV_ALIGN_LEFT_MID, 0, 0);

    // Auto-scroll to the image bubble
    lv_obj_scroll_to_view_recursive(img_bubble, LV_ANIM_ON);
}

/**
 * @brief 执行 ClearChatMessages 对应的模块内部流程。
 * @details 实现会维护 LcdDisplay 的内部一致性；发生错误时记录日志，并避免向后续流程传播无效资源。
 */
void LcdDisplay::ClearChatMessages() {
    DisplayLockGuard lock(this);
    if (content_ == nullptr) {
        return;
    }

    // Use lv_obj_clean to delete all children of content_ (chat message bubbles)
    lv_obj_clean(content_);

    // Reset chat_message_label_ as it has been deleted
    chat_message_label_ = nullptr;

    // Show the centered AI logo (emoji_label_) again
    if (emoji_label_ != nullptr) {
        lv_obj_remove_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }

    ESP_LOGI(TAG, "聊天消息已清除");
}
#else
/**
 * @brief 创建适配 360x360 圆屏的默认 LVGL 界面。
 * @details 顶部使用收窄的图标栏和状态栏，中央保留表情及预览图区域，
 *          底部使用固定两行的大字字幕容器。所有关键控件均位于圆形可见安全区内。
 */
void LcdDisplay::SetupUI() {
    // Prevent duplicate calls - if already called, return early
    if (setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() 被重复调用，已跳过重复初始化");
        return;
    }

    Display::SetupUI();  // Mark SetupUI as called
    DisplayLockGuard lock(this);
    LvglTheme* lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    const lv_font_t* status_font = &font_puhui_basic_20_4;
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lv_color_white(), 0);
    lv_obj_set_style_bg_color(screen, lv_color_hex(kConversationFaceBackgroundColor), 0);

    /* Container - used as background */
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(container_, 0, 0);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_bg_color(container_, lv_color_hex(kConversationFaceBackgroundColor), 0);
    lv_obj_set_style_bg_opa(container_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(container_, lv_color_hex(kConversationFaceBackgroundColor), 0);

    CreateConversationFaceUI();

    /* Bottom layer: emoji_box_ - centered display */
    emoji_box_ = lv_obj_create(screen);
    lv_obj_set_size(emoji_box_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(emoji_box_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(emoji_box_, 0, 0);
    lv_obj_set_style_border_width(emoji_box_, 0, 0);
    lv_obj_align(emoji_box_, LV_ALIGN_CENTER, 0, kRoundEmojiOffsetY);

    emoji_label_ = lv_label_create(emoji_box_);
    lv_obj_set_style_text_font(emoji_label_, large_icon_font, 0);
    lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(emoji_label_, FONT_AWESOME_MICROCHIP_AI);

    emoji_image_ = lv_img_create(emoji_box_);
    lv_obj_center(emoji_image_);
    lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);

    /* Middle layer: preview_image_ - centered display */
    preview_image_ = lv_image_create(screen);
    lv_obj_set_size(preview_image_, width_ / 2, height_ / 2);
    lv_obj_align(preview_image_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);

    /*
     * 第一层顶部状态栏固定为三栏：左侧网络、中间时间、右侧电量。
     */
    top_bar_ = lv_obj_create(screen);
    lv_obj_set_size(top_bar_, kRoundTopBarWidth, kRoundTopBarHeight);
    lv_obj_set_style_radius(top_bar_, kRoundTopBarHeight / 2, 0);
    lv_obj_set_style_bg_opa(top_bar_, LV_OPA_50, 0);
    lv_obj_set_style_bg_color(top_bar_, lv_color_hex(kConversationFaceBackgroundColor), 0);
    lv_obj_set_style_border_width(top_bar_, 0, 0);
    lv_obj_set_style_pad_all(top_bar_, 0, 0);
    lv_obj_set_style_pad_top(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_left(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_pad_right(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_pad_column(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_flex_flow(top_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(top_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(top_bar_, LV_ALIGN_TOP_MID, 0, kRoundTopBarOffsetY);

    /*
     * 左侧固定显示网络状态图标。标签使用与右侧电量标签相同的固定宽度，
     * 从而使中间时间标签的几何中心始终与屏幕中线重合。
     */
    network_label_ = lv_label_create(top_bar_);
    lv_obj_set_width(network_label_, kRoundTopBarSideWidth);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, icon_font, 0);
    lv_obj_set_style_text_align(network_label_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(network_label_, lv_color_white(), 0);

    /*
     * 中间时间标签占用顶部栏的全部剩余宽度，并始终保持居中。
     * 时间与设备状态使用不同的标签，避免语音状态或通知内容覆盖时钟。
     */
    time_label_ = lv_label_create(top_bar_);
    lv_obj_set_width(time_label_, 1);
    lv_obj_set_flex_grow(time_label_, 1);
    lv_obj_set_style_text_font(time_label_, status_font, 0);
    lv_obj_set_style_text_align(time_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(time_label_, lv_color_white(), 0);
    lv_label_set_text(time_label_, "--:--");

    /*
     * 右侧只显示电量图标。固定宽度与左侧网络标签完全一致，文字右对齐，
     * 既维持“网络、时间、电量”的三栏顺序，也避免电量图标挤压时钟位置。
     */
    battery_label_ = lv_label_create(top_bar_);
    lv_obj_set_width(battery_label_, kRoundTopBarSideWidth);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, icon_font, 0);
    lv_obj_set_style_text_align(battery_label_, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(battery_label_, lv_color_white(), 0);

    /*
     * 设备运行状态和临时通知保留在顶部栏下方的独立区域中。
     * 该区域不属于顶部三栏，因此 SetStatus() 和 ShowNotification() 不会影响时间标签。
     */
    status_bar_ = lv_obj_create(screen);
    lv_obj_set_size(status_bar_, kRoundTopBarWidth, kRoundTopBarHeight);
    lv_obj_set_style_radius(status_bar_, 0, 0);
    lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_scrollbar_mode(status_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_layout(status_bar_, LV_LAYOUT_NONE, 0);
    lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0,
                 kRoundTopBarOffsetY + kRoundTopBarHeight);

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(notification_label_, LV_PCT(100));
    lv_obj_set_style_text_font(notification_label_, status_font, 0);
    lv_label_set_long_mode(notification_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(notification_label_, lv_color_white(), 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_align(notification_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(status_label_, LV_PCT(100));
    lv_obj_set_style_text_font(status_label_, status_font, 0);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label_, lv_color_white(), 0);
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);
    lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 0);

    /*
     * 圆屏字幕区使用固定两行高度。不让容器随长文本无限向上扩展，
     * 否则会遮挡中央表情并进入圆屏下方的不可见区。
     */
    bottom_bar_ = lv_obj_create(screen);
    const int subtitle_text_height = text_font->line_height * 2 + kRoundSubtitleLineSpacing;
    lv_obj_set_size(bottom_bar_, kRoundSubtitleWidth,
                    ScaleSubtitleSize(subtitle_text_height) + lvgl_theme->spacing(8));
    lv_obj_set_style_radius(bottom_bar_, lvgl_theme->spacing(10), 0);
    lv_obj_set_style_bg_color(bottom_bar_, lv_color_hex(kConversationFaceBackgroundColor), 0);
    lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_80, 0);
    lv_obj_set_style_text_color(bottom_bar_, lv_color_white(), 0);
    lv_obj_set_style_pad_all(bottom_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_border_width(bottom_bar_, 0, 0);
    lv_obj_set_scrollbar_mode(bottom_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, -kRoundSubtitleBottomOffset);

    /*
     * 字幕容器始终只显示两行；标签自身保存完整文本，超过两行时由
     * UpdateSubtitleScroll() 在容器裁切区域内执行纵向滚动。
     */
    chat_message_label_ = lv_label_create(bottom_bar_);
    lv_label_set_text(chat_message_label_, "");
    lv_obj_set_size(chat_message_label_, kRoundSubtitleLogicalTextWidth, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(chat_message_label_, text_font, 0);
    lv_obj_set_style_text_line_space(chat_message_label_, kRoundSubtitleLineSpacing, 0);
    lv_obj_set_style_transform_scale(chat_message_label_, kRoundSubtitleScale, 0);
    lv_obj_set_style_transform_pivot_x(chat_message_label_, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(chat_message_label_, 0, 0);
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(chat_message_label_, lv_color_white(), 0);
    lv_obj_align(chat_message_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);

    low_battery_popup_ = lv_obj_create(screen);
    lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(low_battery_popup_, kRoundPopupWidth, status_font->line_height * 2);
    lv_obj_align(low_battery_popup_, LV_ALIGN_CENTER, 0, 72);
    lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);
    lv_obj_set_style_radius(low_battery_popup_, lvgl_theme->spacing(4), 0);

    low_battery_label_ = lv_label_create(low_battery_popup_);
    lv_label_set_text(low_battery_label_, Lang::Strings::BATTERY_NEED_CHARGE);
    lv_obj_set_style_text_font(low_battery_label_, status_font, 0);
    lv_obj_set_style_text_color(low_battery_label_, lv_color_white(), 0);
    lv_obj_center(low_battery_label_);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);

    CreateScreensaverUI();
    CreateDeviceBindingUI();
}

/**
 * @brief 临时显示预览图并在 PREVIEW_IMAGE_DURATION_MS 后恢复表情。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void LcdDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    DisplayLockGuard lock(this);
    if (preview_image_ == nullptr) {
        ESP_LOGE(TAG, "预览图片尚未初始化");
        return;
    }

    if (image == nullptr) {
        esp_timer_stop(preview_timer_);
        if (conversation_face_ != nullptr) {
            lv_obj_remove_flag(conversation_face_, LV_OBJ_FLAG_HIDDEN);
            conversation_face_last_frame_key_ = UINT32_MAX;
            UpdateConversationFaceFrame();
        }
        if (!screensaver_active_ && !audio_playback_mode_
            && conversation_face_timer_ != nullptr) {
            lv_timer_resume(conversation_face_timer_);
            lv_timer_reset(conversation_face_timer_);
        }
        lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
        preview_image_cached_.reset();
        return;
    }

    preview_image_cached_ = std::move(image);
    auto img_dsc = preview_image_cached_->image_dsc();
    lv_image_set_src(preview_image_, img_dsc);
    if (img_dsc->header.w > 0 && img_dsc->header.h > 0) {
        // zoom factor 0.5
        lv_image_set_scale(preview_image_, 128 * width_ / img_dsc->header.w);
    }

    if (conversation_face_ != nullptr) {
        lv_obj_add_flag(conversation_face_, LV_OBJ_FLAG_HIDDEN);
    }
    if (conversation_face_timer_ != nullptr) {
        lv_timer_pause(conversation_face_timer_);
    }
    lv_obj_remove_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
    esp_timer_stop(preview_timer_);
    ESP_ERROR_CHECK(esp_timer_start_once(preview_timer_, PREVIEW_IMAGE_DURATION_MS * 1000));
}

/**
 * @brief 更新圆屏底部的对话字幕。
 * @param role 消息角色，用于日志诊断；默认圆屏布局不按角色改变字幕位置。
 * @param content UTF-8 字幕文本；nullptr 或空字符串会隐藏字幕区。
 * @details 方法完整保留云端下发的 UTF-8 文本，使用 30 px 字形源换行并缩放到目标字号。
 *          两行以内的内容静止居中；超过两行时在固定窗口内纵向滚动，不再截断回答尾部。
 */
void LcdDisplay::SetChatMessage(const char* role, const char* content) {
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() 尚未完成，聊天消息将无法显示，角色=%s，内容=%s", role, content);
    }
    DisplayLockGuard lock(this);
    if (chat_message_label_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(TAG, "聊天消息显示失败，消息标签为空，角色=%s，内容=%s", role, content);
        }
        return;
    }

    /*
     * 新字幕到达时先同步终止上一段字幕动画并恢复初始位置，再替换标签内容。
     * 这一步放在 lv_label_set_text() 之前，确保旧动画不会在文本替换过程中继续写入
     * translate_y，从而让新字幕能够立即从第一行显示。
     */
    lv_anim_delete(chat_message_label_, nullptr);
    lv_obj_set_style_translate_y(chat_message_label_, 0, 0);
    lv_obj_align(chat_message_label_, LV_ALIGN_CENTER, 0, 0);

    const char* complete_text = content != nullptr ? content : "";
    lv_label_set_text(chat_message_label_, complete_text);
    if (!audio_playback_mode_) {
        UpdateSubtitleScroll();
    }
    lv_obj_invalidate(chat_message_label_);

    // Show bottom_bar_ only when there is content (and subtitle is not globally hidden)
    if (bottom_bar_ != nullptr) {
        if (complete_text[0] == '\0') {
            lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        } else if (!hide_subtitle_) {
            lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, -kRoundSubtitleBottomOffset);
        lv_obj_invalidate(bottom_bar_);
    }
}

/**
 * @brief 执行 ClearChatMessages 对应的模块内部流程。
 * @details 实现会维护 LcdDisplay 的内部一致性；发生错误时记录日志，并避免向后续流程传播无效资源。
 */
void LcdDisplay::ClearChatMessages() {
    DisplayLockGuard lock(this);
    // In non-wechat mode, just clear the chat message label and hide the bar
    if (chat_message_label_ != nullptr) {
        lv_label_set_text(chat_message_label_, "");
        UpdateSubtitleScroll();
    }
    if (bottom_bar_ != nullptr) {
        lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
    }
}
#endif

/**
 * @brief 更新顶部状态并同步 NOMI 风格双眼动画模式。
 * @param status UTF-8 状态文字。
 */
void LcdDisplay::SetStatus(const char* status) {
    LvglDisplay::SetStatus(status);
    DisplayLockGuard lock(this);
    UpdateConversationFaceMode(status);
}

/**
 * @brief 根据云端情绪更新当前对话表情。
 * @param emotion 云端下发的情绪名称。
 * @details 默认圆屏布局把情绪映射为 NOMI 风格双眼形态；微信气泡布局继续兼容静态图片和 GIF。
 */
void LcdDisplay::SetEmotion(const char* emotion) {
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() 尚未完成，表情无法显示，表情=%s", emotion);
    }
#if !CONFIG_USE_WECHAT_MESSAGE_STYLE
    {
        DisplayLockGuard lock(this);
        UpdateConversationFaceExpression(emotion);
    }
    return;
#endif
    if (emoji_image_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(TAG, "表情显示失败，表情图片为空，表情=%s", emotion);
        }
        return;
    }

    auto emoji_collection = static_cast<LvglTheme*>(current_theme_)->emoji_collection();
    auto image = emoji_collection != nullptr ? emoji_collection->GetEmojiImage(emotion) : nullptr;
    if (image == nullptr) {
        const char* utf8 = font_awesome_get_utf8(emotion);
        if (utf8 != nullptr && emoji_label_ != nullptr) {
            DisplayLockGuard lock(this);
            if (gif_controller_) {
                gif_controller_->Stop();
                gif_controller_.reset();
            }
            lv_label_set_text(emoji_label_, utf8);
            lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    DisplayLockGuard lock(this);
    // Stop any running GIF animation in the same lock scope as setting new image
    // to prevent LVGL from accessing freed image data between operations
    if (gif_controller_) {
        gif_controller_->Stop();
        gif_controller_.reset();
    }
    if (image->IsGif()) {
        // Create new GIF controller
        gif_controller_ = std::make_unique<LvglGif>(image->image_dsc());

        if (gif_controller_->IsLoaded()) {
            // Set up frame update callback
            gif_controller_->SetFrameCallback([this]() {
                lv_image_set_src(emoji_image_, gif_controller_->image_dsc());
            });

            // Set initial frame and start animation
            lv_image_set_src(emoji_image_, gif_controller_->image_dsc());
            if (!audio_playback_mode_ && !screensaver_active_) {
                gif_controller_->Start();
            }

            // Show GIF, hide others
            lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        } else {
            ESP_LOGE(TAG, "表情 GIF 加载失败：%s", emotion);
            gif_controller_.reset();
        }
    } else {
        lv_image_set_src(emoji_image_, image->image_dsc());
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
    }

#if CONFIG_USE_WECHAT_MESSAGE_STYLE
    // In WeChat message style, if emotion is neutral, don't display it
    uint32_t child_count = lv_obj_get_child_cnt(content_);
    if (strcmp(emotion, "neutral") == 0 && child_count > 0) {
        // Stop GIF animation if running
        if (gif_controller_) {
            gif_controller_->Stop();
            gif_controller_.reset();
        }

        lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }
#endif
}

/**
 * @brief 将主题颜色、字体、背景和表情集合应用到已创建控件。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void LcdDisplay::SetTheme(Theme* theme) {
    DisplayLockGuard lock(this);

    auto lvgl_theme = static_cast<LvglTheme*>(theme);

    // Get the active screen
    lv_obj_t* screen = lv_screen_active();

    // Set font
    auto text_font = lvgl_theme->text_font()->font();
    const lv_font_t* status_font = &font_puhui_basic_20_4;
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    LoadScreensaverWeatherFont();
    const lv_font_t* chat_status_font = screensaver_weather_font_ != nullptr
        && screensaver_weather_font_->font() != nullptr
        ? screensaver_weather_font_->font()
        : text_font;

    if (text_font->line_height >= 40) {
        if (mute_label_ != nullptr) {
            lv_obj_set_style_text_font(mute_label_, large_icon_font, 0);
        }
        if (battery_label_ != nullptr) {
            lv_obj_set_style_text_font(battery_label_, large_icon_font, 0);
        }
        if (network_label_ != nullptr) {
            lv_obj_set_style_text_font(network_label_, large_icon_font, 0);
        }
    } else {
        if (mute_label_ != nullptr) {
            lv_obj_set_style_text_font(mute_label_, icon_font, 0);
        }
        if (battery_label_ != nullptr) {
            lv_obj_set_style_text_font(battery_label_, icon_font, 0);
        }
        if (network_label_ != nullptr) {
            lv_obj_set_style_text_font(network_label_, icon_font, 0);
        }
    }

    // Set parent text color
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);

    /*
     * 默认资源加载完成后，主题中的 text_font 会从精简内置字体替换为完整中文字库。
     * 表盘标签设置了局部字体，不会自动继承屏幕字体，因此需要在此显式同步。
     */
    ApplyScreensaverTextFont(text_font);
    ApplyCustomMcpListFont(text_font);
    ApplyScreensaverLocationFont(text_font);
    ApplyScreensaverWeatherFont(text_font);
    ApplyScreensaverDateFont(text_font);
    ApplyScreensaverStatusIconFont(icon_font);
    ApplyDeviceBindingFont(text_font);
    UpdateScreensaverMemo();
    if (custom_mcp_list_active_.load()) {
        UpdateCustomMcpListItem();
    }

    // Set background image
    if (lvgl_theme->background_image() != nullptr) {
        lv_obj_set_style_bg_image_src(container_, lvgl_theme->background_image()->image_dsc(), 0);
    } else {
        lv_obj_set_style_bg_image_src(container_, nullptr, 0);
        lv_obj_set_style_bg_color(container_, lvgl_theme->background_color(), 0);
    }

    // Update top bar background color with 50% opacity
    if (top_bar_ != nullptr) {
        lv_obj_set_style_bg_opa(top_bar_, LV_OPA_50, 0);
        lv_obj_set_style_bg_color(top_bar_, lvgl_theme->background_color(), 0);
    }

    // Update status bar elements
    if (network_label_ != nullptr) {
        lv_obj_set_style_text_color(network_label_, lvgl_theme->text_color(), 0);
    }
    if (time_label_ != nullptr) {
        lv_obj_set_style_text_font(time_label_, status_font, 0);
        lv_obj_set_style_text_color(time_label_, lvgl_theme->text_color(), 0);
    }
    if (status_label_ != nullptr) {
        // 表情上方的聊天标题使用完整 Heavy 24px；WiFi 临时提示仍由 notification_label_ 使用 20px。
        lv_obj_set_style_text_font(status_label_, chat_status_font, 0);
        lv_obj_set_style_text_color(status_label_, lvgl_theme->text_color(), 0);
    }
    if (notification_label_ != nullptr) {
        lv_obj_set_style_text_font(notification_label_, status_font, 0);
        lv_obj_set_style_text_color(notification_label_, lvgl_theme->text_color(), 0);
    }
    if (mute_label_ != nullptr) {
        lv_obj_set_style_text_color(mute_label_, lvgl_theme->text_color(), 0);
    }
    if (battery_label_ != nullptr) {
        lv_obj_set_style_text_color(battery_label_, lvgl_theme->text_color(), 0);
    }
    lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);

    // If we have the chat message style, update all message bubbles
#if CONFIG_USE_WECHAT_MESSAGE_STYLE
    // Set content background opacity
    lv_obj_set_style_bg_opa(content_, LV_OPA_TRANSP, 0);

    // Iterate through all children of content (message containers or bubbles)
    uint32_t child_count = lv_obj_get_child_cnt(content_);
    for (uint32_t i = 0; i < child_count; i++) {
        lv_obj_t* obj = lv_obj_get_child(content_, i);
        if (obj == nullptr) continue;

        lv_obj_t* bubble = nullptr;

        // Check if this object is a container or bubble
        // If it's a container (user or system message), get its child as bubble
        // If it's a bubble (assistant message), use it directly
        if (lv_obj_get_child_cnt(obj) > 0) {
            // Might be a container, check if it's a user or system message container
            // User and system message containers are transparent
            lv_opa_t bg_opa = lv_obj_get_style_bg_opa(obj, LV_PART_MAIN);
            if (bg_opa == LV_OPA_TRANSP) {
                // This is a user or system message container
                bubble = lv_obj_get_child(obj, 0);
            } else {
                // This might be an assistant message bubble itself
                bubble = obj;
            }
        } else {
            // No child elements, might be other UI elements, skip
            continue;
        }

        if (bubble == nullptr) continue;

        // Use saved user data to identify bubble type
        void* bubble_type_ptr = lv_obj_get_user_data(bubble);
        if (bubble_type_ptr != nullptr) {
            const char* bubble_type = static_cast<const char*>(bubble_type_ptr);

            // Apply correct color based on bubble type
            if (strcmp(bubble_type, "user") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->user_bubble_color(), 0);
            } else if (strcmp(bubble_type, "assistant") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->assistant_bubble_color(), 0);
            } else if (strcmp(bubble_type, "system") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->system_bubble_color(), 0);
            } else if (strcmp(bubble_type, "image") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->system_bubble_color(), 0);
            }

            // Update border color
            lv_obj_set_style_border_color(bubble, lvgl_theme->border_color(), 0);

            // Update text color for the message
            if (lv_obj_get_child_cnt(bubble) > 0) {
                lv_obj_t* text = lv_obj_get_child(bubble, 0);
                if (text != nullptr) {
                    // Set text color based on bubble type
                    if (strcmp(bubble_type, "system") == 0) {
                        lv_obj_set_style_text_color(text, lvgl_theme->system_text_color(), 0);
                    } else {
                        lv_obj_set_style_text_color(text, lvgl_theme->text_color(), 0);
                    }
                }
            }
        } else {
            ESP_LOGW(TAG, "未找到子消息气泡类型，下标=%lu", i);
        }
    }
#else
    // 默认圆屏对话界面固定使用 NOMI 风格黑色背景，不跟随浅色主题切换。
    lv_obj_set_style_bg_color(screen, lv_color_hex(kConversationFaceBackgroundColor), 0);
    lv_obj_set_style_text_color(screen, lv_color_white(), 0);
    if (container_ != nullptr) {
        lv_obj_set_style_bg_image_src(container_, nullptr, 0);
        lv_obj_set_style_bg_color(container_, lv_color_hex(kConversationFaceBackgroundColor), 0);
        lv_obj_set_style_bg_opa(container_, LV_OPA_COVER, 0);
    }
    if (top_bar_ != nullptr) {
        lv_obj_set_style_bg_color(top_bar_, lv_color_hex(kConversationFaceBackgroundColor), 0);
    }
    if (network_label_ != nullptr) {
        lv_obj_set_style_text_color(network_label_, lv_color_white(), 0);
    }
    if (time_label_ != nullptr) {
        lv_obj_set_style_text_color(time_label_, lv_color_white(), 0);
    }
    if (battery_label_ != nullptr) {
        lv_obj_set_style_text_color(battery_label_, lv_color_white(), 0);
    }
    if (status_label_ != nullptr) {
        lv_obj_set_style_text_color(status_label_, lv_color_white(), 0);
    }
    if (notification_label_ != nullptr) {
        lv_obj_set_style_text_color(notification_label_, lv_color_white(), 0);
    }

    if (chat_message_label_ != nullptr) {
        const int subtitle_text_height = text_font->line_height * 2 + kRoundSubtitleLineSpacing;
        lv_obj_set_size(chat_message_label_, kRoundSubtitleLogicalTextWidth, LV_SIZE_CONTENT);
        lv_obj_set_style_text_font(chat_message_label_, text_font, 0);
        lv_obj_set_style_text_line_space(chat_message_label_, kRoundSubtitleLineSpacing, 0);
        lv_obj_set_style_transform_scale(chat_message_label_, kRoundSubtitleScale, 0);
        lv_obj_set_style_transform_pivot_x(chat_message_label_, LV_PCT(50), 0);
        lv_obj_set_style_transform_pivot_y(chat_message_label_, 0, 0);
        lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(chat_message_label_, lv_color_white(), 0);
        if (bottom_bar_ != nullptr) {
            lv_obj_set_size(bottom_bar_, kRoundSubtitleWidth,
                            ScaleSubtitleSize(subtitle_text_height) + lvgl_theme->spacing(8));
            lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, -kRoundSubtitleBottomOffset);
        }
        UpdateSubtitleScroll();
    }

    if (emoji_label_ != nullptr) {
        lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);
    }

    // 字幕背景与主界面保持纯黑，避免出现主题色块。
    if (bottom_bar_ != nullptr) {
        lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_80, 0);
        lv_obj_set_style_bg_color(bottom_bar_, lv_color_hex(kConversationFaceBackgroundColor), 0);
        lv_obj_set_style_text_color(bottom_bar_, lv_color_white(), 0);
    }
#endif

    // Update low battery popup
    lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);
    lv_obj_set_style_text_font(low_battery_label_, status_font, 0);

    // No errors occurred. Save theme to settings
    Display::SetTheme(lvgl_theme);
}

/**
 * @brief 在 assets 分区解除映射前释放屏保天气字体。
 * @details 先在 LVGL 锁内将天气和日期标签切换回主题字体，再释放指向旧 mmap 区域的
 *          cbin 字体描述符，避免资源下载后继续访问失效的字形数据。
 */
void LcdDisplay::ReleaseAssetsForReload() {
    if (screensaver_weather_font_ == nullptr) {
        return;
    }

    DisplayLockGuard lock(this);
    const lv_font_t* fallback_font = &font_puhui_basic_20_4;
    if (current_theme_ != nullptr) {
        auto* theme = static_cast<LvglTheme*>(current_theme_);
        if (theme->text_font() != nullptr && theme->text_font()->font() != nullptr) {
            fallback_font = theme->text_font()->font();
        }
    }
    screensaver_weather_font_.reset();
    ApplyScreensaverLocationFont(fallback_font);
    ApplyScreensaverWeatherFont(fallback_font);
    ApplyScreensaverDateFont(fallback_font);
}

/**
 * @brief 设置是否隐藏字幕。
 * @param hide true 隐藏并清空当前字幕。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void LcdDisplay::SetHideSubtitle(bool hide) {
    DisplayLockGuard lock(this);
    hide_subtitle_ = hide;

    // Immediately update UI visibility based on the setting
    if (bottom_bar_ != nullptr) {
        if (hide) {
            if (chat_message_label_ != nullptr) {
                lv_anim_delete(chat_message_label_, nullptr);
                lv_obj_set_style_translate_y(chat_message_label_, 0, 0);
            }
            lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        } else {
            // Only show if there is actual content to display
            const char* text = (chat_message_label_ != nullptr) ? lv_label_get_text(chat_message_label_) : nullptr;
            if (text != nullptr && text[0] != '\0') {
                UpdateSubtitleScroll();
                lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}
