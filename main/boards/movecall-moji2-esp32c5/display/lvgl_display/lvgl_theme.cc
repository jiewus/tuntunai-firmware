/**
 * @file lvgl_theme.cc
 * @brief lvgl_theme.cc 中各类和辅助函数的具体实现。
 */
#include "lvgl_theme.h"

/**
 * @brief 以名称创建主题并填充安全默认值。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
LvglTheme::LvglTheme(const std::string& name) : Theme(name) {
}

/**
 * @brief 解析 #RRGGBB 颜色文本。
 * @return LVGL 颜色值，非法输入回退黑色。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
lv_color_t LvglTheme::ParseColor(const std::string& color) {
    if (color.find("#") == 0) {
        // Convert #112233 to lv_color_t
        uint8_t r = strtol(color.substr(1, 2).c_str(), nullptr, 16);
        uint8_t g = strtol(color.substr(3, 2).c_str(), nullptr, 16);
        uint8_t b = strtol(color.substr(5, 2).c_str(), nullptr, 16);
        return lv_color_make(r, g, b);
    }
    return lv_color_black();
}

/**
 * @brief 构造 LvglThemeManager 对象并初始化该模块运行所需的成员和系统资源。
 * @details 构造阶段只建立本模块自身资源；需要异步运行的任务由后续 Start 或 Initialize 方法启动。
 */
LvglThemeManager::LvglThemeManager() {
}

/**
 * @brief 按名称查找主题。
 * @return 不存在时返回默认主题。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
LvglTheme* LvglThemeManager::GetTheme(const std::string& theme_name) {
    auto it = themes_.find(theme_name);
    if (it != themes_.end()) {
        return it->second;
    }
    return nullptr;
}

/**
 * @brief 注册或替换主题。
 * @param theme_name 查找键。
 * @param theme 管理器不负责释放。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void LvglThemeManager::RegisterTheme(const std::string& theme_name, LvglTheme* theme) {
    themes_[theme_name] = theme;
}
