#ifndef _LED_H_
#define _LED_H_

/**
 * @file led.h
 * @brief 设备状态 LED 的最小抽象。
 */

/**
 * @brief 根据 Application 当前状态刷新指示灯。
 */
class Led {
public:
    virtual ~Led() = default;
    /**
     * @brief 读取全局设备状态并立即更新 LED 动画。
     */
    virtual void OnStateChanged() = 0;
};


/**
 * @brief 无 LED 板型使用的空对象，避免上层判空。
 */
class NoLed : public Led {
public:
    virtual void OnStateChanged() override {}
};

#endif // _LED_H_
