/**
 * @file mqtt_protocol.cc
 * @brief MQTT 控制通道与加密 UDP 音频通道实现。
 */
#include "xiaozhi/protocol/mqtt_protocol.h"
#include "xiaozhi/protocol/protocol_message.h"
#include "board.h"
#include "app/application.h"
#include "system/settings.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/task.h>
#include <cctype>
#include <cstring>
#include <arpa/inet.h>
#include "assets/lang_config.h"

#define TAG "MQTT"

/**
 * @brief 初始化 AES 上下文、事件组和重连定时器。
 * @details 构造阶段不建立网络连接。重连定时器触发后仅在设备空闲且对象仍存活时，
 *          将重新连接操作调度到应用主线程，避免析构后的延迟回调访问无效对象。
 */
MqttProtocol::MqttProtocol() {
    event_group_handle_ = xEventGroupCreate();

    // 创建断线重连定时器；实际连接动作由 Application 主线程调度执行。
    esp_timer_create_args_t reconnect_timer_args = {
        .callback = [](void* arg) {
            MqttProtocol* protocol = (MqttProtocol*)arg;
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateIdle) {
                ESP_LOGI(TAG, "正在重新连接小智 MQTT 服务");
                auto alive = protocol->alive_;  // 共享存活标志用于过滤析构后才执行的调度任务。
                app.Schedule([protocol, alive]() {
                    if (*alive) {
                        protocol->StartMqttClient(false);
                    }
                });
            }
        },
        .arg = this,
    };
    esp_timer_create(&reconnect_timer_args, &reconnect_timer_);
}

/**
 * @brief 停止定时器和网络对象，并使所有延迟回调失效。
 * @details 先清除共享存活标志，再停止定时器和销毁 MQTT、UDP 对象，确保已经进入应用调度队列的
 *          重连闭包不会继续访问 this。最后释放用于等待服务器 hello 的事件组。
 */
MqttProtocol::~MqttProtocol() {
    ESP_LOGI(TAG, "正在释放小智 MQTT 协议资源");
    
    // 必须先标记对象失效，阻止已经排队但尚未执行的重连任务访问 this。
    *alive_ = false;
    
    if (reconnect_timer_ != nullptr) {
        esp_timer_stop(reconnect_timer_);
        esp_timer_delete(reconnect_timer_);
    }

    udp_.reset();
    mqtt_.reset();
    
    if (event_group_handle_ != nullptr) {
        vEventGroupDelete(event_group_handle_);
    }
}

/**
 * @brief 根据 OTA 配置连接 MQTT 并订阅下行主题。
 * @return MQTT 客户端连接成功时返回 true，否则返回 false。
 * @details 初次启动不向应用层弹出错误，后续打开音频通道时会按需重连并上报失败。
 */
bool MqttProtocol::Start() {
    return StartMqttClient(false);
}

/**
 * @brief 创建并连接 MQTT 客户端。
 * @param report_error 失败时是否上报到应用层。
 * @return 成功建立 MQTT 连接时返回 true，配置缺失或连接失败时返回 false。
 * @details 从 NVS 读取服务器、认证和主题参数，注册连接、断线及消息回调，然后建立 TLS MQTT
 *          连接。断线回调启动一次性重连定时器；下行消息按 hello、goodbye 或普通 JSON 分发。
 */
