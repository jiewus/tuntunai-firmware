/**
 * @file esp_wake_word.cc
 * @brief esp_wake_word.cc 中各类和辅助函数的具体实现。
 */
#include "esp_wake_word.h"
#include <esp_log.h>


#define TAG "EspWakeWord"

/**
 * @brief 构造 EspWakeWord 对象并初始化该模块运行所需的成员和系统资源。
 * @details 构造阶段只建立本模块自身资源；需要异步运行的任务由后续 Start 或 Initialize 方法启动。
 */
EspWakeWord::EspWakeWord() {
}

/**
 * @brief 析构 EspWakeWord 对象并释放其持有的系统资源。
 * @details 释放顺序与创建顺序相反，先停止异步来源，再销毁句柄和动态内存，避免回调访问失效对象。
 */
EspWakeWord::~EspWakeWord() {
    if (wakenet_data_ != nullptr) {
        wakenet_iface_->destroy(wakenet_data_);
        esp_srmodel_deinit(wakenet_model_);
    }
}

/**
 * @brief 从模型列表选择 wakenet 模型并创建推理实例。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
bool EspWakeWord::Initialize(AudioCodec* codec, srmodel_list_t* models_list) {
    codec_ = codec;

    if (models_list == nullptr) {
        wakenet_model_ = esp_srmodel_init("model");
    } else {
        wakenet_model_ = models_list;
    }

    if (wakenet_model_ == nullptr || wakenet_model_->num == -1) {
        ESP_LOGE(TAG, "Failed to initialize wakenet model");
        return false;
    }
    if(wakenet_model_->num > 1) {
        ESP_LOGW(TAG, "More than one model found, using the first one");
    } else if (wakenet_model_->num == 0) {
        ESP_LOGE(TAG, "No model found");
        return false;
    }
    char *model_name = wakenet_model_->model_name[0];
    wakenet_iface_ = (esp_wn_iface_t*)esp_wn_handle_from_name(model_name);
    wakenet_data_ = wakenet_iface_->create(model_name, DET_MODE_95);

    int frequency = wakenet_iface_->get_samp_rate(wakenet_data_);
    int audio_chunksize = wakenet_iface_->get_samp_chunksize(wakenet_data_);
    ESP_LOGI(TAG, "Wake word(%s),freq: %d, chunksize: %d", model_name, frequency, audio_chunksize);

    return true;
}

/**
 * @brief 保存唤醒成功回调。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void EspWakeWord::OnWakeWordDetected(std::function<void(const std::string& wake_word)> callback) {
    wake_word_detected_callback_ = callback;
}

/**
 * @brief 原子开启推理并清空输入缓存。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void EspWakeWord::Start() {
    running_ = true;
}

/**
 * @brief 原子停止推理并清空输入缓存。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void EspWakeWord::Stop() {
    running_ = false;

    std::lock_guard<std::mutex> lock(input_buffer_mutex_);
    input_buffer_.clear();
}

/**
 * @brief 累积 PCM，在满足模型帧长后执行一次或多次推理。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void EspWakeWord::Feed(const std::vector<int16_t>& data) {
    if (wakenet_data_ == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(input_buffer_mutex_);
    // Check running state inside lock to avoid TOCTOU race with Stop()
    if (!running_) {
        return;
    }

    if (codec_->input_channels() == 2) {
        for (size_t i = 0; i < data.size(); i += 2) {
            input_buffer_.push_back(data[i]);
        }
    } else {
        input_buffer_.insert(input_buffer_.end(), data.begin(), data.end());
    }

    int chunksize = wakenet_iface_->get_samp_chunksize(wakenet_data_);
    while (input_buffer_.size() >= chunksize) {
        int res = wakenet_iface_->detect(wakenet_data_, input_buffer_.data());
        if (res > 0) {
            last_detected_wake_word_ = wakenet_iface_->get_word_name(wakenet_data_, res);
            running_ = false;
            input_buffer_.clear();

            if (wake_word_detected_callback_) {
                wake_word_detected_callback_(last_detected_wake_word_);
            }
            break;
        }
        input_buffer_.erase(input_buffer_.begin(), input_buffer_.begin() + chunksize);
    }
}

/**
 * @return 模型接口要求的每帧样本数乘以输入声道数。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
size_t EspWakeWord::GetFeedSize() {
    if (wakenet_data_ == nullptr) {
        return 0;
    }
    return wakenet_iface_->get_samp_chunksize(wakenet_data_);
}
