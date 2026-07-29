#pragma once

#include <lvgl.h>


/**
 * @file lvgl_font.h
 * @brief 内置字体和资源分区 cbin 字体的统一封装。
 */

/**
 * @brief 提供只读 LVGL 字体描述符。
 */
class LvglFont {
public:
    virtual const lv_font_t* font() const = 0;
    virtual ~LvglFont() = default;
};

/**
 * @brief 包装编译进固件的静态字体，不负责释放。
 */
class LvglBuiltInFont : public LvglFont {
public:
    LvglBuiltInFont(const lv_font_t* font) : font_(font) {}
    virtual const lv_font_t* font() const override { return font_; }

private:
    const lv_font_t* font_;
};


/**
 * @brief 从 mmap 资源数据创建 cbin 字体，析构时销毁动态描述符。
 */
class LvglCBinFont : public LvglFont {
public:
    /**
     * @param data 指向 cbin 字体文件在资源分区中的首地址。
     */
    LvglCBinFont(void* data);
    virtual ~LvglCBinFont();
    virtual const lv_font_t* font() const override { return font_; }

private:
    lv_font_t* font_;
};
