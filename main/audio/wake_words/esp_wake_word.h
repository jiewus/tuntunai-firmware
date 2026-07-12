#ifndef ESP_WAKE_WORD_H
#define ESP_WAKE_WORD_H

#include <esp_wn_iface.h>
#include <esp_wn_models.h>
#include <model_path.h>

#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <mutex>

#include "audio_codec.h"
#include "wake_word.h"

/**
 * @file esp_wake_word.h
 * @brief Espressif WakeNet 唤醒词适配器。
 */

/**
 * @brief 使用资源分区中的 WakeNet9s 模型执行本地唤醒检测。
 */
class EspWakeWord : public WakeWord {
public:
    EspWakeWord();
    ~EspWakeWord();

    /**
     * @brief 从模型列表选择 wakenet 模型并创建推理实例。
     */
    bool Initialize(AudioCodec* codec, srmodel_list_t* models_list);
    /**
     * @brief 累积 PCM，在满足模型帧长后执行一次或多次推理。
     */
    void Feed(const std::vector<int16_t>& data);
    /**
     * @brief 保存唤醒成功回调。
     */
    void OnWakeWordDetected(std::function<void(const std::string& wake_word)> callback);
    /**
     * @brief 原子开启推理并清空输入缓存。
     */
    void Start();
    /**
     * @brief 原子停止推理并清空输入缓存。
     */
    void Stop();
    /**
     * @return 模型接口要求的每帧样本数乘以输入声道数。
     */
    size_t GetFeedSize();
    const std::string& GetLastDetectedWakeWord() const { return last_detected_wake_word_; }

private:
    esp_wn_iface_t *wakenet_iface_ = nullptr;
    model_iface_data_t *wakenet_data_ = nullptr;
    srmodel_list_t *wakenet_model_ = nullptr;
    AudioCodec* codec_ = nullptr;
    std::atomic<bool> running_ = false;

    std::function<void(const std::string& wake_word)> wake_word_detected_callback_;
    std::string last_detected_wake_word_;
    std::vector<int16_t> input_buffer_;
    std::mutex input_buffer_mutex_;
};

#endif
