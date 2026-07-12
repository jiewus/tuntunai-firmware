#ifndef WAKE_WORD_H
#define WAKE_WORD_H

#include <string>
#include <vector>
#include <functional>

#include <model_path.h>
#include "audio_codec.h"

/**
 * @file wake_word.h
 * @brief 本地离线唤醒词引擎的统一接口。
 */

/**
 * @brief 接收固定长度 PCM 并报告唤醒词的抽象类。
 */
class WakeWord {
public:
    virtual ~WakeWord() = default;
    
    /**
     * @brief 选择并加载模型。
     * @param codec 用于确定声道布局。
     * @param models_list 可用模型列表。
     */
    virtual bool Initialize(AudioCodec* codec, srmodel_list_t* models_list) = 0;
    /**
     * @brief 输入原始 PCM；样本数应等于 GetFeedSize()。
     */
    virtual void Feed(const std::vector<int16_t>& data) = 0;
    /**
     * @brief 注册检测成功回调。
     * @param callback 参数为模型对应的唤醒词文本。
     */
    virtual void OnWakeWordDetected(std::function<void(const std::string& wake_word)> callback) = 0;
    /**
     * @brief 开始检测并清除旧音频。
     */
    virtual void Start() = 0;
    /**
     * @brief 停止检测。
     */
    virtual void Stop() = 0;
    /**
     * @return WakeNet 每次推理需要的 PCM 样本数。
     */
    virtual size_t GetFeedSize() = 0;
    /**
     * @return 最近一次检测到的唤醒词。
     */
    virtual const std::string& GetLastDetectedWakeWord() const = 0;
};

#endif
