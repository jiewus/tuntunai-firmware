/**
 * @file lvgl_font.cc
 * @brief lvgl_font.cc 中各类和辅助函数的具体实现。
 */
#include "lvgl_font.h"
#include <cbin_font.h>


/**
 * @brief 构造 LvglCBinFont 对象并初始化该模块运行所需的成员和系统资源。
 * @details 构造阶段只建立本模块自身资源；需要异步运行的任务由后续 Start 或 Initialize 方法启动。
 */
LvglCBinFont::LvglCBinFont(void* data) {
    font_ = cbin_font_create(static_cast<uint8_t*>(data));
}

/**
 * @brief 析构 LvglCBinFont 对象并释放其持有的系统资源。
 * @details 释放顺序与创建顺序相反，先停止异步来源，再销毁句柄和动态内存，避免回调访问失效对象。
 */
LvglCBinFont::~LvglCBinFont() {
    if (font_ != nullptr) {
        cbin_font_delete(font_);
    }
}