bool MqttProtocol::StartMqttClient(bool report_error) {
    if (mqtt_ != nullptr) {
        ESP_LOGW(TAG, "小智 MQTT 客户端已启动，将重新创建连接");
        mqtt_.reset();
    }

    Settings settings("mqtt", false);
    auto endpoint = settings.GetString("endpoint");
    auto client_id = settings.GetString("client_id");
    auto username = settings.GetString("username");
    auto password = settings.GetString("password");
    int keepalive_interval = settings.GetInt("keepalive", 240);
    publish_topic_ = settings.GetString("publish_topic");

    if (endpoint.empty()) {
        ESP_LOGW(TAG, "未配置小智 MQTT 服务地址");
        if (report_error) {
            SetError(Lang::Strings::SERVER_NOT_FOUND);
        }
        return false;
    }

    auto network = Board::GetInstance().GetNetwork();
    mqtt_ = network->CreateMqtt(0);
    mqtt_->SetKeepAlive(keepalive_interval);

    mqtt_->OnDisconnected([this]() {
        if (on_disconnected_ != nullptr) {
            on_disconnected_();
        }
        ESP_LOGI(TAG, "小智 MQTT 已断开，将在 %d 秒后重连", MQTT_RECONNECT_INTERVAL_MS / 1000);
        esp_timer_start_once(reconnect_timer_, MQTT_RECONNECT_INTERVAL_MS * 1000);
    });

    mqtt_->OnConnected([this]() {
        if (on_connected_ != nullptr) {
            on_connected_();
        }
        esp_timer_stop(reconnect_timer_);
    });

    mqtt_->OnMessage([this](const std::string& topic, const std::string& payload) {
        cJSON* root = cJSON_Parse(payload.c_str());
        if (root == nullptr) {
            ESP_LOGE(TAG, "解析小智 MQTT JSON 消息失败，内容=%s", payload.c_str());
            return;
        }
        cJSON* type = cJSON_GetObjectItem(root, "type");
        if (!cJSON_IsString(type)) {
            ESP_LOGE(TAG, "小智 MQTT 消息类型无效");
            cJSON_Delete(root);
            return;
        }

        if (strcmp(type->valuestring, "hello") == 0) {
            ParseServerHello(root);
        } else if (strcmp(type->valuestring, "goodbye") == 0) {
            auto session_id = cJSON_GetObjectItem(root, "session_id");
            ESP_LOGI(TAG, "收到小智会话结束消息，会话标识=%s", cJSON_IsString(session_id) ? session_id->valuestring : "空");
            if (cJSON_IsString(session_id) && session_id_ == session_id->valuestring) {
                auto alive = alive_;  // 调度关闭动作时保留共享存活标志，避免对象析构后执行。
                Application::GetInstance().Schedule([this, alive]() {
                    if (*alive) {
                        // 服务端主动结束会话时不再回发 goodbye，防止双方重复应答。
                        CloseAudioChannel(false);
                    }
                });
            }
        } else if (on_incoming_json_ != nullptr) {
            on_incoming_json_(root);
        }
        cJSON_Delete(root);
        last_incoming_time_ = std::chrono::steady_clock::now();
    });

    ESP_LOGI(TAG, "正在连接小智 MQTT 服务，地址=%s", endpoint.c_str());
    std::string broker_address;
    int broker_port = 8883;
    size_t pos = endpoint.find(':');
    if (pos != std::string::npos) {
        broker_address = endpoint.substr(0, pos);
        broker_port = std::stoi(endpoint.substr(pos + 1));
    } else {
        broker_address = endpoint;
    }
    if (!mqtt_->Connect(broker_address, broker_port, client_id, username, password)) {
        ESP_LOGE(TAG, "连接小智 MQTT 服务失败，错误码=%d", mqtt_->GetLastError());
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        return false;
    }

    ESP_LOGI(TAG, "小智 MQTT 服务连接成功");
    return true;
}

/**
 * @brief 向设备发布主题发送 JSON 文本。
 * @param text 需要发布的 UTF-8 JSON 文本。
 * @return 发布主题有效且 MQTT 发布成功时返回 true，否则返回 false。
 * @details 发布失败会设置服务器错误，触发应用层统一错误提示。
 */
bool MqttProtocol::SendText(const std::string& text) {
    if (publish_topic_.empty()) {
        return false;
    }
    if (!mqtt_->Publish(publish_topic_, text)) {
        ESP_LOGE(TAG, "发布小智 MQTT 消息失败，内容=%s", text.c_str());
        SetError(Lang::Strings::SERVER_ERROR);
        return false;
    }
    return true;
}

