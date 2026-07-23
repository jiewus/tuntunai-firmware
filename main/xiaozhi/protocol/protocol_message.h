#ifndef XIAOZHI_PROTOCOL_MESSAGE_H
#define XIAOZHI_PROTOCOL_MESSAGE_H

#include <cJSON.h>

#include <string>

/**
 * @file protocol_message.h
 * @brief 小智协议公共握手消息的构建和解析接口。
 */

/**
 * @brief 服务端 hello 中所有传输方式共用的会话和音频参数。
 */
struct XiaozhiServerHello {
    std::string session_id;
    int sample_rate = 24000;
    int frame_duration = 60;
};

/**
 * @brief 构建声明 MCP 和 Opus 能力的客户端 hello 消息。
 * @param version 小智协议版本。
 * @param transport 音频传输方式，当前为 udp 或 websocket。
 * @return 无多余空白的 hello JSON；内存不足时返回空字符串。
 */
std::string BuildXiaozhiHelloMessage(int version, const char* transport);

/**
 * @brief 解析服务端 hello 的公共字段并校验传输方式。
 * @param root 服务端 hello JSON 根对象。
 * @param expected_transport 当前协议实现要求的传输方式。
 * @param hello 成功时写入会话编号、采样率和帧时长。
 * @return 字段完整且合法时返回 true，否则返回 false。
 */
bool ParseXiaozhiServerHello(
    const cJSON* root,
    const char* expected_transport,
    XiaozhiServerHello& hello);

#endif  // XIAOZHI_PROTOCOL_MESSAGE_H
