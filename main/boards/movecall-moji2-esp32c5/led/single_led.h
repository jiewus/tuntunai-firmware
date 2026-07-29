#ifndef _SINGLE_LED_H_
#define _SINGLE_LED_H_

#include "led/led.h"
#include <driver/gpio.h>
#include <led_strip.h>
#include <esp_timer.h>
#include <atomic>
#include <mutex>

/**
 * @file single_led.h
 * @brief 单颗 WS2812 RGB 状态灯驱动。
 */

/**
 * @brief 将设备状态映射为颜色、常亮或闪烁模式。
 */
class SingleLed : public Led {
public:
    /**
     * @param gpio WS2812 单线数据 GPIO。
     */
    SingleLed(gpio_num_t gpio);
    virtual ~SingleLed();

    void OnStateChanged() override;

private:
    std::mutex mutex_;
    TaskHandle_t blink_task_ = nullptr;
    led_strip_handle_t led_strip_ = nullptr;
    uint8_t r_ = 0, g_ = 0, b_ = 0;
    int blink_counter_ = 0;
    int blink_interval_ms_ = 0;
    esp_timer_handle_t blink_timer_ = nullptr;

    /**
     * @brief 启动有限次数闪烁。
     * @param times 点亮次数。
     * @param interval_ms 明灭切换间隔。
     */
    void StartBlinkTask(int times, int interval_ms);
    /**
     * @brief 定时器回调，根据计数切换灯并停止已完成动画。
     */
    void OnBlinkTimer();

    void BlinkOnce();
    void Blink(int times, int interval_ms);
    void StartContinuousBlink(int interval_ms);
    void TurnOn();
    void TurnOff();
    /**
     * @brief 设置后续点亮使用的 RGB 颜色，每个分量范围 0-255。
     */
    void SetColor(uint8_t r, uint8_t g, uint8_t b);
};

#endif // _SINGLE_LED_H_
