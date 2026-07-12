#ifndef ADC_BATTERY_MONITOR_H
#define ADC_BATTERY_MONITOR_H

#include <functional>
#include <driver/gpio.h>
#include <adc_battery_estimation.h>
#include <esp_timer.h>

/**
 * @file adc_battery_monitor.h
 * @brief ADC 电池电压估算和充放电状态监视器。
 */

/**
 * @brief 周期采样电池分压，并将电压映射为 0-100% 电量。
 */
class AdcBatteryMonitor {
public:
    /**
     * @param adc_unit ADC 单元编号。
     * @param adc_channel 连接电池分压点的 ADC 通道。
     * @param upper_resistor 电池正极到 ADC 端的上拉电阻，单位欧姆。
     * @param lower_resistor ADC 端到地的下拉电阻，单位欧姆。
     * @param charging_pin 充电状态 GPIO；GPIO_NUM_NC 表示使用电压趋势推断。
     */
    AdcBatteryMonitor(adc_unit_t adc_unit, adc_channel_t adc_channel, float upper_resistor, float lower_resistor, gpio_num_t charging_pin = GPIO_NUM_NC);
    /**
     * @brief 停止周期定时器并释放 ADC 估算句柄。
     */
    ~AdcBatteryMonitor();

    /**
     * @return 当前是否处于充电状态。
     */
    bool IsCharging();
    /**
     * @return 当前是否处于放电状态。
     */
    bool IsDischarging();
    /**
     * @return 根据电池曲线估算的电量百分比 0-100。
     */
    uint8_t GetBatteryLevel();

    /**
     * @brief 注册充电状态翻转回调；参数 true 表示开始充电。
     */
    void OnChargingStatusChanged(std::function<void(bool)> callback);

private:
    gpio_num_t charging_pin_;
    adc_battery_estimation_handle_t adc_battery_estimation_handle_ = nullptr;
    esp_timer_handle_t timer_handle_ = nullptr;
    bool is_charging_ = false;
    std::function<void(bool)> on_charging_status_changed_;

    /**
     * @brief 定时读取充电 GPIO/电压并在状态变化时通知回调。
     */
    void CheckBatteryStatus();
};

#endif // ADC_BATTERY_MONITOR_H