/**
 * @brief 添加序号与时间戳、AES 加密后通过 UDP 发送音频。
 * @param packet 待发送的 Opus 音频包；调用后所有权转移给本方法。
 * @return UDP 成功写入至少一个字节时返回 true，通道不可用或加密失败时返回 false。
 * @details 使用服务端下发的 nonce 模板写入负载长度、时间戳和递增序号，再以 AES-CTR 加密
 *          Opus 负载。通道互斥锁保证关闭 UDP 与发送音频不会并发访问网络对象。
 */
bool MqttProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    std::lock_guard<std::mutex> lock(channel_mutex_);
    if (udp_ == nullptr) {
        return false;
    }

    std::string nonce(aes_nonce_);
    *(uint16_t*)&nonce[2] = htons(packet->payload.size());
    *(uint32_t*)&nonce[8] = htonl(packet->timestamp);
    *(uint32_t*)&nonce[12] = htonl(++local_sequence_);

    std::string encrypted;
    encrypted.resize(aes_nonce_.size() + packet->payload.size());
    memcpy(encrypted.data(), nonce.data(), nonce.size());

    if (aes_key_id_ == 0) {
        ESP_LOGE(TAG, "AES 密钥尚未初始化，无法加密上行音频");
        return false;
    }
    // 用本包独立的 nonce 作为 IV，对 Opus 负载做一次 AES-CTR 加密（PSA Crypto）。
    psa_cipher_operation_t operation = psa_cipher_operation_init();
    psa_status_t cipher_status = PSA_SUCCESS;
    cipher_status = psa_cipher_encrypt_setup(
        &operation, aes_key_id_, PSA_ALG_CTR);
    if (cipher_status == PSA_SUCCESS) {
        cipher_status = psa_cipher_set_iv(
            &operation, reinterpret_cast<const unsigned char*>(nonce.data()), aes_nonce_.size());
    }
    size_t encrypted_len = 0;
    if (cipher_status == PSA_SUCCESS) {
        cipher_status = psa_cipher_update(
            &operation,
            reinterpret_cast<const unsigned char*>(packet->payload.data()),
            packet->payload.size(),
            reinterpret_cast<unsigned char*>(&encrypted[nonce.size()]),
            packet->payload.size(),
            &encrypted_len);
    }
    if (cipher_status == PSA_SUCCESS) {
        cipher_status = psa_cipher_finish(
            &operation,
            reinterpret_cast<unsigned char*>(&encrypted[nonce.size()]) + encrypted_len,
            packet->payload.size() - encrypted_len,
            &encrypted_len);
    }
    psa_cipher_abort(&operation);
    if (cipher_status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "加密上行音频数据失败，错误码=%ld", static_cast<long>(cipher_status));
        return false;
    }

    return udp_->Send(encrypted) > 0;
}

/**
 * @brief 关闭 UDP 会话并按需发布 goodbye。
 * @param send_goodbye 为 true 时表示客户端主动关闭，需要通过 MQTT 通知服务端；服务端主动关闭时应传 false。
 * @details 先在通道锁保护下销毁 UDP，阻止后续音频发送；再按需发布 goodbye，最后通知应用层
 *          恢复低功耗和空闲状态。无论是否发送成功，都会触发通道关闭回调。
 */
void MqttProtocol::CloseAudioChannel(bool send_goodbye) {
    {
        std::lock_guard<std::mutex> lock(channel_mutex_);
        udp_.reset();
    }

    ESP_LOGI(TAG, "正在关闭小智音频通道，是否发送结束消息=%d", send_goodbye);

    // 只有客户端主动关闭时才发送 goodbye；服务端已发送 goodbye 时不回发，避免消息往返循环。
    if (send_goodbye) {
        std::string message = "{";
        message += "\"session_id\":\"" + session_id_ + "\",";
        message += "\"type\":\"goodbye\"";
        message += "}";
        SendText(message);
    }

    if (on_audio_channel_closed_ != nullptr) {
        on_audio_channel_closed_();
    }
}

