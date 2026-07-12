#ifndef DUMMY_AUDIO_PROCESSOR_H
#define DUMMY_AUDIO_PROCESSOR_H

#include <vector>
#include <functional>
#include <atomic>

#include "audio_processor.h"
#include "audio_codec.h"

/**
 * @file no_audio_processor.h
 * @brief 不启用 AFE/AEC 时的轻量 PCM 分帧处理器。
 */

/**
 * @brief 仅缓存和分帧，不进行降噪、回声消除或真实 VAD。
 */
class NoAudioProcessor : public AudioProcessor {
public:
    NoAudioProcessor() = default;
    ~NoAudioProcessor() = default;

    /**
     * @brief 根据 Codec 采样率和目标帧长计算每帧样本数；models_list 未使用。
     */
    void Initialize(AudioCodec* codec, int frame_duration_ms, srmodel_list_t* models_list) override;
    /**
     * @brief 累积输入并按固定帧长通过输出回调转交。
     */
    void Feed(std::vector<int16_t>&& data) override;
    void Start() override;
    void Stop() override;
    bool IsRunning() override;
    void OnOutput(std::function<void(std::vector<int16_t>&& data)> callback) override;
    void OnVadStateChange(std::function<void(bool speaking)> callback) override;
    size_t GetFeedSize() override;

private:
    AudioCodec* codec_ = nullptr;
    int frame_samples_ = 0;
    std::vector<int16_t> output_buffer_;
    std::function<void(std::vector<int16_t>&& data)> output_callback_;
    std::function<void(bool speaking)> vad_state_change_callback_;
    std::atomic<bool> is_running_ = false;
};

#endif 
