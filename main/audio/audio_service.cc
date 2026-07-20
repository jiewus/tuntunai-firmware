/**
 * @file audio_service.cc
 * @brief 音频采集、编解码、队列调度和播放任务实现。
 */
#include "audio_service.h"
#include <esp_log.h>
#include <cstring>

#define RATE_CVT_CFG(_src_rate, _dest_rate, _channel)        \
    (esp_ae_rate_cvt_cfg_t)                                  \
    {                                                        \
        .src_rate        = (uint32_t)(_src_rate),            \
        .dest_rate       = (uint32_t)(_dest_rate),           \
        .channel         = (uint8_t)(_channel),              \
        .bits_per_sample = ESP_AUDIO_BIT16,                  \
        .complexity      = 2,                                \
        .perf_type       = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,  \
    }

#define OPUS_DEC_CFG(_sample_rate, _frame_duration_ms)                                                    \
    (esp_opus_dec_cfg_t)                                                                                  \
    {                                                                                                     \
        .sample_rate    = (uint32_t)(_sample_rate),                                                       \
        .channel        = ESP_AUDIO_MONO,                                                                 \
        .frame_duration = (esp_opus_dec_frame_duration_t)AS_OPUS_GET_FRAME_DRU_ENUM(_frame_duration_ms),  \
        .self_delimited = false,                                                                          \
    }

#if CONFIG_USE_AUDIO_PROCESSOR
#include "processors/afe_audio_processor.h"
#else
#include "processors/no_audio_processor.h"
#endif

#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4
#include "wake_words/afe_wake_word.h"
#include "wake_words/custom_wake_word.h"
#else
#include "wake_words/esp_wake_word.h"
#endif

#define TAG "AudioService"

/**
 * @brief 创建同步对象和定时器，尚未绑定硬件。
 * @details 构造阶段仅创建 FreeRTOS 事件组，Codec、编解码器和定时器在 Initialize() 中配置。
 */
AudioService::AudioService() {
    event_group_ = xEventGroupCreate();
}

/**
 * @brief 停止任务并释放 Opus、重采样器和同步资源。
 * @details 释放事件组、Opus 编解码器以及输入输出重采样器。正常销毁前应先调用 Stop()，
 *          确保后台任务不再访问这些句柄。
 */
AudioService::~AudioService() {
    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
    }
    if (opus_encoder_ != nullptr) {
        esp_opus_enc_close(opus_encoder_);
    }
    if (opus_decoder_ != nullptr) {
        esp_opus_dec_close(opus_decoder_);
    }
    if (input_resampler_ != nullptr) {
        esp_ae_rate_cvt_close(input_resampler_);
    }
    if (output_resampler_ != nullptr) {
        esp_ae_rate_cvt_close(output_resampler_);
    }
}

/**
 * @brief 绑定板级 Codec 并创建唤醒词、处理器和编解码器。
 * @param codec 非空且生命周期覆盖本服务。
 * @details 启动 Codec 后按输出参数创建 Opus 解码器，按 16 kHz 上行参数创建编码器；输入采样率
 *          非 16 kHz 时建立重采样器。随后选择音频处理器并创建音频电源检查定时器。
 */
