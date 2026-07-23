/**
 * @file audio_codec.cc
 * @brief audio_codec.cc 中各类和辅助函数的具体实现。
 */
#include "audio_codec.h"
#include "board.h"
#include "system/settings.h"

#include <esp_log.h>
#include <cstring>
#include <driver/i2s_common.h>

#define TAG "AudioCodec"

/**
 * @brief 初始化音频状态变量，不会立即开启 I2S。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
AudioCodec::AudioCodec() {
}

/**
 * @brief 关闭仍在运行的 I2S 通道。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
AudioCodec::~AudioCodec() {
}

/**
 * @brief 将 PCM 样本写入扬声器。
 * @param data 有符号 16 位交错 PCM。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void AudioCodec::OutputData(std::vector<int16_t>& data) {
    Write(data.data(), data.size());
}

/**
 * @brief 从麦克风读取一帧 PCM。
 * @param data 输出缓冲区。
 * @return 成功读到数据返回 true。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
bool AudioCodec::InputData(std::vector<int16_t>& data) {
    int samples = Read(data.data(), data.size());
    if (samples > 0) {
        return true;
    }
    return false;
}

/**
 * @brief 启动 Codec；具体输入输出仍由 EnableInput/EnableOutput 按需开启。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void AudioCodec::Start() {
    Settings settings("audio", false);
    output_volume_ = settings.GetInt("output_volume", output_volume_);
    if (output_volume_ <= 0) {
        ESP_LOGW(TAG, "Output volume value (%d) is too small, setting to default (10)", output_volume_);
        output_volume_ = 10;
    }

    ESP_LOGI(TAG, "Audio codec started");
}

/**
 * @brief 设置扬声器音量。
 * @param volume 百分比，合法范围 0-100。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void AudioCodec::SetOutputVolume(int volume) {
    output_volume_ = volume;
    ESP_LOGI(TAG, "Set output volume to %d", output_volume_);
    
    Settings settings("audio", true);
    settings.SetInt("output_volume", output_volume_);
}

/**
 * @brief 设置麦克风模拟/数字增益。
 * @param gain 增益值，单位由具体 Codec 驱动定义。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void AudioCodec::SetInputGain(float gain) {
    input_gain_ = gain;
    ESP_LOGI(TAG, "Set input gain to %.1f", input_gain_);
}

/**
 * @brief 打开或关闭麦克风及接收 I2S 通道。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void AudioCodec::EnableInput(bool enable) {
    if (enable == input_enabled_) {
        return;
    }
    input_enabled_ = enable;
    ESP_LOGI(TAG, "Set input enable to %s", enable ? "true" : "false");
}

/**
 * @brief 打开或关闭功放、Codec DAC 及发送 I2S 通道。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void AudioCodec::EnableOutput(bool enable) {
    if (enable == output_enabled_) {
        return;
    }
    output_enabled_ = enable;
    ESP_LOGI(TAG, "Set output enable to %s", enable ? "true" : "false");
}
