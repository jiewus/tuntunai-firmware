#pragma once

#include "../lvgl_image.h"
#include "gifdec.h"
#include <lvgl.h>
#include <memory>
#include <functional>

/**
 * @file lvgl_gif.h
 * @brief gifdec 与 LVGL 图片描述符之间的 C++ 动画控制器。
 */
/**
 * @brief 解码内存 GIF，并通过 LVGL 定时器逐帧更新 RGB 像素缓冲。
 */
class LvglGif {
public:
    /**
     * @param img_dsc data 指向完整 GIF 文件字节；不取得原始文件所有权。
     */
    explicit LvglGif(const lv_img_dsc_t* img_dsc);
    virtual ~LvglGif();

    /**
     * @return 当前解码帧对应的 LVGL 图片描述符。
     */
    virtual const lv_img_dsc_t* image_dsc() const;

    /**
     *
     * @brief 从第一帧开始或重新开始播放动画。
     */
    void Start();

    /**
     *
     * @brief 暂停在当前帧，保留解码状态。
     */
    void Pause();

    /**
     *
     * @brief 从暂停位置继续播放。
     */
    void Resume();

    /**
     *
     * @brief 停止动画并把解码器回绕到第一帧。
     */
    void Stop();

    /**
     *
     * @brief 查询定时器是否处于播放状态。
     */
    bool IsPlaying() const;

    /**
     *
     * @brief 查询 GIF 头、缓冲分配和首帧初始化是否成功。
     */
    bool IsLoaded() const;

    /**
     *
     * @brief 获取循环次数；0 通常表示无限循环。
     */
    int32_t GetLoopCount() const;

    /**
     *
     * @brief 设置剩余循环次数。
     * @param count 0 表示无限循环。
     */
    void SetLoopCount(int32_t count);

    /**
     *
     * @brief 获取每轮播放完成后的额外等待时间，单位 ms。
     */
    uint32_t GetLoopDelay() const;

    /**
     * @brief 设置两轮动画之间的等待时间。
     * @param delay_ms 毫秒数，0 表示无额外延迟。
     */
    void SetLoopDelay(uint32_t delay_ms);

    /**
     *
     * @brief 获取 GIF 逻辑宽度和高度，单位像素。
     */
    uint16_t width() const;
    uint16_t height() const;

    /**
     *
     * @brief 设置帧刷新回调，通常用于通知 LVGL image invalidate。
     */
    void SetFrameCallback(std::function<void()> callback);

private:
    // gifdec 解码器实例，内部保存帧位置、调色板和循环信息。
    gd_GIF* gif_;
    
    // 指向当前 RGB 帧缓冲的 LVGL 图片描述符。
    lv_img_dsc_t img_dsc_;
    
    // Animation timer
    lv_timer_t* timer_;
    
    // Last frame update time
    uint32_t last_call_;
    
    // Animation state
    bool playing_;
    bool loaded_;
    
    // Loop delay configuration
    uint32_t loop_delay_ms_;      // Delay between loops in milliseconds
    bool loop_waiting_;           // Whether we're waiting for the next loop
    uint32_t loop_wait_start_;    // Timestamp when loop wait started
    
    // Frame update callback
    std::function<void()> frame_callback_;
    
    /**
     *
     * @brief 根据帧延时推进动画，并处理循环结束等待。
     */
    void NextFrame();
    
    /**
     *
     * @brief 删除 LVGL 定时器、关闭解码器并释放像素缓冲。
     */
    void Cleanup();
};