void AudioService::Initialize(AudioCodec* codec) {
    codec_ = codec;
    codec_->Start();

    esp_opus_dec_cfg_t opus_dec_cfg = OPUS_DEC_CFG(codec->output_sample_rate(), OPUS_FRAME_DURATION_MS);
    auto ret = esp_opus_dec_open(&opus_dec_cfg, sizeof(esp_opus_dec_cfg_t), &opus_decoder_);
    if (opus_decoder_ == nullptr) {
        ESP_LOGE(TAG, "音频解码器创建失败，错误码=%d", ret);
    } else {
        decoder_sample_rate_ = codec->output_sample_rate();
        decoder_duration_ms_ = OPUS_FRAME_DURATION_MS;
        decoder_frame_size_ = decoder_sample_rate_ / 1000 * OPUS_FRAME_DURATION_MS;
    }
    esp_opus_enc_config_t opus_enc_cfg = AS_OPUS_ENC_CONFIG();
    ret = esp_opus_enc_open(&opus_enc_cfg, sizeof(esp_opus_enc_config_t), &opus_encoder_);
    if (opus_encoder_ == nullptr) {
        ESP_LOGE(TAG, "音频编码器创建失败，错误码=%d", ret);
    } else {
        encoder_sample_rate_ = 16000;
        encoder_duration_ms_ = OPUS_FRAME_DURATION_MS;
        esp_opus_enc_get_frame_size(opus_encoder_, &encoder_frame_size_, &encoder_outbuf_size_);
        encoder_frame_size_ = encoder_frame_size_ / sizeof(int16_t);
    }

    if (codec->input_sample_rate() != 16000) {
        esp_ae_rate_cvt_cfg_t input_resampler_cfg = RATE_CVT_CFG(
            codec->input_sample_rate(), ESP_AUDIO_SAMPLE_RATE_16K, codec->input_channels());
        auto resampler_ret = esp_ae_rate_cvt_open(&input_resampler_cfg, &input_resampler_);
        if (input_resampler_ == nullptr) {
            ESP_LOGE(TAG, "输入音频重采样器创建失败，错误码=%d", resampler_ret);
        }
    }

#if CONFIG_USE_AUDIO_PROCESSOR
    audio_processor_ = std::make_unique<AfeAudioProcessor>();
#else
    audio_processor_ = std::make_unique<NoAudioProcessor>();
#endif

    audio_processor_->OnOutput([this](std::vector<int16_t>&& data) {
        PushTaskToEncodeQueue(kAudioTaskTypeEncodeToSendQueue, std::move(data));
    });

    audio_processor_->OnVadStateChange([this](bool speaking) {
        voice_detected_ = speaking;
        if (callbacks_.on_vad_change) {
            callbacks_.on_vad_change(speaking);
        }
    });

    esp_timer_create_args_t audio_power_timer_args = {
        .callback = [](void* arg) {
            AudioService* audio_service = (AudioService*)arg;
            audio_service->CheckAndUpdateAudioPowerState();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "audio_power_timer",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&audio_power_timer_args, &audio_power_timer_);
}

/**
 * @brief 创建输入、输出、Opus 三个后台任务并开始处理。
 * @details 清除停止标志和运行事件位，启动电源检查定时器，并按是否启用 AFE 选择任务栈大小。
 *          输入任务优先级最高，输出任务次之，编解码任务使用较低优先级和较大栈空间。
 */
void AudioService::Start() {
    service_stopped_ = false;
    xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING | AS_EVENT_WAKE_WORD_RUNNING | AS_EVENT_AUDIO_PROCESSOR_RUNNING);

    esp_timer_start_periodic(audio_power_timer_, 1000000);

#if CONFIG_USE_AUDIO_PROCESSOR
    /* 创建固定在核心 0 上运行的音频输入任务。 */
    xTaskCreatePinnedToCore([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->AudioInputTask();
        vTaskDelete(NULL);
    }, "audio_input", 2048 * 3, this, 8, &audio_input_task_handle_, 0);

    /* 创建音频输出任务。 */
    xTaskCreate([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->AudioOutputTask();
        vTaskDelete(NULL);
    }, "audio_output", 2048 * 2, this, 4, &audio_output_task_handle_);
#else
    /* 无 AFE 时创建栈空间较小的音频输入任务。 */
    xTaskCreate([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->AudioInputTask();
        vTaskDelete(NULL);
    }, "audio_input", 2048 * 2, this, 8, &audio_input_task_handle_);

    /* 无 AFE 时创建栈空间较小的音频输出任务。 */
    xTaskCreate([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->AudioOutputTask();
        vTaskDelete(NULL);
    }, "audio_output", 2048, this, 4, &audio_output_task_handle_);
#endif

    /* 创建统一处理 Opus 编码和解码的后台任务。 */
    xTaskCreate([](void* arg) {
        AudioService* audio_service = (AudioService*)arg;
        audio_service->OpusCodecTask();
        vTaskDelete(NULL);
    }, "opus_codec", 2048 * 12, this, 2, &opus_codec_task_handle_);
}

/**
 * @brief 请求后台任务停止并唤醒所有正在等待的队列。
 * @details 停止电源定时器，设置 service_stopped_，同时置位所有输入事件并清空队列，
 *          使等待事件组或条件变量的三个后台任务都能退出循环。
 */
void AudioService::Stop() {
    esp_timer_stop(audio_power_timer_);
    service_stopped_ = true;
    xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING |
        AS_EVENT_WAKE_WORD_RUNNING |
        AS_EVENT_AUDIO_PROCESSOR_RUNNING);

    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    audio_encode_queue_.clear();
    audio_decode_queue_.clear();
    audio_playback_queue_.clear();
    audio_testing_queue_.clear();
    audio_queue_cv_.notify_all();
}

/**
 * @brief 从 Codec 读取并按需重采样到指定参数。
 * @param data 输出 PCM。
 * @param sample_rate 目标采样率 Hz。
 * @param samples 目标单声道样本数。
 * @return 成功取得指定数量的 PCM 数据时返回 true，Codec 读取失败时返回 false。
 * @details 必要时重新启用输入电源。Codec 原始采样率不匹配时先按输入通道数读取足量数据，再在
 *          互斥锁保护下重采样；读取成功会刷新最近输入时间并按配置送入音频调试器。
 */
