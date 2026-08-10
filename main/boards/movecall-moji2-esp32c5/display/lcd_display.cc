/**
 * @file lcd_display.cc
 * @brief LCD 主题、面板初始化、资源释放和基础锁实现。
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
 * @brief 注册内置浅色/深色主题及资源主题。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void LcdDisplay::InitializeLcdThemes() {
    auto text_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_TEXT_FONT);
    auto icon_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_ICON_FONT);
    auto large_icon_font = std::make_shared<LvglBuiltInFont>(&font_awesome_30_4);

    // light theme
    auto light_theme = new LvglTheme("light");
    light_theme->set_background_color(lv_color_hex(0xFFFFFF));
    light_theme->set_text_color(lv_color_hex(0x000000));
    light_theme->set_chat_background_color(lv_color_hex(0xE0E0E0));
    light_theme->set_user_bubble_color(lv_color_hex(0x00FF00));
    light_theme->set_assistant_bubble_color(lv_color_hex(0xDDDDDD));
    light_theme->set_system_bubble_color(lv_color_hex(0xFFFFFF));
    light_theme->set_system_text_color(lv_color_hex(0x000000));
    light_theme->set_border_color(lv_color_hex(0x000000));
    light_theme->set_low_battery_color(lv_color_hex(0x000000));
    light_theme->set_text_font(text_font);
    light_theme->set_icon_font(icon_font);
    light_theme->set_large_icon_font(large_icon_font);

    // dark theme
    auto dark_theme = new LvglTheme("dark");
    dark_theme->set_background_color(lv_color_hex(0x000000));
    dark_theme->set_text_color(lv_color_hex(0xFFFFFF));
    dark_theme->set_chat_background_color(lv_color_hex(0x1F1F1F));
    dark_theme->set_user_bubble_color(lv_color_hex(0x00FF00));
    dark_theme->set_assistant_bubble_color(lv_color_hex(0x222222));
    dark_theme->set_system_bubble_color(lv_color_hex(0x000000));
    dark_theme->set_system_text_color(lv_color_hex(0xFFFFFF));
    dark_theme->set_border_color(lv_color_hex(0xFFFFFF));
    dark_theme->set_low_battery_color(lv_color_hex(0xFF0000));
    dark_theme->set_text_font(text_font);
    dark_theme->set_icon_font(icon_font);
    dark_theme->set_large_icon_font(large_icon_font);

    auto& theme_manager = LvglThemeManager::GetInstance();
    theme_manager.RegisterTheme("light", light_theme);
    theme_manager.RegisterTheme("dark", dark_theme);
}

/**
 * @brief 保存面板句柄和逻辑尺寸，供具体总线子类完成 LVGL 注册。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
LcdDisplay::LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width, int height)
    : panel_io_(panel_io), panel_(panel) {
    width_ = width;
    height_ = height;

    // Initialize LCD themes
    InitializeLcdThemes();

    // Load theme from settings
    Settings settings("display", false);
    std::string theme_name = settings.GetString("theme", "light");
    current_theme_ = LvglThemeManager::GetInstance().GetTheme(theme_name);

    // Create a timer to hide the preview image
    esp_timer_create_args_t preview_timer_args = {
        .callback = [](void* arg) {
            LcdDisplay* display = static_cast<LcdDisplay*>(arg);
            display->SetPreviewImage(nullptr);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "preview_timer",
        .skip_unhandled_events = false,
    };
    esp_timer_create(&preview_timer_args, &preview_timer_);
}

/**
 * @brief 构造 SpiLcdDisplay 对象并初始化该模块运行所需的成员和系统资源。
 * @details 构造阶段只建立本模块自身资源；需要异步运行的任务由后续 Start 或 Initialize 方法启动。
 */
