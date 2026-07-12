/**
 * @file lvgl_gif.cc
 * @brief lvgl_gif.cc 中各类和辅助函数的具体实现。
 */
#include "lvgl_gif.h"
#include <esp_log.h>
#include <cstring>

#define TAG "LvglGif"

/**
 * @brief 构造 LvglGif 对象并初始化该模块运行所需的成员和系统资源。
 * @details 构造阶段只建立本模块自身资源；需要异步运行的任务由后续 Start 或 Initialize 方法启动。
 */
LvglGif::LvglGif(const lv_img_dsc_t* img_dsc)
    : gif_(nullptr), timer_(nullptr), last_call_(0), playing_(false), loaded_(false),
      loop_delay_ms_(0), loop_waiting_(false), loop_wait_start_(0) {
    if (!img_dsc || !img_dsc->data) {
        ESP_LOGE(TAG, "Invalid image descriptor");
        return;
    }

    gif_ = gd_open_gif_data(img_dsc->data);
    if (!gif_) {
        ESP_LOGE(TAG, "Failed to open GIF from image descriptor");
        return;
    }

    // Setup LVGL image descriptor
    memset(&img_dsc_, 0, sizeof(img_dsc_));
    img_dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
    img_dsc_.header.flags = LV_IMAGE_FLAGS_MODIFIABLE;
    img_dsc_.header.cf = LV_COLOR_FORMAT_ARGB8888;
    img_dsc_.header.w = gif_->width;
    img_dsc_.header.h = gif_->height;
    img_dsc_.header.stride = gif_->width * 4;
    img_dsc_.data = gif_->canvas;
    img_dsc_.data_size = gif_->width * gif_->height * 4;

    // Render first frame
    if (gif_->canvas) {
        gd_render_frame(gif_, gif_->canvas);
    }

    loaded_ = true;
    ESP_LOGD(TAG, "GIF loaded from image descriptor: %dx%d", gif_->width, gif_->height);
}

// Destructor
/**
 * @brief 析构 LvglGif 对象并释放其持有的系统资源。
 * @details 释放顺序与创建顺序相反，先停止异步来源，再销毁句柄和动态内存，避免回调访问失效对象。
 */
LvglGif::~LvglGif() {
    Cleanup();
}

// LvglImage interface implementation
/**
 * @return 当前解码帧对应的 LVGL 图片描述符。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
const lv_img_dsc_t* LvglGif::image_dsc() const {
    if (!loaded_) {
        return nullptr;
    }
    return &img_dsc_;
}

// Animation control methods
/**
 *
 * @brief 从第一帧开始或重新开始播放动画。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void LvglGif::Start() {
    if (!loaded_ || !gif_) {
        ESP_LOGW(TAG, "GIF not loaded, cannot start");
        return;
    }

    if (!timer_) {
        timer_ = lv_timer_create([](lv_timer_t* timer) {
            LvglGif* gif_obj = static_cast<LvglGif*>(lv_timer_get_user_data(timer));
            gif_obj->NextFrame();
        }, 10, this);
    }

    if (timer_) {
        playing_ = true;
        loop_waiting_ = false;  // Reset loop waiting state
        last_call_ = lv_tick_get();
        lv_timer_resume(timer_);
        lv_timer_reset(timer_);
        
        // Render first frame
        NextFrame();
        
        ESP_LOGD(TAG, "GIF animation started");
    }
}

/**
 *
 * @brief 暂停在当前帧，保留解码状态。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void LvglGif::Pause() {
    if (timer_) {
        playing_ = false;
        lv_timer_pause(timer_);
        ESP_LOGD(TAG, "GIF animation paused");
    }
}

/**
 *
 * @brief 从暂停位置继续播放。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void LvglGif::Resume() {
    if (!loaded_ || !gif_) {
        ESP_LOGW(TAG, "GIF not loaded, cannot resume");
        return;
    }

    if (timer_) {
        playing_ = true;
        lv_timer_resume(timer_);
        ESP_LOGD(TAG, "GIF animation resumed");
    }
}

/**
 *
 * @brief 停止动画并把解码器回绕到第一帧。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void LvglGif::Stop() {
    if (timer_) {
        playing_ = false;
        lv_timer_pause(timer_);
    }

    // Reset loop waiting state
    loop_waiting_ = false;

    if (gif_) {
        gd_rewind(gif_);
        // Render first frame without advancing
        if (gif_->canvas) {
            gd_render_frame(gif_, gif_->canvas);
        }
        ESP_LOGD(TAG, "GIF animation stopped and rewound");
    }
}

/**
 *
 * @brief 查询定时器是否处于播放状态。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
bool LvglGif::IsPlaying() const {
    return playing_;
}

/**
 *
 * @brief 查询 GIF 头、缓冲分配和首帧初始化是否成功。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
bool LvglGif::IsLoaded() const {
    return loaded_;
}

/**
 *
 * @brief 获取循环次数；0 通常表示无限循环。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
int32_t LvglGif::GetLoopCount() const {
    if (!loaded_ || !gif_) {
        return -1;
    }
    return gif_->loop_count;
}

/**
 *
 * @brief 设置剩余循环次数。
 * @param count 0 表示无限循环。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void LvglGif::SetLoopCount(int32_t count) {
    if (!loaded_ || !gif_) {
        ESP_LOGW(TAG, "GIF not loaded, cannot set loop count");
        return;
    }
    gif_->loop_count = count;
}

/**
 *
 * @brief 获取每轮播放完成后的额外等待时间，单位 ms。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
uint32_t LvglGif::GetLoopDelay() const {
    return loop_delay_ms_;
}

/**
 * @brief 设置两轮动画之间的等待时间。
 * @param delay_ms 毫秒数，0 表示无额外延迟。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void LvglGif::SetLoopDelay(uint32_t delay_ms) {
    loop_delay_ms_ = delay_ms;
    ESP_LOGD(TAG, "Loop delay set to %lu ms", delay_ms);
}

/**
 *
 * @brief 获取 GIF 逻辑宽度和高度，单位像素。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
uint16_t LvglGif::width() const {
    if (!loaded_ || !gif_) {
        return 0;
    }
    return gif_->width;
}

/**
 * @brief 执行 height 对应的模块内部流程。
 * @details 实现会维护 LvglGif 的内部一致性；发生错误时记录日志，并避免向后续流程传播无效资源。
 */