bool AudioService::ReadAudioData(std::vector<int16_t>& data, int sample_rate, int samples) {
    if (!codec_->input_enabled()) {
        esp_timer_stop(audio_power_timer_);
        esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
        codec_->EnableInput(true);
    }

    if (codec_->input_sample_rate() != sample_rate) {
        data.resize(samples * codec_->input_sample_rate() / sample_rate * codec_->input_channels());
        if (!codec_->InputData(data)) {
            return false;
        }
        if (input_resampler_ != nullptr) {
            std::lock_guard<std::mutex> lock(input_resampler_mutex_);
            uint32_t in_sample_num = data.size() / codec_->input_channels();
            uint32_t output_samples = 0;
            esp_ae_rate_cvt_get_max_out_sample_num(input_resampler_, in_sample_num, &output_samples);
            input_resample_buffer_.resize(output_samples * codec_->input_channels());
            uint32_t actual_output = output_samples;
            esp_ae_rate_cvt_process(input_resampler_, (esp_ae_sample_t)data.data(), in_sample_num,
                                   (esp_ae_sample_t)input_resample_buffer_.data(), &actual_output);
            data.assign(
                input_resample_buffer_.begin(),
                input_resample_buffer_.begin() + actual_output * codec_->input_channels());
        }
    } else {
        data.resize(samples * codec_->input_channels());
        if (!codec_->InputData(data)) {
            return false;
        }
    }

    /* 记录最近一次成功采集时间，供输入自动断电逻辑使用。 */
    last_input_time_ = std::chrono::steady_clock::now();
    debug_statistics_.input_count++;

#if CONFIG_USE_AUDIO_DEBUGGER
    // 音频调试：发送原始音频数据
    if (audio_debugger_ == nullptr) {
        audio_debugger_ = std::make_unique<AudioDebugger>();
    }
    audio_debugger_->Feed(data);
#endif

    return true;
}

/**
 * @brief 循环读取麦克风，将数据分发给唤醒词、处理器或测试队列。
 * @details 任务阻塞等待音频测试、唤醒词检测或语音处理事件。测试模式按 Opus 帧长采集，
 *          正常模式按 10 ms 采集并同时投递给已启用的唤醒词和语音处理模块。
 *          读取超时只会短暂让出 CPU，不会结束常驻输入任务。
 */
void AudioService::AudioInputTask() {
    std::vector<int16_t> input_data;
    std::vector<int16_t> testing_data;
    input_data.reserve(
        static_cast<size_t>(160) * codec_->input_sample_rate() / 16000
        * codec_->input_channels());
    testing_data.reserve(
        static_cast<size_t>(encoder_frame_size_) * codec_->input_channels());
    while (true) {
        EventBits_t bits = xEventGroupWaitBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING |
            AS_EVENT_WAKE_WORD_RUNNING | AS_EVENT_AUDIO_PROCESSOR_RUNNING,
            pdFALSE, pdFALSE, portMAX_DELAY);

        if (service_stopped_) {
            break;
        }
        if (audio_input_need_warmup_) {
            audio_input_need_warmup_ = false;
            vTaskDelay(pdMS_TO_TICKS(120));
            continue;
        }

        // 配网状态下按 BOOT 键进入音频测试；采集数据经过 Opus 编码后用于本地回放验证。
        if (bits & AS_EVENT_AUDIO_TESTING_RUNNING) {
            bool testing_queue_full = false;
            {
                std::lock_guard<std::mutex> lock(audio_queue_mutex_);
                testing_queue_full = audio_testing_queue_.size()
                    >= AUDIO_TESTING_MAX_DURATION_MS / OPUS_FRAME_DURATION_MS;
            }
            if (testing_queue_full) {
                ESP_LOGW(TAG, "音频测试队列已满，正在停止音频测试");
                EnableAudioTesting(false);
                continue;
            }
            int samples = OPUS_FRAME_DURATION_MS * 16000 / 1000;
            if (ReadAudioData(testing_data, 16000, samples)) {
                // 双通道输入只取左声道，保证测试编码器收到 16 kHz 单声道 PCM。
                if (codec_->input_channels() == 2) {
                    auto mono_data = std::vector<int16_t>(testing_data.size() / 2);
                    for (size_t i = 0, j = 0; i < mono_data.size(); ++i, j += 2) {
                        mono_data[i] = testing_data[j];
                    }
                    testing_data = std::move(mono_data);
                }
                PushTaskToEncodeQueue(
                    kAudioTaskTypeEncodeToTestingQueue, std::move(testing_data));
                continue;
            }
        }

        // 将每个 10 ms 音频块分别送入当前启用的唤醒词检测器和语音前处理器。
        if (bits & (AS_EVENT_WAKE_WORD_RUNNING | AS_EVENT_AUDIO_PROCESSOR_RUNNING)) {
            int samples = 160; // 10ms
            if (ReadAudioData(input_data, 16000, samples)) {
                if (bits & AS_EVENT_WAKE_WORD_RUNNING) {
                    wake_word_->Feed(input_data);
                }
                if (bits & AS_EVENT_AUDIO_PROCESSOR_RUNNING) {
                    audio_processor_->Feed(std::move(input_data));
                }
                continue;
            }
        }

        // 单次读取超时或失败不应终止输入任务，延时后继续尝试可避免空转占满 CPU。
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    ESP_LOGW(TAG, "音频输入任务已停止");
}

