#pragma once

#include "backlight.h"

#include <driver/gpio.h>
#include <esp_timer.h>

/**
 * @brief Moji2 使用的 LEDC PWM 背光实现。
 */
class PwmBacklight : public Backlight {
public:
    PwmBacklight(gpio_num_t pin, bool output_invert = false, uint32_t freq_hz = 25000);
    ~PwmBacklight() override;

    void RestoreBrightness() override;
    void SetBrightness(uint8_t brightness, bool permanent = false) override;
    uint8_t brightness() const override { return brightness_; }

private:
    void OnTransitionTimer();
    void SetBrightnessImpl(uint8_t brightness);

    esp_timer_handle_t transition_timer_ = nullptr;
    uint8_t brightness_ = 0;
    uint8_t target_brightness_ = 0;
    int8_t step_ = 1;
};
