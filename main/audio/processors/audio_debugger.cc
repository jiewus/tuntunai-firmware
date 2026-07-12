/**
 * @file audio_debugger.cc
 * @brief audio_debugger.cc 中各类和辅助函数的具体实现。
 */
#include "audio_debugger.h"
#include "sdkconfig.h"

#if CONFIG_USE_AUDIO_DEBUGGER
#include <esp_log.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <cstring>
#include <string>
#endif

#define TAG "AudioDebugger"


/**
 * @brief 根据编译配置创建 UDP socket 和目标地址。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
AudioDebugger::AudioDebugger() {
#if CONFIG_USE_AUDIO_DEBUGGER
    udp_sockfd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_sockfd_ >= 0) {
        // 解析配置的服务器地址 "IP:PORT"
        std::string server_addr = CONFIG_AUDIO_DEBUG_UDP_SERVER;
        size_t colon_pos = server_addr.find(':');
        
        if (colon_pos != std::string::npos) {
            std::string ip = server_addr.substr(0, colon_pos);
            int port = std::stoi(server_addr.substr(colon_pos + 1));
            
            memset(&udp_server_addr_, 0, sizeof(udp_server_addr_));
            udp_server_addr_.sin_family = AF_INET;
            udp_server_addr_.sin_port = htons(port);
            inet_pton(AF_INET, ip.c_str(), &udp_server_addr_.sin_addr);
            
            ESP_LOGI(TAG, "Initialized server address: %s", CONFIG_AUDIO_DEBUG_UDP_SERVER);
        } else {
            ESP_LOGW(TAG, "Invalid server address: %s, should be IP:PORT", CONFIG_AUDIO_DEBUG_UDP_SERVER);
            close(udp_sockfd_);
            udp_sockfd_ = -1;
        }
    } else {
        ESP_LOGW(TAG, "Failed to create UDP socket: %d", errno);
    }
#endif
}

/**
 * @brief 关闭 UDP socket。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
AudioDebugger::~AudioDebugger() {
#if CONFIG_USE_AUDIO_DEBUGGER
    if (udp_sockfd_ >= 0) {
        close(udp_sockfd_);
        ESP_LOGI(TAG, "Closed UDP socket");
    }
#endif
}

/**
 * @file audio_debugger.h
 * @brief 将原始 PCM 通过 UDP 镜像到开发机的调试工具。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void AudioDebugger::Feed(const std::vector<int16_t>& data) {
#if CONFIG_USE_AUDIO_DEBUGGER
    if (udp_sockfd_ >= 0) {
        ssize_t sent = sendto(udp_sockfd_, data.data(), data.size() * sizeof(int16_t), 0,
                             (struct sockaddr*)&udp_server_addr_, sizeof(udp_server_addr_));
        if (sent < 0) {
            ESP_LOGW(TAG, "Failed to send audio data to %s: %d", CONFIG_AUDIO_DEBUG_UDP_SERVER, errno);
        } else {
            ESP_LOGD(TAG, "Sent %d bytes audio data to %s", sent, CONFIG_AUDIO_DEBUG_UDP_SERVER);
        }
    }
#endif
}

 