/**
 * @brief 循环从播放队列取 PCM 并写入 Codec。
 * @details 任务通过条件变量等待解码后的 PCM；取出任务后先释放队列锁，再启用 Codec 输出并播放，
 *          避免慢速 I/O 阻塞编解码队列。每次播放会刷新音频活跃时间，供自动断电逻辑判断。
 */
void AudioService::AudioOutputTask() {
    while (true) {
        std::unique_lock<std::mutex> lock(audio_queue_mutex_);
        audio_queue_cv_.wait(lock, [this]() { return !audio_playback_queue_.empty() || service_stopped_; });
        if (service_stopped_) {
            break;
        }

        auto task = std::move(audio_playback_queue_.front());
        audio_playback_queue_.pop_front();
        audio_output_active_ = true;
        audio_queue_cv_.notify_all();
        lock.unlock();

        if (!codec_->output_enabled()) {
            esp_timer_stop(audio_power_timer_);
            esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
            codec_->EnableOutput(true);
        }

        codec_->OutputData(task->pcm);

        // 记录最近一次输出时间，用于在持续静音后关闭功放和 Codec 输出。
        last_output_time_ = std::chrono::steady_clock::now();
        debug_statistics_.playback_count++;

        lock.lock();
#if CONFIG_USE_SERVER_AEC
        /* 服务端 AEC 启用时保存播放时间戳，用于关联回声参考帧。 */
        if (task->timestamp > 0) {
            timestamp_queue_.push_back(task->timestamp);
        }
#endif
        audio_output_active_ = false;
        task->timestamp = 0;
        task->pcm.clear();
        reusable_audio_tasks_.push_back(std::move(task));
        audio_queue_cv_.notify_all();
    }

    ESP_LOGW(TAG, "音频输出任务已停止");
}

/**
 * @brief 在同一任务中串行执行 Opus 编码和解码，降低并发内存峰值。
 * @details 条件变量仅在输入队列有数据且目标队列仍有容量时唤醒任务。解码路径把云端 Opus
 *          数据转换为播放 PCM，并在采样率不一致时重采样；编码路径把 16 kHz PCM 转为
 *          待上传的 Opus 包。队列容量限制用于建立背压，防止网络或播放变慢时内存持续增长。
 */