/**
 * @brief 通过 MQTT 请求服务器创建 UDP 音频会话并等待 hello。
 * @return MQTT、hello 握手和 UDP 初始化全部成功时返回 true，否则返回 false。
 * @details 若 MQTT 已断开会先尝试重连，然后发布客户端 hello 并最多等待十秒。服务器 hello
 *          提供 UDP 地址、AES 密钥和音频参数；随后创建 UDP 回调，校验序号、解密音频并上交应用层。
 */
bool MqttProtocol::OpenAudioChannel() {
    if (mqtt_ == nullptr || !mqtt_->IsConnected()) {
        ESP_LOGI(TAG, "小智 MQTT 尚未连接，正在立即重连");
        if (!StartMqttClient(true)) {
            return false;
        }
    }

    error_occurred_ = false;
    session_id_ = "";
    xEventGroupClearBits(event_group_handle_, MQTT_PROTOCOL_SERVER_HELLO_EVENT);

    auto message = GetHelloMessage();
    if (!SendText(message)) {
        return false;
    }

    // 等待服务器响应
    EventBits_t bits = xEventGroupWaitBits(event_group_handle_, MQTT_PROTOCOL_SERVER_HELLO_EVENT, pdTRUE, pdFALSE, pdMS_TO_TICKS(10000));
    if (!(bits & MQTT_PROTOCOL_SERVER_HELLO_EVENT)) {
        ESP_LOGE(TAG, "等待小智服务端 hello 超时");
        SetError(Lang::Strings::SERVER_TIMEOUT);
        return false;
    }

    std::lock_guard<std::mutex> lock(channel_mutex_);
    auto network = Board::GetInstance().GetNetwork();
    udp_ = network->CreateUdp(2);
    udp_->OnMessage([this](const std::string& data) {
        /*
         * UDP 加密 Opus 数据包格式：
         * |type 1u|flags 1u|payload_len 2u|ssrc 4u|timestamp 4u|sequence 4u|
         * |payload payload_len|
         */
        if (data.size() < sizeof(aes_nonce_)) {
            ESP_LOGE(TAG, "下行音频包长度无效，长度=%u", data.size());
            return;
        }
        if (data[0] != 0x01) {
            ESP_LOGE(TAG, "下行音频包类型无效，类型=%x", data[0]);
            return;
        }
        uint32_t timestamp = ntohl(*(uint32_t*)&data[8]);
        uint32_t sequence = ntohl(*(uint32_t*)&data[12]);
        if (sequence <= remote_sequence_) {
            return;
        }
        if (sequence != remote_sequence_ + 1) {
            remote_sequence_gap_count_ += sequence - remote_sequence_ - 1;
            const int64_t now_us = esp_timer_get_time();
            if (remote_sequence_last_warning_us_ == 0
                || now_us - remote_sequence_last_warning_us_ >= 1000000) {
                ESP_LOGW(TAG, "音频 UDP 出现丢包，当前序号=%lu，期望序号=%lu，最近丢失=%lu 个",
                         static_cast<unsigned long>(sequence),
                         static_cast<unsigned long>(remote_sequence_ + 1),
                         static_cast<unsigned long>(remote_sequence_gap_count_));
                remote_sequence_gap_count_ = 0;
                remote_sequence_last_warning_us_ = now_us;
            }
        }

        size_t decrypted_size = data.size() - aes_nonce_.size();
        auto nonce = (uint8_t*)data.data();
        auto encrypted = (uint8_t*)data.data() + aes_nonce_.size();
        auto packet = std::make_unique<AudioStreamPacket>();
        packet->sample_rate = server_sample_rate_;
        packet->frame_duration = server_frame_duration_;
        packet->timestamp = timestamp;
        packet->payload.resize(decrypted_size);
        psa_cipher_operation_t operation = psa_cipher_operation_init();
        psa_status_t cipher_status = psa_cipher_decrypt_setup(
            &operation, aes_key_id_, PSA_ALG_CTR);
        if (cipher_status == PSA_SUCCESS) {
            cipher_status = psa_cipher_set_iv(&operation, nonce, aes_nonce_.size());
        }
        size_t decrypted_len = 0;
        if (cipher_status == PSA_SUCCESS) {
            cipher_status = psa_cipher_update(
                &operation, encrypted, decrypted_size,
                reinterpret_cast<unsigned char*>(packet->payload.data()), decrypted_size,
                &decrypted_len);
        }
        if (cipher_status == PSA_SUCCESS) {
            cipher_status = psa_cipher_finish(
                &operation,
                reinterpret_cast<unsigned char*>(packet->payload.data()) + decrypted_len,
                decrypted_size - decrypted_len,
                &decrypted_len);
        }
        psa_cipher_abort(&operation);
        if (cipher_status != PSA_SUCCESS) {
            ESP_LOGE(TAG, "解密下行音频数据失败，错误码=%ld", static_cast<long>(cipher_status));
            return;
        }
        if (on_incoming_audio_ != nullptr) {
            on_incoming_audio_(std::move(packet));
        }
        remote_sequence_ = sequence;
        last_incoming_time_ = std::chrono::steady_clock::now();
    });

    if (!udp_->Connect(udp_server_, udp_port_)) {
        ESP_LOGE(TAG, "UDP 音频通道连接失败，错误码=%d", udp_->GetLastError());
        udp_.reset();
        return false;
    }
    TaskHandle_t udp_receive_task = xTaskGetHandle("udp_receive");
    if (udp_receive_task != nullptr) {
        vTaskPrioritySet(udp_receive_task, 3);
    }

    if (on_audio_channel_opened_ != nullptr) {
        on_audio_channel_opened_();
    }
    return true;
}