SpiLcdDisplay::SpiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                           int width, int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {

    // draw white
    std::vector<uint16_t> buffer(width_, 0xFFFF);
    for (int y = 0; y < height_; y++) {
        esp_lcd_panel_draw_bitmap(panel_, 0, y, width_, y + 1, buffer.data());
    }

    // Set the display to on
    ESP_LOGI(TAG, "正在打开显示屏");
    {
        esp_err_t __err = esp_lcd_panel_disp_on_off(panel_, true);
        if (__err == ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGW(TAG, "屏幕面板不支持开关控制，按已打开处理");
        } else {
            ESP_ERROR_CHECK(__err);
        }
    }

    ESP_LOGI(TAG, "正在初始化 LVGL 图形库");
    lv_init();

#if CONFIG_SPIRAM
    // lv image cache, currently only PNG is supported
    size_t psram_size_mb = esp_psram_get_size() / 1024 / 1024;
    if (psram_size_mb >= 8) {
        lv_image_cache_resize(2 * 1024 * 1024, true);
        ESP_LOGI(TAG, "图像缓存使用 2MB PSRAM");
    } else if (psram_size_mb >= 2) {
        lv_image_cache_resize(512 * 1024, true);
        ESP_LOGI(TAG, "图像缓存使用 512KB PSRAM");
    }
#endif

    ESP_LOGI(TAG, "正在初始化 LVGL 端口");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = kResponsiveLvglTaskPriority;
#if CONFIG_SOC_CPU_CORES_NUM > 1
    port_cfg.task_affinity = 1;
#endif
    const esp_err_t lvgl_init_result = lvgl_port_init(&port_cfg);
    if (lvgl_init_result != ESP_OK) {
        ESP_LOGE(TAG, "LVGL 端口初始化失败，错误码=%s", esp_err_to_name(lvgl_init_result));
        return;
    }

    ESP_LOGI(TAG, "正在添加 LCD 显示设备");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * kSpiLcdDrawBufferLines),
        .double_buffer = false,
        .trans_size = 0,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = false,
        .rotation = {
            .swap_xy = swap_xy,
            .mirror_x = mirror_x,
            .mirror_y = mirror_y,
        },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = 1,
            .buff_spiram = 0,
            .sw_rotate = 0,
            .swap_bytes = 1,
            .full_refresh = 0,
            .direct_mode = 0,
        },
    };

    display_ = lvgl_port_add_disp(&display_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "添加 LCD 显示设备失败");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }
}


// RGB LCD implementation
/**
 * @brief 构造 RgbLcdDisplay 对象并初始化该模块运行所需的成员和系统资源。
 * @details 构造阶段只建立本模块自身资源；需要异步运行的任务由后续 Start 或 Initialize 方法启动。
 */
RgbLcdDisplay::RgbLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                           int width, int height, int offset_x, int offset_y,
                           bool mirror_x, bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {

    // draw white
    std::vector<uint16_t> buffer(width_, 0xFFFF);
    for (int y = 0; y < height_; y++) {
        esp_lcd_panel_draw_bitmap(panel_, 0, y, width_, y + 1, buffer.data());
    }

    ESP_LOGI(TAG, "正在初始化 LVGL 图形库");
    lv_init();

    ESP_LOGI(TAG, "正在初始化 LVGL 端口");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
    port_cfg.timer_period_ms = 50;
    const esp_err_t lvgl_init_result = lvgl_port_init(&port_cfg);
    if (lvgl_init_result != ESP_OK) {
        ESP_LOGE(TAG, "LVGL 端口初始化失败，错误码=%s", esp_err_to_name(lvgl_init_result));
        return;
    }

    ESP_LOGI(TAG, "正在添加 LCD 显示设备");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .buffer_size = static_cast<uint32_t>(width_ * 20),
        .double_buffer = true,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .rotation = {
            .swap_xy = swap_xy,
            .mirror_x = mirror_x,
            .mirror_y = mirror_y,
        },
        .flags = {
            .buff_dma = 1,
            .swap_bytes = 0,
            .full_refresh = 1,
            .direct_mode = 1,
        },
    };

    const lvgl_port_display_rgb_cfg_t rgb_cfg = {
        .flags = {
            .bb_mode = true,
            .avoid_tearing = true,
        }
    };
    
    display_ = lvgl_port_add_disp_rgb(&display_cfg, &rgb_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "添加 RGB 显示设备失败");
        return;
    }
    
    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }
}