void AudioService::OpusCodecTask() {
    while (true) {
        std::unique_lock<std::mutex> lock(audio_queue_mutex_);
        audio_queue_cv_.wait(lock, [this]() {
            return service_stopped_ ||
                (!audio_encode_queue_.empty() && audio_send_queue_.size() < MAX_SEND_PACKETS_IN_QUEUE) ||
                (!audio_decode_queue_.empty() && audio_playback_queue_.size() < MAX_PLAYBACK_TASKS_IN_QUEUE);
        });
        if (service_stopped_) {
            break;
        }

        // 优先把云端下发的 Opus 包解码为 PCM，并推入扬声器播放队列。
        if (!audio_decode_queue_.empty() && audio_playback_queue_.size() < MAX_PLAYBACK_TASKS_IN_QUEUE) {
            auto packet = std::move(audio_decode_queue_.front());
            audio_decode_queue_.pop_front();
            audio_decode_active_ = true;
            audio_queue_cv_.notify_all();
            std::unique_ptr<AudioTask> task;
            if (!reusable_audio_tasks_.empty()) {
                task = std::move(reusable_audio_tasks_.front());
                reusable_audio_tasks_.pop_front();
            } else {
                task = std::make_unique<AudioTask>();
            }
            lock.unlock();
            task->type = kAudioTaskTypeDecodeToPlaybackQueue;
            task->timestamp = packet->timestamp;

            SetDecodeSampleRate(packet->sample_rate, packet->frame_duration);
            if (opus_decoder_ != nullptr) {
                task->pcm.resize(decoder_frame_size_);
                esp_audio_dec_in_raw_t raw = {
                    .buffer = (uint8_t *)(packet->payload.data()),
                    .len = (uint32_t)(packet->payload.size()),
                    .consumed = 0,
                    .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE,
                };
                esp_audio_dec_out_frame_t out_frame = {
                    .buffer = (uint8_t *)(task->pcm.data()),
                    .len = (uint32_t)(task->pcm.size() * sizeof(int16_t)),
                    .decoded_size = 0,
                };
                esp_audio_dec_info_t dec_info = {};
                std::unique_lock<std::mutex> decoder_lock(decoder_mutex_);
                auto ret = esp_opus_dec_decode(opus_decoder_, &raw, &out_frame, &dec_info);
                decoder_lock.unlock();
                if (ret == ESP_AUDIO_ERR_OK) {
                    task->pcm.resize(out_frame.decoded_size / sizeof(int16_t));
                    if (decoder_sample_rate_ != codec_->output_sample_rate() && output_resampler_ != nullptr) {
                        uint32_t target_size = 0;
                        esp_ae_rate_cvt_get_max_out_sample_num(output_resampler_, task->pcm.size(), &target_size);
                        output_resample_buffer_.resize(target_size);
                        uint32_t actual_output = target_size;
                        esp_ae_rate_cvt_process(output_resampler_, (esp_ae_sample_t)task->pcm.data(), task->pcm.size(),
                                                (esp_ae_sample_t)output_resample_buffer_.data(), &actual_output);
                        task->pcm.assign(
                            output_resample_buffer_.begin(),
                            output_resample_buffer_.begin() + actual_output);
                    }
                    lock.lock();
                    audio_playback_queue_.push_back(std::move(task));
                    audio_queue_cv_.notify_all();
                    debug_statistics_.decode_count++;
                } else {
                    ESP_LOGE(TAG, "音频解码失败，错误码=%d", ret);
                    lock.lock();
                    task->pcm.clear();
                    reusable_audio_tasks_.push_back(std::move(task));
                }
            } else {
                ESP_LOGE(TAG, "音频解码器尚未配置");
                lock.lock();
                task->pcm.clear();
                reusable_audio_tasks_.push_back(std::move(task));
            }
            audio_decode_active_ = false;
            audio_queue_cv_.notify_all();
            debug_statistics_.decode_count++;
        }
        // 把麦克风处理后的 PCM 编码为 Opus，并推入协议发送队列。
        if (!audio_encode_queue_.empty() && audio_send_queue_.size() < MAX_SEND_PACKETS_IN_QUEUE) {
            auto task = std::move(audio_encode_queue_.front());
            audio_encode_queue_.pop_front();
            audio_queue_cv_.notify_all();
            lock.unlock();

            auto packet = std::make_unique<AudioStreamPacket>();
            packet->frame_duration = OPUS_FRAME_DURATION_MS;
            packet->sample_rate = 16000;
            packet->timestamp = task->timestamp;

            if (opus_encoder_ != nullptr && task->pcm.size() == encoder_frame_size_) {
                packet->payload.resize(encoder_outbuf_size_);
                esp_audio_enc_in_frame_t in = {
                    .buffer = (uint8_t *)(task->pcm.data()),
                    .len = (uint32_t)(encoder_frame_size_ * sizeof(int16_t)),
                };
                esp_audio_enc_out_frame_t out = {
                    .buffer = packet->payload.data(),
                    .len = (uint32_t)encoder_outbuf_size_,
                    .encoded_bytes = 0,
                };
                auto ret = esp_opus_enc_process(opus_encoder_, &in, &out);
                if (ret == ESP_AUDIO_ERR_OK) {
                    packet->payload.resize(out.encoded_bytes);

                    if (task->type == kAudioTaskTypeEncodeToSendQueue) {
                        {
                            std::lock_guard<std::mutex> lock2(audio_queue_mutex_);
                            audio_send_queue_.push_back(std::move(packet));
                        }
                        if (callbacks_.on_send_queue_available) {
                            callbacks_.on_send_queue_available();
                        }
                    } else if (task->type == kAudioTaskTypeEncodeToTestingQueue) {
                        std::lock_guard<std::mutex> lock2(audio_queue_mutex_);
                        audio_testing_queue_.push_back(std::move(packet));
                    }
                    debug_statistics_.encode_count++;
                } else {
                    ESP_LOGE(TAG, "音频编码失败，错误码=%d", ret);
                }
            } else {
                ESP_LOGE(TAG, "音频编码失败：编码器未配置或帧长度无效，实际=%u，期望=%u",
                         task->pcm.size(), encoder_frame_size_);
            }
            lock.lock();
            task->timestamp = 0;
            task->pcm.clear();
            reusable_audio_tasks_.push_back(std::move(task));
        }
    }

    ESP_LOGW(TAG, "Opus 编解码任务已停止");
}

/**
 * @brief 更新下行参数并重建解码器/输出重采样器。
 * @param sample_rate 服务端 Opus 音频采样率，单位为 Hz。
 * @param frame_duration 单个 Opus 帧的持续时间，单位为毫秒。
 * @details 参数未变化时直接返回；变化时在解码器互斥锁保护下重建 Opus 解码器，并在服务端
 *          采样率与 Codec 输出采样率不同时重建输出重采样器。
 */
