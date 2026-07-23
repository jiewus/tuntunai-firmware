/**
 * @file protocol_message.cc
 * @brief 小智协议公共握手消息的构建和解析实现。
 */

#include "xiaozhi/protocol/protocol_message.h"

#include <cstring>

#include "sdkconfig.h"

namespace {

constexpr int kXiaozhiOpusFrameDurationMs = 60;

}  // namespace

std::string BuildXiaozhiHelloMessage(int version, const char* transport) {
    if (transport == nullptr || transport[0] == '\0') {
        return {};
    }

    cJSON* root = cJSON_CreateObject();
    cJSON* features = cJSON_CreateObject();
    cJSON* audio_params = cJSON_CreateObject();
    if (root == nullptr || features == nullptr || audio_params == nullptr) {
        cJSON_Delete(root);
        cJSON_Delete(features);
        cJSON_Delete(audio_params);
        return {};
    }

    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddNumberToObject(root, "version", version);
    cJSON_AddStringToObject(root, "transport", transport);
#if CONFIG_USE_SERVER_AEC
    cJSON_AddBoolToObject(features, "aec", true);
#endif
    cJSON_AddBoolToObject(features, "mcp", true);
    cJSON_AddItemToObject(root, "features", features);
    cJSON_AddStringToObject(audio_params, "format", "opus");
    cJSON_AddNumberToObject(audio_params, "sample_rate", 16000);
    cJSON_AddNumberToObject(audio_params, "channels", 1);
    cJSON_AddNumberToObject(
        audio_params,
        "frame_duration",
        kXiaozhiOpusFrameDurationMs);
    cJSON_AddItemToObject(root, "audio_params", audio_params);

    char* serialized = cJSON_PrintUnformatted(root);
    std::string result = serialized == nullptr ? std::string() : std::string(serialized);
    cJSON_free(serialized);
    cJSON_Delete(root);
    return result;
}

bool ParseXiaozhiServerHello(
    const cJSON* root,
    const char* expected_transport,
    XiaozhiServerHello& hello) {
    if (root == nullptr || expected_transport == nullptr) {
        return false;
    }

    const cJSON* transport = cJSON_GetObjectItemCaseSensitive(root, "transport");
    const cJSON* session_id = cJSON_GetObjectItemCaseSensitive(root, "session_id");
    const cJSON* audio_params = cJSON_GetObjectItemCaseSensitive(root, "audio_params");
    if (!cJSON_IsString(transport)
        || std::strcmp(transport->valuestring, expected_transport) != 0
        || !cJSON_IsString(session_id)
        || session_id->valuestring[0] == '\0'
        || !cJSON_IsObject(audio_params)) {
        return false;
    }

    const cJSON* sample_rate = cJSON_GetObjectItemCaseSensitive(audio_params, "sample_rate");
    const cJSON* frame_duration = cJSON_GetObjectItemCaseSensitive(audio_params, "frame_duration");
    if (!cJSON_IsNumber(sample_rate)
        || sample_rate->valueint <= 0
        || !cJSON_IsNumber(frame_duration)
        || frame_duration->valueint <= 0) {
        return false;
    }

    hello.session_id = session_id->valuestring;
    hello.sample_rate = sample_rate->valueint;
    hello.frame_duration = frame_duration->valueint;
    return true;
}