uint16_t LvglGif::height() const {
    if (!loaded_ || !gif_) {
        return 0;
    }
    return gif_->height;
}

/**
 *
 * @brief 设置帧刷新回调，通常用于通知 LVGL image invalidate。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void LvglGif::SetFrameCallback(std::function<void()> callback) {
    frame_callback_ = callback;
}

/**
 *
 * @brief 根据帧延时推进动画，并处理循环结束等待。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void LvglGif::NextFrame() {
    if (!loaded_ || !gif_ || !playing_) {
        return;
    }

    // Check if we're in loop wait state (only for infinite loop GIFs with delay)
    if (loop_waiting_) {
        uint32_t wait_elapsed = lv_tick_elaps(loop_wait_start_);
        if (wait_elapsed < loop_delay_ms_) {
            // Still waiting for loop delay
            return;
        }
        // Loop delay completed, continue playing
        loop_waiting_ = false;
        ESP_LOGD(TAG, "Loop delay completed, continuing GIF");
    }

    // Check if enough time has passed for the next frame
    uint32_t elapsed = lv_tick_elaps(last_call_);
    if (elapsed < gif_->gce.delay * 10) {
        return;
    }

    last_call_ = lv_tick_get();

    // Save file position before getting next frame to detect loop
    uint32_t pos_before = gif_->f_rw_p;

    // Get next frame
    int has_next = gd_get_frame(gif_);
    if (has_next == 0) {
        // Animation truly finished (non-infinite loop)
        playing_ = false;
        if (timer_) {
            lv_timer_pause(timer_);
        }
        ESP_LOGD(TAG, "GIF animation completed");
        return;
    }

    // Detect loop by checking if file position jumped back (rewound to start)
    // This works for looping GIFs regardless of when loop_count is set
    if (loop_delay_ms_ > 0 && gif_->f_rw_p < pos_before) {
        // File position decreased, meaning GIF looped back to beginning
        // Start waiting before rendering this frame
        loop_waiting_ = true;
        loop_wait_start_ = lv_tick_get();
        ESP_LOGD(TAG, "GIF completed one cycle, waiting %lu ms before next loop", loop_delay_ms_);
        return;
    }

    // Render current frame
    if (gif_->canvas) {
        gd_render_frame(gif_, gif_->canvas);
        
        // Call frame callback if set
        if (frame_callback_) {
            frame_callback_();
        }
    }
}

/**
 *
 * @brief 删除 LVGL 定时器、关闭解码器并释放像素缓冲。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void LvglGif::Cleanup() {
    // Stop and delete timer
    if (timer_) {
        lv_timer_delete(timer_);
        timer_ = nullptr;
    }

    // Close GIF decoder
    if (gif_) {
        gd_close_gif(gif_);
        gif_ = nullptr;
    }

    playing_ = false;
    loaded_ = false;
    
    // Clear image descriptor
    memset(&img_dsc_, 0, sizeof(img_dsc_));
}