/**
 * @brief 生成 MQTT 协议使用的客户端 hello JSON。
 * @return 无多余空白的客户端 hello JSON。
 * @details 消息申请 UDP 音频传输，并声明 MCP 能力、16 kHz 单声道 Opus 编码和帧时长；
 *          启用服务端 AEC 时额外声明 aec 特性。
 */
std::string MqttProtocol::GetHelloMessage() {
    return BuildXiaozhiHelloMessage(3, "udp");
}

/**
 * @brief 解析服务端 UDP 地址、端口、AES nonce 和会话参数。
 * @param root 服务端 hello JSON 根对象，仅在本次调用期间有效。
 * @details 校验传输类型后保存会话编号、Opus 参数和 UDP 配置，将十六进制密钥及 nonce 转换为
 *          原始字节并初始化 AES-CTR 上下文。全部参数就绪后设置事件位，唤醒 OpenAudioChannel()。
 */
void MqttProtocol::ParseServerHello(const cJSON* root) {
    XiaozhiServerHello hello;
    if (!ParseXiaozhiServerHello(root, "udp", hello)) {
        ESP_LOGE(TAG, "服务端 hello 的会话或音频参数无效");
        return;
    }

    session_id_ = std::move(hello.session_id);
    server_sample_rate_ = hello.sample_rate;
    server_frame_duration_ = hello.frame_duration;
    ESP_LOGI(TAG, "小智会话已建立，会话标识=%s", session_id_.c_str());

    auto udp = cJSON_GetObjectItem(root, "udp");
    if (!cJSON_IsObject(udp)) {
        ESP_LOGE(TAG, "服务端 hello 缺少 UDP 配置");
        return;
    }
    const cJSON* server = cJSON_GetObjectItemCaseSensitive(udp, "server");
    const cJSON* port = cJSON_GetObjectItemCaseSensitive(udp, "port");
    const cJSON* key = cJSON_GetObjectItemCaseSensitive(udp, "key");
    const cJSON* nonce = cJSON_GetObjectItemCaseSensitive(udp, "nonce");
    if (!cJSON_IsString(server)
        || !cJSON_IsNumber(port)
        || port->valueint <= 0
        || port->valueint > 65535
        || !cJSON_IsString(key)
        || !cJSON_IsString(nonce)) {
        ESP_LOGE(TAG, "服务端 hello 的 UDP 配置无效");
        return;
    }
    udp_server_ = server->valuestring;
    udp_port_ = port->valueint;

    // auto encryption = cJSON_GetObjectItem(udp, "encryption")->valuestring;
    // ESP_LOGI(TAG, "UDP server: %s, port: %d, encryption: %s", udp_server_.c_str(), udp_port_, encryption);
    const std::string decoded_key = DecodeHexString(key->valuestring);
    const std::string decoded_nonce = DecodeHexString(nonce->valuestring);
    if (decoded_key.size() != 16 || decoded_nonce.size() != 16) {
        ESP_LOGE(TAG, "服务端 hello 的 AES 密钥或 nonce 无效");
        return;
    }

    aes_nonce_ = decoded_nonce;
    // ESP-IDF 6.0 起用 PSA Crypto 导入 AES-128 密钥，替代已迁移走的 mbedtls_aes_setkey_enc。
    psa_key_attributes_t key_attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&key_attributes, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&key_attributes, PSA_ALG_CTR);
    psa_set_key_type(&key_attributes, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&key_attributes, 128);
    const psa_status_t psa_status = psa_import_key(
        &key_attributes,
        reinterpret_cast<const unsigned char*>(decoded_key.data()),
        decoded_key.size(),
        &aes_key_id_);
    if (psa_status != PSA_SUCCESS) {
        ESP_LOGE(TAG, "初始化 AES 密钥失败，错误码=%ld", static_cast<long>(psa_status));
        aes_key_id_ = 0;
        return;
    }
    local_sequence_ = 0;
    remote_sequence_ = 0;
    remote_sequence_gap_count_ = 0;
    remote_sequence_last_warning_us_ = 0;
    xEventGroupSetBits(event_group_handle_, MQTT_PROTOCOL_SERVER_HELLO_EVENT);
}