void AudioService::SetDecodeSampleRate(int sample_rate, int frame_duration) {
    if (decoder_sample_rate_ == sample_rate && decoder_duration_ms_ == frame_duration) {
        return;
    }
    std::unique_lock<std::mutex> decoder_lock(decoder_mutex_);
    if (opus_decoder_ != nullptr) {
        esp_opus_dec_close(opus_decoder_);
        opus_decoder_ = nullptr;
    }
    decoder_lock.unlock();
    esp_opus_dec_cfg_t opus_dec_cfg = OPUS_DEC_CFG(sample_rate, frame_duration);
    auto ret = esp_opus_dec_open(&opus_dec_cfg, sizeof(esp_opus_dec_cfg_t), &opus_decoder_);
    if (opus_decoder_ == nullptr) {
        ESP_LOGE(TAG, "音频解码器创建失败，错误码=%d", ret);
        return;
    }
    decoder_sample_rate_ = sample_rate;
    decoder_duration_ms_ = frame_duration;
    decoder_frame_size_ = decoder_sample_rate_ / 1000 * frame_duration;

    auto codec = Board::GetInstance().GetAudioCodec();
    if (decoder_sample_rate_ != codec->output_sample_rate()) {
        ESP_LOGI(TAG, "正在将音频采样率从%d赫兹转换为%d赫兹",
                 decoder_sample_rate_, codec->output_sample_rate());
        if (output_resampler_ != nullptr) {
            esp_ae_rate_cvt_close(output_resampler_);
            output_resampler_ = nullptr;
        }
        esp_ae_rate_cvt_cfg_t output_resampler_cfg = RATE_CVT_CFG(
            decoder_sample_rate_, codec->output_sample_rate(), ESP_AUDIO_MONO);
        auto resampler_ret = esp_ae_rate_cvt_open(&output_resampler_cfg, &output_resampler_);
        if (output_resampler_ == nullptr) {
            ESP_LOGE(TAG, "输出音频重采样器创建失败，错误码=%d", resampler_ret);
        }
    }
}

/**
 * @brief 将一帧 PCM 任务加入有界编码队列。
 * @param type 指定编码结果进入实时发送队列还是音频测试队列。
 * @param pcm 待编码的 PCM 样本，调用后底层缓冲区所有权转移给队列任务。
 * @details 队列达到容量上限时阻塞等待 Opus 任务腾出空间，成功入队后唤醒编解码任务。
 */
void AudioService::PushTaskToEncodeQueue(AudioTaskType type, std::vector<int16_t>&& pcm) {
    // 队列满时等待消费者取走任务，防止无界增长耗尽内存。
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);

    audio_queue_cv_.wait(lock, [this]() { return audio_encode_queue_.size() < MAX_ENCODE_TASKS_IN_QUEUE; });
    std::unique_ptr<AudioTask> task;
    if (!reusable_audio_tasks_.empty()) {
        task = std::move(reusable_audio_tasks_.front());
        reusable_audio_tasks_.pop_front();
    } else {
        task = std::make_unique<AudioTask>();
    }
    task->type = type;
    task->pcm = std::move(pcm);
    audio_encode_queue_.push_back(std::move(task));
    audio_queue_cv_.notify_all();
}

/**
 * @brief 将云端 Opus 包加入解码队列。
 * @param packet 数据包所有权。
 * @param wait 队列满时是否等待空间；网络回调通常传 false，避免阻塞收包线程。
 * @return 数据包成功入队时返回 true；队列已满且 wait 为 false 时返回 false。
 * @details wait 为 true 时通过条件变量等待播放链路释放容量，入队后通知 Opus 解码任务。
 */
bool AudioService::PushPacketToDecodeQueue(std::unique_ptr<AudioStreamPacket> packet, bool wait) {
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);
    if (audio_decode_queue_.size() >= MAX_DECODE_PACKETS_IN_QUEUE) {
        if (wait) {
            audio_queue_cv_.wait(lock, [this]() { return audio_decode_queue_.size() < MAX_DECODE_PACKETS_IN_QUEUE; });
        } else {
            return false;
        }
    }
    audio_decode_queue_.push_back(std::move(packet));
    audio_queue_cv_.notify_all();
    return true;
}

/**
 * @brief 取出最早的上行 Opus 包。
 * @return 队列为空时返回 nullptr。
 * @details 返回非空指针时，音频包所有权从发送队列转移给调用者，并通知可能等待队列空间的编码任务。
 */
std::unique_ptr<AudioStreamPacket> AudioService::PopPacketFromSendQueue() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    if (audio_send_queue_.empty()) {
        return nullptr;
    }
    auto packet = std::move(audio_send_queue_.front());
    audio_send_queue_.pop_front();
    audio_queue_cv_.notify_all();
    return packet;
}

/**
 * @brief 获取最近一次由唤醒词模块识别出的文本。
 * @return 最近一次识别到的唤醒词；尚未识别时为空字符串。
 * @details 返回对唤醒词模块内部字符串的只读引用，调用者不得保存到 AudioService 生命周期之外。
 */
