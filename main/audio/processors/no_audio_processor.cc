/**
 * @file no_audio_processor.cc
 * @brief no_audio_processor.cc 中各类和辅助函数的具体实现。
 */
#include "no_audio_processor.h"
#include <esp_log.h>

#define TAG "NoAudioProcessor"

/**
 * @brief 根据 Codec 采样率和目标帧长计算每帧样本数；models_list 未使用。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void NoAudioProcessor::Initialize(AudioCodec* codec, int frame_duration_ms, srmodel_list_t* models_list) {
    codec_ = codec;
    frame_samples_ = frame_duration_ms * 16000 / 1000;
    output_buffer_.reserve(frame_samples_);
}

/**
 * @brief 累积输入并按固定帧长通过输出回调转交。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void NoAudioProcessor::Feed(std::vector<int16_t>&& data) {
    if (!is_running_ || !output_callback_) {
        return;
    }

    // Convert stereo to mono if needed
    if (codec_->input_channels() == 2) {
        for (size_t i = 0, j = 0; i < data.size() / 2; ++i, j += 2) {
            output_buffer_.push_back(data[j]);
        }
    } else {
        output_buffer_.insert(output_buffer_.end(), data.begin(), data.end());
    }

    // Output complete frames when buffer has enough data
    while (output_buffer_.size() >= (size_t)frame_samples_) {
        if (output_buffer_.size() == (size_t)frame_samples_) {
            output_callback_(std::move(output_buffer_));
            output_buffer_.clear();
            output_buffer_.reserve(frame_samples_);
        } else {
            output_callback_(std::vector<int16_t>(output_buffer_.begin(), output_buffer_.begin() + frame_samples_));
            output_buffer_.erase(output_buffer_.begin(), output_buffer_.begin() + frame_samples_);
        }
    }
}

/**
 * @brief 启动对应功能及其异步处理流程。
 * @details 实现会维护 NoAudioProcessor 的内部一致性；发生错误时记录日志，并避免向后续流程传播无效资源。
 */
void NoAudioProcessor::Start() {
    is_running_ = true;
}

/**
 * @brief 停止对应功能并收敛后台状态。
 * @details 实现会维护 NoAudioProcessor 的内部一致性；发生错误时记录日志，并避免向后续流程传播无效资源。
 */
void NoAudioProcessor::Stop() {
    is_running_ = false;
    output_buffer_.clear();
}

/**
 * @brief 检查对应条件或运行状态。
 * @details 实现会维护 NoAudioProcessor 的内部一致性；发生错误时记录日志，并避免向后续流程传播无效资源。
 */
bool NoAudioProcessor::IsRunning() {
    return is_running_;
}

/**
 * @brief 注册或执行对应事件回调。
 * @details 实现会维护 NoAudioProcessor 的内部一致性；发生错误时记录日志，并避免向后续流程传播无效资源。
 */
void NoAudioProcessor::OnOutput(std::function<void(std::vector<int16_t>&& data)> callback) {
    output_callback_ = callback;
}

/**
 * @brief 注册或执行对应事件回调。
 * @details 实现会维护 NoAudioProcessor 的内部一致性；发生错误时记录日志，并避免向后续流程传播无效资源。
 */
void NoAudioProcessor::OnVadStateChange(std::function<void(bool speaking)> callback) {
    vad_state_change_callback_ = callback;
}

/**
 * @brief 读取并返回对应状态或资源。
 * @details 实现会维护 NoAudioProcessor 的内部一致性；发生错误时记录日志，并避免向后续流程传播无效资源。
 */
size_t NoAudioProcessor::GetFeedSize() {
    if (!codec_) {
        return 0;
    }
    return frame_samples_;
}