static const char hex_chars[] = "0123456789ABCDEF";

/**
 * @brief 将一个十六进制字符转换为 4 位数值。
 * @param c 字符 '0' 至 '9'、'A' 至 'F' 或 'a' 至 'f'。
 * @return 合法字符对应的 0 至 15；无效字符返回 0。
 */
static inline uint8_t CharToHex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return 0;  // 对无效输入返回 0；协议配置应保证密钥和 nonce 为合法十六进制文本。
}

/**
 * @brief 把偶数长度十六进制字符串转换为原始字节。
 * @param hex_string 由两个字符表示一个字节的十六进制文本，长度必须为偶数。
 * @return 转换后的二进制字符串，可包含零字节。
 * @details 每次读取高、低两个半字节并合并；调用者负责确保输入长度和字符内容合法。
 */
std::string MqttProtocol::DecodeHexString(const std::string& hex_string) {
    if (hex_string.size() % 2 != 0) {
        return {};
    }

    std::string decoded;
    decoded.reserve(hex_string.size() / 2);
    for (size_t i = 0; i < hex_string.size(); i += 2) {
        if (!std::isxdigit(static_cast<unsigned char>(hex_string[i]))
            || !std::isxdigit(static_cast<unsigned char>(hex_string[i + 1]))) {
            return {};
        }
        char byte = (CharToHex(hex_string[i]) << 4) | CharToHex(hex_string[i + 1]);
        decoded.push_back(byte);
    }
    return decoded;
}

/**
 * @brief UDP 对象和会话参数均有效时返回 true。
 * @return UDP 对象存在、协议未发生错误且最近通信未超时时返回 true。
 */
bool MqttProtocol::IsAudioChannelOpened() const {
    return udp_ != nullptr && !error_occurred_ && !IsTimeout();
}