const std::string& AudioService::GetLastWakeWord() const {
    return wake_word_->GetLastDetectedWakeWord();
}

/**
 * @brief 启停本地 WakeNet 检测。
 * @param enable true 开启，false 停止并清缓冲。
 * @details 首次启用时延迟初始化模型；切换到唤醒词模式前重置输入重采样器，清除另一种音频块长度
 *          留下的缓存，避免切换 Feed 尺寸后发生缓冲区溢出。
 */
void AudioService::EnableWakeWordDetection(bool enable) {
    if (!wake_word_) {
        return;
    }

    ESP_LOGD(TAG, "%s唤醒词检测", enable ? "正在启用" : "正在停用");
    if (enable) {
        if (!wake_word_initialized_) {
            if (!wake_word_->Initialize(codec_, models_list_)) {
                ESP_LOGE(TAG, "唤醒词模块初始化失败");
                return;
            }
            wake_word_initialized_ = true;
        }
        // 清除语音处理模式遗留的重采样缓存，避免切换音频块长度后发生缓冲区溢出。
        {
            std::lock_guard<std::mutex> lock(input_resampler_mutex_);
            if (input_resampler_ != nullptr) {
                esp_ae_rate_cvt_reset(input_resampler_);
            }
        }
        wake_word_->Start();
        xEventGroupSetBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
    } else {
        wake_word_->Stop();
        xEventGroupClearBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
    }
}

/**
 * @brief 启停上传前的语音处理/VAD 流水线。
 * @param enable true 启动语音处理和上行编码，false 停止处理并清除运行事件位。
 * @details 首次启用时初始化处理器；启动前清空播放和解码数据，设置麦克风预热标志并重置输入
 *          重采样缓存，保证唤醒词模式与语音处理模式切换时不会混入旧样本。
 */
void AudioService::EnableVoiceProcessing(bool enable) {
    ESP_LOGD(TAG, "%s语音处理", enable ? "正在启用" : "正在停用");
    if (enable) {
        if (!audio_processor_initialized_) {
            audio_processor_->Initialize(codec_, OPUS_FRAME_DURATION_MS, models_list_);
            audio_processor_initialized_ = true;
        }

        // 开始上传麦克风前清空下行播放，避免提示音或上一轮 TTS 混入新会话。
        ResetDecoder();
        audio_input_need_warmup_ = true;
        // 清除唤醒词模式遗留的重采样缓存，避免切换音频块长度后发生缓冲区溢出。
        {
            std::lock_guard<std::mutex> lock(input_resampler_mutex_);
            if (input_resampler_ != nullptr) {
                esp_ae_rate_cvt_reset(input_resampler_);
            }
        }
        audio_processor_->Start();
        xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_PROCESSOR_RUNNING);
    } else {
        audio_processor_->Stop();
        xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_PROCESSOR_RUNNING);
    }
}

/**
 * @brief 启停十秒回环录音测试模式。
 * @param enable true 开始采集并编码测试音频，false 停止采集并把测试包转入解码播放队列。
 * @details 停止测试时使用移动赋值把整段测试录音交给正常解码链路，实现录音后的本地回放。
 */
void AudioService::EnableAudioTesting(bool enable) {
    ESP_LOGI(TAG, "%s音频测试", enable ? "正在启用" : "正在停用");
    if (enable) {
        xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING);
    } else {
        xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING);
        // 将测试期间生成的 Opus 包整体转入解码队列，随后由扬声器回放。
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        audio_decode_queue_ = std::move(audio_testing_queue_);
        audio_queue_cv_.notify_all();
    }
}

/**
 * @brief 设置应用层回调。
 * @param callbacks 回调会被复制保存。
 * @details 回调分别用于通知上行队列可读、唤醒词命中和 VAD 状态变化，执行线程取决于事件来源。
 */
void AudioService::SetCallbacks(AudioServiceCallbacks& callbacks) {
    callbacks_ = callbacks;
}

/**
 * @brief 解封装并播放内嵌 OGG/Opus 提示音。
 * @param ogg 完整 OGG 文件数据视图，调用期间必须保持有效。
 * @details 必要时先启用 Codec 输出，再由 OggDemuxer 拆出每个 Opus 包，并以阻塞方式加入解码队列，
 *          保证短提示音不会因队列临时满载而丢帧。
 */
void AudioService::PlaySound(const std::string_view& ogg) {
    if (!codec_->output_enabled()) {
        esp_timer_stop(audio_power_timer_);
        esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
        codec_->EnableOutput(true);
    }

    const auto* buf = reinterpret_cast<const uint8_t*>(ogg.data());
    size_t size = ogg.size();

    auto demuxer = std::make_unique<OggDemuxer>();
    demuxer->OnDemuxerFinished(
        [this](const uint8_t* data, int sample_rate, int frame_duration, size_t size){
            auto packet = std::make_unique<AudioStreamPacket>();
            packet->sample_rate = sample_rate;
            packet->frame_duration = frame_duration;
            packet->payload.resize(size);
            std::memcpy(packet->payload.data(), data, size);
            PushPacketToDecodeQueue(std::move(packet), true);
        });
    demuxer->Reset();
    demuxer->Process(buf, size);
}

