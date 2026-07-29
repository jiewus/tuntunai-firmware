#ifndef _ES8311_AUDIO_CODEC_H
#define _ES8311_AUDIO_CODEC_H

#include "audio_codec.h"

#include <driver/i2c_master.h>
#include <driver/gpio.h>
#include <esp_codec_dev.h>
#include <esp_codec_dev_defaults.h>
#include <mutex>


/**
 * @file es8311_audio_codec.h
 * @brief ES8311 Codec 与 ESP-IDF I2S 标准驱动实现。
 */

/**
 * @brief Tuntun Moji2 使用的 ES8311 全双工音频驱动。
 */
class Es8311AudioCodec : public AudioCodec {
private:
    const audio_codec_data_if_t* data_if_ = nullptr;
    const audio_codec_ctrl_if_t* ctrl_if_ = nullptr;
    const audio_codec_if_t* codec_if_ = nullptr;
    const audio_codec_gpio_if_t* gpio_if_ = nullptr;

    esp_codec_dev_handle_t dev_ = nullptr;
    gpio_num_t pa_pin_ = GPIO_NUM_NC;
    bool pa_inverted_ = false;
    std::mutex data_if_mutex_;

    /**
     * @brief 创建共享时钟的 I2S TX/RX 通道并配置五根音频引脚。
     */
    void CreateDuplexChannels(gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din);
    /**
     * @brief 根据 input_enabled_/output_enabled_ 同步 Codec、I2S 与功放状态。
     */
    void UpdateDeviceState();

    virtual int Read(int16_t* dest, int samples) override;
    virtual int Write(const int16_t* data, int samples) override;

public:
    /**
     * @brief 创建并初始化 ES8311 音频设备。
     * @param i2c_master_handle 已创建的 i2c_master_bus_handle_t，以 void* 传入兼容公共接口。
     * @param i2c_port I2C 控制器编号。
     * @param input_sample_rate 麦克风采样率 Hz。
     * @param output_sample_rate 扬声器采样率 Hz。
     * @param mclk,bclk,ws,dout,din I2S 主时钟、位时钟、帧同步、输出和输入 GPIO。
     * @param pa_pin 外部功放使能 GPIO，GPIO_NUM_NC 表示没有独立功放控制。
     * @param es8311_addr ES8311 的 7 位 I2C 地址。
     * @param use_mclk 是否向 Codec 输出 MCLK。
     * @param pa_inverted 功放使能电平是否反相。
     */
    Es8311AudioCodec(void* i2c_master_handle, i2c_port_t i2c_port, int input_sample_rate, int output_sample_rate,
        gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din,
        gpio_num_t pa_pin, uint8_t es8311_addr, bool use_mclk = true, bool pa_inverted = false);
    virtual ~Es8311AudioCodec();

    /**
     * @brief 将 0-100 音量换算后写入 ES8311。
     */
    virtual void SetOutputVolume(int volume) override;
    virtual void EnableInput(bool enable) override;
    virtual void EnableOutput(bool enable) override;
};

#endif // _ES8311_AUDIO_CODEC_H
