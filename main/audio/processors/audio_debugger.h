#ifndef AUDIO_DEBUGGER_H
#define AUDIO_DEBUGGER_H

#include <vector>
#include <cstdint>

#include <sys/socket.h>
#include <netinet/in.h>

/**
 * @file audio_debugger.h
 * @brief 将原始 PCM 通过 UDP 镜像到开发机的调试工具。
 */

/**
 * @brief 音频调试器；未配置调试地址时 Feed() 不发送数据。
 */
class AudioDebugger {
public:
    /**
     * @brief 根据编译配置创建 UDP socket 和目标地址。
     */
    AudioDebugger();
    /**
     * @brief 关闭 UDP socket。
     */
    ~AudioDebugger();

    /**
     * @brief 发送一块 16 位 PCM。
     * @param data 连续 PCM 样本，不改变其内容。
     */
    void Feed(const std::vector<int16_t>& data);

private:
    int udp_sockfd_ = -1;
    struct sockaddr_in udp_server_addr_;
};

#endif 