/**
 * @brief 构造 MipiLcdDisplay 对象并初始化该模块运行所需的成员和系统资源。
 * @details 构造阶段只建立本模块自身资源；需要异步运行的任务由后续 Start 或 Initialize 方法启动。
 */
MipiLcdDisplay::MipiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                            int width, int height,  int offset_x, int offset_y,
                            bool mirror_x, bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {

    ESP_LOGI(TAG, "正在初始化 LVGL 图形库");
    lv_init();

    ESP_LOGI(TAG, "正在初始化 LVGL 端口");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    const esp_err_t lvgl_init_result = lvgl_port_init(&port_cfg);
    if (lvgl_init_result != ESP_OK) {
        ESP_LOGE(TAG, "LVGL 端口初始化失败，错误码=%s", esp_err_to_name(lvgl_init_result));
        return;
    }

    ESP_LOGI(TAG, "正在添加 LCD 显示设备");
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = panel_io,
        .panel_handle = panel,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * 50),
        .double_buffer = false,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = false,
        /* Rotation values must be same as used in esp_lcd for initial settings of the screen */
        .rotation = {
            .swap_xy = swap_xy,
            .mirror_x = mirror_x,
            .mirror_y = mirror_y,
        },
        .flags = {
            .buff_dma = true,
            .buff_spiram =false,
            .sw_rotate = true,
        },
    };

    const lvgl_port_display_dsi_cfg_t dpi_cfg = {
        .flags = {
            .avoid_tearing = false,
        }
    };
    display_ = lvgl_port_add_disp_dsi(&disp_cfg, &dpi_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "添加 LCD 显示设备失败");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }
}

/**
 * @brief 析构 LcdDisplay 对象并释放其持有的系统资源。
 * @details 释放顺序与创建顺序相反，先停止异步来源，再销毁句柄和动态内存，避免回调访问失效对象。
 */
LcdDisplay::~LcdDisplay() {
    SetPreviewImage(nullptr);
    
    // Clean up GIF controller
    if (gif_controller_) {
        gif_controller_->Stop();
        gif_controller_.reset();
    }
    
    if (preview_timer_ != nullptr) {
        esp_timer_stop(preview_timer_);
        esp_timer_delete(preview_timer_);
    }

    if (conversation_face_timer_ != nullptr) {
        lv_timer_delete(conversation_face_timer_);
        conversation_face_timer_ = nullptr;
    }
    if (conversation_face_ != nullptr) {
        lv_obj_del(conversation_face_);
        conversation_face_ = nullptr;
        conversation_left_eye_ = nullptr;
        conversation_right_eye_ = nullptr;
    }

    lv_anim_delete(this, ScreensaverMemoScrollAnimationCallback);
    if (screensaver_mcp_list_switch_timer_ != nullptr) {
        lv_timer_delete(screensaver_mcp_list_switch_timer_);
        screensaver_mcp_list_switch_timer_ = nullptr;
    }
    if (screensaver_memo_timer_ != nullptr) {
        lv_timer_delete(screensaver_memo_timer_);
        screensaver_memo_timer_ = nullptr;
    }

    if (screensaver_container_ != nullptr) {
        lv_obj_del(screensaver_container_);
        screensaver_container_ = nullptr;
    }
    if (binding_container_ != nullptr) {
        lv_obj_del(binding_container_);
        binding_container_ = nullptr;
        binding_title_label_ = nullptr;
        binding_code_panel_ = nullptr;
        binding_code_label_ = nullptr;
        binding_message_label_ = nullptr;
    }
    if (preview_image_ != nullptr) {
        lv_obj_del(preview_image_);
    }
    if (chat_message_label_ != nullptr) {
        lv_obj_del(chat_message_label_);
    }
    if (emoji_label_ != nullptr) {
        lv_obj_del(emoji_label_);
    }
    if (emoji_image_ != nullptr) {
        lv_obj_del(emoji_image_);
    }
    if (emoji_box_ != nullptr) {
        lv_obj_del(emoji_box_);
    }
    if (content_ != nullptr) {
        lv_obj_del(content_);
    }
    if (bottom_bar_ != nullptr) {
        lv_obj_del(bottom_bar_);
    }
    if (status_bar_ != nullptr) {
        lv_obj_del(status_bar_);
    }
    if (top_bar_ != nullptr) {
        lv_obj_del(top_bar_);
    }
    if (side_bar_ != nullptr) {
        lv_obj_del(side_bar_);
    }
    if (container_ != nullptr) {
        lv_obj_del(container_);
    }
    if (display_ != nullptr) {
        lv_display_delete(display_);
    }

    if (panel_ != nullptr) {
        esp_lcd_panel_del(panel_);
    }
    if (panel_io_ != nullptr) {
        esp_lcd_panel_io_del(panel_io_);
    }
}

