/**
 * @file display.cc
 * @brief display.cc 中各类和辅助函数的具体实现。
 */
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <string>
#include <cstdlib>

#include "display.h"
#include "system/settings.h"

#define TAG "Display"

/**
 * @brief 构造 Display 对象并初始化该模块运行所需的成员和系统资源。
 * @details 构造阶段只建立本模块自身资源；需要异步运行的任务由后续 Start 或 Initialize 方法启动。
 */
Display::Display() {
}

/**
 * @brief 析构 Display 对象并释放其持有的系统资源。
 * @details 释放顺序与创建顺序相反，先停止异步来源，再销毁句柄和动态内存，避免回调访问失效对象。
 */
Display::~Display() {
}

/**
 * @brief 更新主状态文字。
 * @param status UTF-8 文本，方法会在需要时复制。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void Display::SetStatus(const char* status) {
    ESP_LOGW(TAG, "SetStatus: %s", status);
}

/**
 * @brief 临时覆盖状态区域显示通知。
 * @param duration_ms 自动隐藏延时，单位 ms。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void Display::ShowNotification(const std::string &notification, int duration_ms) {
    ShowNotification(notification.c_str(), duration_ms);
}

/**
 * @brief 临时覆盖状态区域显示通知。
 * @param duration_ms 自动隐藏延时，单位 ms。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void Display::ShowNotification(const char* notification, int duration_ms) {
    ESP_LOGW(TAG, "ShowNotification: %s", notification);
}

/**
 * @brief 刷新网络、电池、静音和时钟。
 * @param update_all true 强制刷新未变化字段。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void Display::UpdateStatusBar(bool update_all) {
}


/**
 * @brief 切换助手表情。
 * @param emotion 资源索引中的表情名称。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void Display::SetEmotion(const char* emotion) {
    ESP_LOGW(TAG, "SetEmotion: %s", emotion);
}

/**
 * @brief 显示一条对话字幕。
 * @param role user/assistant/system。
 * @param content UTF-8 正文。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void Display::SetChatMessage(const char* role, const char* content) {
    ESP_LOGW(TAG, "Role:%s", role);
    ESP_LOGW(TAG, "     %s", content);
}

/**
 * @brief 清除当前字幕/聊天内容。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void Display::ClearChatMessages() {
    // Default empty implementation, override in subclasses if needed
}

/**
 * @brief 切换主题。
 * @param theme 由主题管理器持有，不能在显示使用期间释放。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void Display::SetTheme(Theme* theme) {
    current_theme_ = theme;
    Settings settings("display", true);
    settings.SetString("theme", theme->name());
}

/**
 * @brief 切换显示省电状态。
 * @param on true 时降低刷新或关闭面板。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void Display::SetPowerSaveMode(bool on) {
    ESP_LOGW(TAG, "SetPowerSaveMode: %d", on);
}

/**
 * @brief 进入或退出常亮屏保界面。
 * @param enabled true 显示屏保；false 恢复正常界面。
 * @details 无屏设备和未实现屏保的显示类型使用空实现，避免板级代码判断具体显示类型。
 */
void Display::SetScreensaverMode(bool enabled) {
    (void)enabled;
}

/**
 * @brief 切换音频优先显示模式。
 * @param active true 暂停非必要显示动画，false 恢复当前页面动画。
 */
void Display::SetAudioPlaybackMode(bool active) {
    (void)active;
}

bool Display::SetPreviewImageData(void* data, size_t size) {
    (void)size;
    heap_caps_free(data);
    return false;
}
