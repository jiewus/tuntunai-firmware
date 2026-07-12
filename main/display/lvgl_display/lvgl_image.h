#pragma once

#include <lvgl.h>


/**
 * @file lvgl_image.h
 * @brief 不同来源 LVGL 图片描述符的生命周期封装。
 */

/**
 * @brief 统一提供 lv_img_dsc_t，并声明图片是否为 GIF。
 */
class LvglImage {
public:
    virtual const lv_img_dsc_t* image_dsc() const = 0;
    virtual bool IsGif() const { return false; }
    virtual ~LvglImage() = default;
};


/**
 * @brief 包装包含完整图片文件字节的外部内存，不取得 data 所有权。
 */
class LvglRawImage : public LvglImage {
public:
    /**
     * @param data 图片文件首地址。
     * @param size 文件字节数。
     */
    LvglRawImage(void* data, size_t size);
    virtual const lv_img_dsc_t* image_dsc() const override { return &image_dsc_; }
    virtual bool IsGif() const;

private:
    lv_img_dsc_t image_dsc_;
};

/**
 * @brief 从 xiaozhi-fonts 的 cbin 内存格式创建 LVGL 图片描述符。
 */
class LvglCBinImage : public LvglImage {
public:
    LvglCBinImage(void* data);
    virtual ~LvglCBinImage();
    virtual const lv_img_dsc_t* image_dsc() const override { return image_dsc_; }

private:
    lv_img_dsc_t* image_dsc_ = nullptr;
};

/**
 * @brief 包装编译期静态 lv_img_dsc_t，不负责释放。
 */
class LvglSourceImage : public LvglImage {
public:
    LvglSourceImage(const lv_img_dsc_t* image_dsc) : image_dsc_(image_dsc) {}
    virtual const lv_img_dsc_t* image_dsc() const override { return image_dsc_; }

private:
    const lv_img_dsc_t* image_dsc_;
};

/**
 * @brief 拥有动态分配像素内存的图片，析构时释放 data。
 */
class LvglAllocatedImage : public LvglImage {
public:
    /**
     * @brief 从可探测格式的内存构造。
     */
    LvglAllocatedImage(void* data, size_t size);
    /**
     * @brief 从原始像素构造。
     * @param stride 每行字节数。
     * @param color_format lv_color_format_t 数值。
     */
    LvglAllocatedImage(void* data, size_t size, int width, int height, int stride, int color_format);
    virtual ~LvglAllocatedImage();
    virtual const lv_img_dsc_t* image_dsc() const override { return &image_dsc_; }

private:
    lv_img_dsc_t image_dsc_;
};