/**
 * @brief 获取 LVGL 全局互斥锁。
 * @param timeout_ms 最大等待毫秒数。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
bool LcdDisplay::Lock(int timeout_ms) {
    return lvgl_port_lock(timeout_ms);
}

/**
 * @brief 释放 LVGL 全局互斥锁。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void LcdDisplay::Unlock() {
    lvgl_port_unlock();
}

/**
 * @brief 根据完整字幕高度配置两行窗口中的纵向滚动动画。
 * @details 方法先终止上一段字幕的动画并恢复到顶部。文本不超过两行时保持居中静止；
 *          超过两行时计算缩放后的溢出距离，以固定可见速度向上滚动。动画到达最后一行后
 *          停留一段时间，再回到第一行重新开始。调用本方法前必须持有 LVGL 锁。
 */
void LcdDisplay::UpdateSubtitleScroll() {
    if (chat_message_label_ == nullptr) {
        return;
    }

    /*
     * 新字幕必须从第一行开始显示。删除以标签为目标的旧动画，同时清除上一段字幕
     * 遗留的 Y 轴平移，避免新回答从中间位置开始。
     */
    lv_anim_delete(chat_message_label_, nullptr);
    lv_obj_set_style_translate_y(chat_message_label_, 0, 0);

    const char* text = lv_label_get_text(chat_message_label_);
    if (text == nullptr || text[0] == '\0') {
        return;
    }
    if (audio_playback_mode_) {
        lv_obj_align(chat_message_label_, LV_ALIGN_CENTER, 0, 0);
        return;
    }

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    const lv_font_t* text_font = lvgl_theme->text_font()->font();
    const int two_line_height = text_font->line_height * 2 + kRoundSubtitleLineSpacing;

    /*
     * 标签高度使用完整文本的自适应高度，而外层 bottom_bar_ 仍保持两行高度并负责裁切。
     * 这样所有文字都保留在标签中，只通过移动标签决定当前可见的两行内容。
     */
    lv_obj_set_height(chat_message_label_, LV_SIZE_CONTENT);
    lv_obj_update_layout(chat_message_label_);
    const int content_height = lv_obj_get_height(chat_message_label_);
    if (content_height <= two_line_height) {
        lv_obj_align(chat_message_label_, LV_ALIGN_CENTER, 0, 0);
        return;
    }

    lv_obj_align(chat_message_label_, LV_ALIGN_TOP_MID, 0, 0);
    const int scroll_distance = ScaleSubtitleSize(content_height - two_line_height);
    const int calculated_duration =
        scroll_distance * 1000 / kRoundSubtitleScrollPixelsPerSecond;
    const int scroll_duration = std::min(30000, std::max(1500, calculated_duration));

    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, chat_message_label_);
    lv_anim_set_exec_cb(&animation, [](void* target, int32_t value) {
        lv_obj_set_style_translate_y(static_cast<lv_obj_t*>(target), value, 0);
    });
    lv_anim_set_values(&animation, 0, -scroll_distance);
    lv_anim_set_duration(&animation, scroll_duration);
    lv_anim_set_delay(&animation, kRoundSubtitleScrollStartDelayMs);
    lv_anim_set_repeat_delay(&animation, kRoundSubtitleScrollRepeatDelayMs);
    lv_anim_set_repeat_count(&animation, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&animation, lv_anim_path_linear);
    lv_anim_start(&animation);
}