/**
 * @brief 所有编解码与播放队列为空且没有正在解码或输出的音频帧时返回 true。
 * @return 编码、解码、播放和测试队列全部为空且下行音频处理空闲时返回 true，否则返回 false。
 * @details 判断过程受队列互斥锁保护，结果只代表调用瞬间的快照。
 */
bool AudioService::IsIdle() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    return audio_encode_queue_.empty()
        && audio_decode_queue_.empty()
        && audio_playback_queue_.empty()
        && audio_testing_queue_.empty()
        && !audio_decode_active_
        && !audio_output_active_;
}

/**
 * @brief 阻塞等待所有待播放 PCM 消耗完，用于重启或切换音频状态。
 * @details 条件变量会在解码队列为空、没有正在解码的 Opus 包、播放队列为空且最后一帧已经写入
 *          Codec，或服务已停止时唤醒调用者。
 */
void AudioService::WaitForPlaybackQueueEmpty() {
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);
    audio_queue_cv_.wait(lock, [this]() { 
        return service_stopped_
            || (audio_decode_queue_.empty()
                && !audio_decode_active_
                && audio_playback_queue_.empty()
                && !audio_output_active_);
    });
}

/**
 * @brief 重置 Opus 解码状态并清空所有下行播放数据。
 * @details 在队列锁和解码器锁保护下调用解码器 reset，随后清空解码、播放和测试队列，
 *          用于打断 TTS 或开始新会话时防止旧音频继续播放。
 */
void AudioService::ResetDecoder() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    std::unique_lock<std::mutex> decoder_lock(decoder_mutex_);
    if (opus_decoder_ != nullptr) {
        esp_opus_dec_reset(opus_decoder_);
    }
    decoder_lock.unlock();
    audio_decode_queue_.clear();
    audio_playback_queue_.clear();
    audio_testing_queue_.clear();
    audio_queue_cv_.notify_all();
}

/**
 * @brief 根据最近输入输出时间自动关闭或唤醒 Codec 电源。
 * @details 输入或输出超过空闲超时后关闭对应 Codec 方向。全双工输入仍活跃时保留发送时钟，
 *          避免部分板卡接收链路停顿；输入输出均关闭后停止电源检查定时器。
 */
void AudioService::CheckAndUpdateAudioPowerState() {
    auto now = std::chrono::steady_clock::now();
    auto input_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_input_time_).count();
    auto output_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_output_time_).count();
    if (input_elapsed > AUDIO_POWER_TIMEOUT_MS && codec_->input_enabled()) {
        codec_->EnableInput(false);
    }
    if (output_elapsed > AUDIO_POWER_TIMEOUT_MS && codec_->output_enabled()) {
        // 全双工接收仍工作时保留 TX 时钟，否则部分板卡的 RX 可能停顿。
        if (!(codec_->duplex() && codec_->input_enabled())) {
            codec_->EnableOutput(false);
        }
    }
    if (!codec_->input_enabled() && !codec_->output_enabled()) {
        esp_timer_stop(audio_power_timer_);
    }
}

/**
 * @brief 设置资源分区加载出的 ESP-SR 模型列表；列表所有权仍归资源管理器。
 * @param models_list ESP-SR 模型列表指针，可为空；AudioService 不负责释放。
 * @details 在支持 AFE 的芯片上按模型前缀选择自定义命令词或 WakeNet 实现；其他目标继续使用
 *          构造阶段创建的轻量唤醒词实现。
 */
void AudioService::SetModelsList(srmodel_list_t* models_list) {
    models_list_ = models_list;

#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4
    if (esp_srmodel_filter(models_list_, ESP_MN_PREFIX, NULL) != nullptr) {
        wake_word_ = std::make_unique<CustomWakeWord>();
    } else if (esp_srmodel_filter(models_list_, ESP_WN_PREFIX, NULL) != nullptr) {
        wake_word_ = std::make_unique<AfeWakeWord>();
    } else {
        wake_word_ = nullptr;
    }
#else
    if (esp_srmodel_filter(models_list_, ESP_WN_PREFIX, NULL) != nullptr) {
        wake_word_ = std::make_unique<EspWakeWord>();
    } else {
        wake_word_ = nullptr;
    }
#endif

    if (wake_word_) {
        wake_word_->OnWakeWordDetected([this](const std::string& wake_word) {
            if (callbacks_.on_wake_word_detected) {
                callbacks_.on_wake_word_detected(wake_word);
            }
        });
    }
}
