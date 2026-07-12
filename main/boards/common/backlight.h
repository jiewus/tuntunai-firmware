#pragma once

#include <cstdint>
#include <functional>

#include <driver/gpio.h>
#include <esp_timer.h>


/**
 * @file backlight.h
 * @brief 屏幕背光亮度渐变与持久化控制。
 */

/**
 * @brief 将 0-100% 逻辑亮度平滑过渡到具体硬件实现。
 */
class Backlight {
public:
    Backlight();
    ~Backlight();

    /**
     * @brief 从 NVS 恢复用户上次永久保存的亮度。
     */
    void RestoreBrightness();
    /**
     * @brief 设置目标亮度。
     * @param brightness 0-100。
     * @param permanent 是否同步保存到 NVS。
     */
    void SetBrightness(uint8_t brightness, bool permanent = false);
    inline uint8_t brightness() const { return brightness_; }

protected:
    /**
     * @brief 定时向目标值递增/递减一步，实现无闪烁渐变。
     */
    void OnTransitionTimer();
    /**
     * @brief 将逻辑百分比写入硬件。
     */
    virtual void SetBrightnessImpl(uint8_t brightness) = 0;

    esp_timer_handle_t transition_timer_ = nullptr;
    uint8_t brightness_ = 0;
    uint8_t target_brightness_ = 0;
    uint8_t step_ = 1;
};


class PwmBacklight : public Backlight {
public:
    /**
     * @param pin LEDC PWM 输出 GPIO。
     * @param output_invert true 表示占空比反相，适用于低电平点亮电路。
     * @param freq_hz PWM 频率，默认 25 kHz 以避免可闻噪声和视觉闪烁。
     */
    PwmBacklight(gpio_num_t pin, bool output_invert = false, uint32_t freq_hz = 25000);
    ~PwmBacklight();

    void SetBrightnessImpl(uint8_t brightness) override;
};
