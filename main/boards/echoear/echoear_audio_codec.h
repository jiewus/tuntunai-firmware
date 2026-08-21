#ifndef ECHOEAR_AUDIO_CODEC_H
#define ECHOEAR_AUDIO_CODEC_H

#include "audio_codec.h"

#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <esp_codec_dev.h>
#include <esp_codec_dev_defaults.h>
#include <mutex>

/**
 * @brief EchoEar 的 ES8311 输出与 ES7210 四通道输入音频实现。
 */
class EchoEarAudioCodec : public AudioCodec {
public:
    EchoEarAudioCodec(void* i2c_master_handle, i2c_port_t i2c_port,
                      int input_sample_rate, int output_sample_rate,
                      gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws,
                      gpio_num_t dout, gpio_num_t din, gpio_num_t pa_pin,
                      uint8_t es8311_addr, uint8_t es7210_addr,
                      bool input_reference);
    ~EchoEarAudioCodec() override;

    void SetOutputVolume(int volume) override;
    void EnableInput(bool enable) override;
    void EnableOutput(bool enable) override;

private:
    const audio_codec_data_if_t* data_if_ = nullptr;
    const audio_codec_ctrl_if_t* output_ctrl_if_ = nullptr;
    const audio_codec_if_t* output_codec_if_ = nullptr;
    const audio_codec_ctrl_if_t* input_ctrl_if_ = nullptr;
    const audio_codec_if_t* input_codec_if_ = nullptr;
    const audio_codec_gpio_if_t* gpio_if_ = nullptr;
    esp_codec_dev_handle_t output_device_ = nullptr;
    esp_codec_dev_handle_t input_device_ = nullptr;
    std::mutex mutex_;

    void CreateDuplexChannels(gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws,
                              gpio_num_t dout, gpio_num_t din);
    int Read(int16_t* dest, int samples) override;
    int Write(const int16_t* data, int samples) override;
};

#endif
