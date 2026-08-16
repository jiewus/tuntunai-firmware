#ifndef WAKE_WORD_AUDIO_CACHE_H
#define WAKE_WORD_AUDIO_CACHE_H

#include <cstddef>
#include <cstdint>
#include <mutex>

/**
 * @file wake_word_audio_cache.h
 * @brief 唤醒词音频缓存：用于在唤醒词命中后把检测窗口内的麦克风 PCM 重新上传。
 *
 * 本地唤醒阶段麦克风 PCM 只送入 WakeNet 做推理，不经过上传编码链路。当启用了唤醒词音频
 * 上报（CONFIG_SEND_WAKE_WORD_DATA）时，本环形缓冲在唤醒词检测期间持续保存最近一段
 * 16 kHz 单声道 PCM，命中唤醒词后由 AudioService 取出并编码为 Opus 上传，供云端对齐唤醒词
 * 语义与后续对话上下文。
 */

/**
 * @brief 常驻 PSRAM 的唤醒词音频环形缓存。
 * @details 容量在 Initialize() 时一次分配，超过容量时覆盖最旧的样本，保证始终保存最近一段
 *          输入音频。并发读写受内部互斥锁保护，可安全地从音频输入任务写入、从主任务读取。
 */
class WakeWordAudioCache {
public:
    WakeWordAudioCache() = default;
    ~WakeWordAudioCache();
    WakeWordAudioCache(const WakeWordAudioCache&) = delete;
    WakeWordAudioCache& operator=(const WakeWordAudioCache&) = delete;

    /**
     * @brief 分配并初始化环形缓冲区。
     * @param sample_count 需要保存的单声道 16 kHz 样本总数。
     * @return 分配成功或容量一致时返回 true；PSRAM 分配失败或样本数为零时返回 false。
     */
    bool Initialize(size_t sample_count);
    /**
     * @brief 写入一帧 PCM，超出容量时覆盖最旧样本。
     * @param data 待写入样本首地址。
     * @param samples 本次写入的样本数。
     */
    void Store(const int16_t* data, size_t samples);
    /**
     * @brief 读取从 offset 开始的连续样本。
     * @param offset 相对首样本的偏移。
     * @param output 接收数据的输出缓冲。
     * @param samples 想要读取的样本数，实际返回不超过缓存已存样本数。
     * @return 实际读取的样本数。
     */
    size_t Read(size_t offset, int16_t* output, size_t samples) const;
    /** @return 当前已缓存样本数。 */
    size_t Size() const;
    /** @brief 清空缓存内容。 */
    void Clear();

private:
    int16_t* buffer_ = nullptr;
    size_t capacity_ = 0;
    size_t size_ = 0;
    size_t write_position_ = 0;
    mutable std::mutex mutex_;
};

#endif  // WAKE_WORD_AUDIO_CACHE_H
