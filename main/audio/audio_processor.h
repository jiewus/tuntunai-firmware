#ifndef AUDIO_PROCESSOR_H
#define AUDIO_PROCESSOR_H

#include <string>
#include <vector>
#include <functional>

#include <model_path.h>
#include "audio_codec.h"

/**
 * @file audio_processor.h
 * @brief 麦克风 PCM 预处理和 VAD 的可替换接口。
 */

/**
 * @brief 将 Codec 原始音频转换为适合 Opus 编码的单声道 PCM。
 */
class AudioProcessor {
public:
    virtual ~AudioProcessor() = default;
    
    /**
     * @brief 绑定硬件与模型。
     * @param codec 音频硬件。
     * @param frame_duration_ms 输出帧长。
     * @param models_list ESP-SR 模型。
     */
    virtual void Initialize(AudioCodec* codec, int frame_duration_ms, srmodel_list_t* models_list) = 0;
    /**
     * @brief 输入一段 PCM，所有权移动给处理器。
     */
    virtual void Feed(std::vector<int16_t>&& data) = 0;
    /**
     * @brief 开始接受和输出数据。
     */
    virtual void Start() = 0;
    /**
     * @brief 停止处理并清除未完成帧。
     */
    virtual void Stop() = 0;
    /**
     * @brief 查询处理器是否运行。
     */
    virtual bool IsRunning() = 0;
    /**
     * @brief 注册处理完成回调；回调取得输出 PCM 所有权。
     */
    virtual void OnOutput(std::function<void(std::vector<int16_t>&& data)> callback) = 0;
    /**
     * @brief 注册 VAD 状态变化回调；speaking=true 表示检测到人声。
     */
    virtual void OnVadStateChange(std::function<void(bool speaking)> callback) = 0;
    /**
     * @return 每次 Feed() 应提供的样本数。
     */
    virtual size_t GetFeedSize() = 0;
};

#endif
