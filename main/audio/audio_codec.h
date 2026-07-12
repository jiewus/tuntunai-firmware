#ifndef _AUDIO_CODEC_H
#define _AUDIO_CODEC_H

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <driver/i2s_std.h>

#include <vector>
#include <string>
#include <functional>

#include "board.h"

#define AUDIO_CODEC_DMA_DESC_NUM 6
#define AUDIO_CODEC_DMA_FRAME_NUM 240

/**
 * @file audio_codec.h
 * @brief 音频 Codec 与 I2S 数据通道的统一抽象。
 */

/**
 * @brief 麦克风输入和扬声器输出的基类。
 *
 * 子类负责具体 Codec 芯片和 I2S 驱动；本类统一保存采样率、声道数、音量、
 * 增益及输入输出电源状态，并提供 vector 形式的数据接口。
 */
class AudioCodec {
public:
    /**
     * @brief 初始化音频状态变量，不会立即开启 I2S。
     */
    AudioCodec();
    /**
     * @brief 关闭仍在运行的 I2S 通道。
     */
    virtual ~AudioCodec();

    /**
     * @brief 设置扬声器音量。
     * @param volume 百分比，合法范围 0-100。
     */
    virtual void SetOutputVolume(int volume);
    /**
     * @brief 设置麦克风模拟/数字增益。
     * @param gain 增益值，单位由具体 Codec 驱动定义。
     */
    virtual void SetInputGain(float gain);
    /**
     * @brief 打开或关闭麦克风及接收 I2S 通道。
     */
    virtual void EnableInput(bool enable);
    /**
     * @brief 打开或关闭功放、Codec DAC 及发送 I2S 通道。
     */
    virtual void EnableOutput(bool enable);

    /**
     * @brief 将 PCM 样本写入扬声器。
     * @param data 有符号 16 位交错 PCM。
     */
    virtual void OutputData(std::vector<int16_t>& data);
    /**
     * @brief 从麦克风读取一帧 PCM。
     * @param data 输出缓冲区。
     * @return 成功读到数据返回 true。
     */
    virtual bool InputData(std::vector<int16_t>& data);
    /**
     * @brief 启动 Codec；具体输入输出仍由 EnableInput/EnableOutput 按需开启。
     */
    virtual void Start();

    inline bool duplex() const { return duplex_; }
    inline bool input_reference() const { return input_reference_; }
    inline int input_sample_rate() const { return input_sample_rate_; }
    inline int output_sample_rate() const { return output_sample_rate_; }
    inline int input_channels() const { return input_channels_; }
    inline int output_channels() const { return output_channels_; }
    inline int output_volume() const { return output_volume_; }
    inline float input_gain() const { return input_gain_; }
    inline bool input_enabled() const { return input_enabled_; }
    inline bool output_enabled() const { return output_enabled_; }

protected:
    i2s_chan_handle_t tx_handle_ = nullptr;
    i2s_chan_handle_t rx_handle_ = nullptr;

    bool duplex_ = false;
    bool input_reference_ = false;
    bool input_enabled_ = false;
    bool output_enabled_ = false;
    int input_sample_rate_ = 0;
    int output_sample_rate_ = 0;
    int input_channels_ = 1;
    int output_channels_ = 1;
    int output_volume_ = 70;
    float input_gain_ = 0.0;

    /**
     * @brief 底层阻塞读取。
     * @param dest 目标 PCM 缓冲区。
     * @param samples 期望样本数。
     * @return 实际样本数。
     */
    virtual int Read(int16_t* dest, int samples) = 0;
    /**
     * @brief 底层阻塞写入。
     * @param data PCM 缓冲区。
     * @param samples 样本数。
     * @return 实际写入样本数。
     */
    virtual int Write(const int16_t* data, int samples) = 0;
};

#endif // _AUDIO_CODEC_H
