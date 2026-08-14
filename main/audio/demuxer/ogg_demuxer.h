#ifndef OGG_DEMUXER_H_
#define OGG_DEMUXER_H_

#include <functional>
#include <cstdint>
#include <cstring>
#include <vector>

/**
 * @file ogg_demuxer.h
 * @brief 面向流式输入的 Ogg/Opus 解封装器。
 */

/**
 * @brief 将任意分片到达的 Ogg 页面重组为连续 Opus 数据包。
 *
 * 解析器不要求一次输入完整页面；Process() 会保存状态并在下一段数据继续。
 * 固定 8 KB 包缓冲避免播放提示音时频繁动态分配。容器缺少 OpusHead/OpusTags
 * 时，使用构造参数指定的回退采样率继续拆分 Opus 包，兼容平台豆包语音输出。
 */
class OggDemuxer {
private:
    enum ParseState : int8_t {
        FIND_PAGE,
        PARSE_HEADER,
        PARSE_SEGMENTS,
        PARSE_DATA
    };

    struct Opus_t {
        bool    head_seen{false};
        bool    tags_seen{false};
        int     sample_rate{48000};
    };


    // 使用固定大小的缓冲区避免动态分配
    struct context_t {
        bool packet_continued{false};   // 当前包是否跨多个段
        uint8_t header[27];             // Ogg页头
        uint8_t seg_table[255];         // 当前存储的段表
        uint8_t packet_buf[8192];       // 8KB包缓冲区
        size_t packet_len = 0;          // 缓冲区中累计的数据长度
        size_t seg_count = 0;           // 当前页段数
        size_t seg_index = 0;           // 当前处理的段索引
        size_t data_offset = 0;         // 解析当前阶段已读取的字节数
        size_t bytes_needed = 0;        // 解析当前字段还需要读取的字节数
        size_t seg_remaining = 0;       // 当前段剩余需要读取的字节数
        size_t body_size = 0;           // 数据体总大小
        size_t body_offset = 0;         // 数据体已读取的字节数
    };
    
public:
    explicit OggDemuxer(int fallback_sample_rate = 48000)
        : fallback_sample_rate_(fallback_sample_rate) {
        Reset();
    }
    
    /**
     * @brief 清空解析状态、页面上下文和已识别的 Opus 信息。
     */
    void Reset();
    
    /**
     * @brief 消费一段 Ogg 字节流。
     * @param data 输入首地址。
     * @param size 可读字节数。
     * @return 已消费字节数。
     */
    size_t Process(const uint8_t* data, size_t size);

    /// @brief 设置每个完整 Opus 包解封装完成后的回调。
    /// @param on_demuxer_finished 参数依次为包数据、Opus 采样率、包时长和包长度；数据仅在回调期间有效。
    void OnDemuxerFinished(
        std::function<void(const uint8_t* data, int sample_rate, int frame_duration, size_t len)>
            on_demuxer_finished) {
        on_demuxer_finished_ = on_demuxer_finished;
    }
private:

    /**
     * @brief 根据 Opus TOC 和帧数量计算当前完整包所需的解码时长。
     * @param data 完整 Opus 包首地址。
     * @param size 完整 Opus 包字节数。
     * @return 解码器支持的最小覆盖时长，异常包回退为 60 ms。
     */
    static int GetPacketDurationMilliseconds(const uint8_t* data, size_t size);

    int         fallback_sample_rate_ = 48000;
    ParseState  state_ = ParseState::FIND_PAGE;
    context_t   ctx_;
    Opus_t      opus_info_;
    std::function<void(const uint8_t*, int, int, size_t)> on_demuxer_finished_;
};

#endif
