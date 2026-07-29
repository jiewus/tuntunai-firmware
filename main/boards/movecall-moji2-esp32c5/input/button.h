#ifndef BUTTON_H_
#define BUTTON_H_

#include <driver/gpio.h>
#include <iot_button.h>
#include <button_types.h>
#include <button_adc.h>
#include <button_gpio.h>
#include <functional>

/**
 * @file button.h
 * @brief GPIO/ADC 按键事件的 C++ 回调封装。
 */

/**
 * @brief 管理一个 iot_button 句柄并提供常用按键手势回调。
 */
class Button {
public:
    /**
     * @brief 接管已创建的按键句柄。
     * @param button_handle 有效句柄，析构时会删除。
     */
    Button(button_handle_t button_handle);
    /**
     * @brief 从 GPIO 创建按键。
     * @param gpio_num 输入 GPIO。
     * @param active_high true 表示高电平按下，false 表示低电平按下并启用上拉。
     * @param long_press_time 长按判定时间 ms，0 使用组件默认值。
     * @param short_press_time 短按判定时间 ms，0 使用组件默认值。
     * @param enable_power_save 是否允许按键组件使用低功耗唤醒功能。
     */
    Button(gpio_num_t gpio_num, bool active_high = false, uint16_t long_press_time = 0, uint16_t short_press_time = 0, bool enable_power_save = false);
    /**
     * @brief 注销事件并删除底层按键句柄。
     */
    ~Button();

    /**
     * @brief 设置物理按下瞬间回调。
     */
    void OnPressDown(std::function<void()> callback);
    /**
     * @brief 设置物理释放瞬间回调。
     */
    void OnPressUp(std::function<void()> callback);
    /**
     * @brief 设置达到长按阈值后的回调。
     */
    void OnLongPress(std::function<void()> callback);
    /**
     * @brief 设置一次短按完成回调。
     */
    void OnClick(std::function<void()> callback);
    /**
     * @brief 设置双击回调。
     */
    void OnDoubleClick(std::function<void()> callback);
    /**
     * @brief 设置指定次数连击回调。
     * @param click_count 目标点击次数，默认三击。
     */
    void OnMultipleClick(std::function<void()> callback, uint8_t click_count = 3);

protected:
    gpio_num_t gpio_num_;
    button_handle_t button_handle_ = nullptr;

    std::function<void()> on_press_down_;
    std::function<void()> on_press_up_;
    std::function<void()> on_long_press_;
    std::function<void()> on_click_;
    std::function<void()> on_double_click_;
    std::function<void()> on_multiple_click_;
};

#if CONFIG_SOC_ADC_SUPPORTED
class AdcButton : public Button {
public:
    /**
     * @brief 创建电阻分压式 ADC 按键。
     * @param adc_config ADC 单元、通道及电压范围。
     */
    AdcButton(const button_adc_config_t& adc_config);
};
#endif

class PowerSaveButton : public Button {
public:
    /**
     * @brief 创建低电平有效且支持省电唤醒的 GPIO 按键。
     */
    PowerSaveButton(gpio_num_t gpio_num) : Button(gpio_num, false, 0, 0, true) {
    }
};

#endif // BUTTON_H_
