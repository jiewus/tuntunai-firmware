#pragma once

#include <cstdint>

/**
 * @brief 屏幕背光的硬件无关控制接口。
 */
class Backlight {
public:
    virtual ~Backlight() = default;
    virtual void RestoreBrightness() = 0;
    virtual void SetBrightness(uint8_t brightness, bool permanent = false) = 0;
    virtual uint8_t brightness() const = 0;
};
