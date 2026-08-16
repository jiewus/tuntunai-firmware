/**
 * @file wake_word_audio_cache.cc
 * @brief 唤醒词音频缓存实现。
 */
#include "wake_word_audio_cache.h"

#include <algorithm>
#include <cstring>

#include <esp_heap_caps.h>
#include <esp_log.h>

#define TAG "WakeWordCache"

/**
 * @brief 释放分配的 PSRAM 缓冲。
 */
WakeWordAudioCache::~WakeWordAudioCache() {
    if (buffer_ != nullptr) {
        heap_caps_free(buffer_);
    }
}

/**
 * @brief 分配并初始化环形缓冲区。
 * @param sample_count 需要保存的单声道 16 kHz 样本总数。
 * @details 优先从 PSRAM 分配以避免占用宝贵的内部 RAM；重复初始化时仅当容量一致才复用现有缓冲。
 */
bool WakeWordAudioCache::Initialize(size_t sample_count) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (buffer_ != nullptr) {
        return capacity_ == sample_count;
    }
    if (sample_count == 0) {
        return false;
    }

    buffer_ = static_cast<int16_t*>(heap_caps_malloc(
        sample_count * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buffer_ == nullptr) {
        // 某些板型 PSRAM 不可用时退化为内部 RAM，保证功能不失效。
        buffer_ = static_cast<int16_t*>(heap_caps_malloc(
            sample_count * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    if (buffer_ == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate %u bytes for wake word audio cache",
                 static_cast<unsigned>(sample_count * sizeof(int16_t)));
        return false;
    }

    capacity_ = sample_count;
    ESP_LOGI(TAG, "Allocated %u bytes for wake word audio cache",
             static_cast<unsigned>(capacity_ * sizeof(int16_t)));
    return true;
}

/**
 * @brief 写入一帧 PCM，超出容量时覆盖最旧样本。
 */
void WakeWordAudioCache::Store(const int16_t* data, size_t samples) {
    if (data == nullptr || samples == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (buffer_ == nullptr) {
        return;
    }

    // 一次写入超过整个容量时只保留最末 capacity_ 个样本。
    if (samples >= capacity_) {
        data += samples - capacity_;
        samples = capacity_;
        std::memcpy(buffer_, data, samples * sizeof(int16_t));
        size_ = capacity_;
        write_position_ = 0;
        return;
    }

    const size_t first = std::min(samples, capacity_ - write_position_);
    std::memcpy(buffer_ + write_position_, data, first * sizeof(int16_t));
    if (samples > first) {
        std::memcpy(buffer_, data + first, (samples - first) * sizeof(int16_t));
    }
    write_position_ = static_cast<size_t>((write_position_ + samples) % capacity_);
    size_ = std::min(capacity_, size_ + samples);
}

/**
 * @brief 读取从 offset 开始的连续样本。
 */
size_t WakeWordAudioCache::Read(size_t offset, int16_t* output, size_t samples) const {
    if (output == nullptr || samples == 0) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (buffer_ == nullptr || offset >= size_) {
        return 0;
    }

    samples = std::min(samples, size_ - offset);
    const size_t oldest = (write_position_ + capacity_ - size_) % capacity_;
    const size_t read_position = (oldest + offset) % capacity_;
    const size_t first = std::min(samples, capacity_ - read_position);
    std::memcpy(output, buffer_ + read_position, first * sizeof(int16_t));
    if (samples > first) {
        std::memcpy(output + first, buffer_, (samples - first) * sizeof(int16_t));
    }
    return samples;
}

/**
 * @return 当前已缓存样本数。
 */
size_t WakeWordAudioCache::Size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return size_;
}

/**
 * @brief 清空缓存内容。
 */
void WakeWordAudioCache::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    size_ = 0;
    write_position_ = 0;
}
