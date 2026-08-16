/**
 * @file ota.cc
 * @brief 版本检查、设备激活和 OTA 固件升级实现。
 */
#include "xiaozhi/provisioning/ota.h"
#include "system/system_info.h"
#include "system/settings.h"
#include "assets/lang_config.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cJSON.h>
#include <esp_log.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>
#include <esp_app_format.h>
#include <esp_efuse.h>
#include <esp_efuse_table.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#ifdef SOC_HMAC_SUPPORTED
#include <esp_hmac.h>
#endif

#include <cstring>
#include <vector>
#include <sstream>
#include <algorithm>

#define TAG "Ota"


/**
 * @brief 读取当前固件版本并初始化查询状态。
 * @details 在支持用户 eFuse 数据区的芯片上读取 32 字节设备序列号。序列号存在时，后续
 * 版本检查和激活请求会使用新版激活协议并携带 Serial-Number 请求头。
 */
Ota::Ota() {
#ifdef ESP_EFUSE_BLOCK_USR_DATA
    // 从 eFuse 用户数据区读取出厂烧录的设备序列号，内容为空时按无序列号设备处理。
    uint8_t serial_number[33] = {0};
    if (esp_efuse_read_field_blob(ESP_EFUSE_USER_DATA, serial_number, 32 * 8) == ESP_OK) {
        if (serial_number[0] == 0) {
            has_serial_number_ = false;
        } else {
            serial_number_ = std::string(reinterpret_cast<char*>(serial_number), 32);
            has_serial_number_ = true;
        }
    }
#endif
}

/**
 * @brief 释放查询过程中持有的资源。
 * @details 当前类只保存值类型状态和临时智能指针，无需额外手工释放资源。
 */
Ota::~Ota() {
}

/**
 * @brief 根据板型和配置生成版本检查服务 URL。
 * @return NVS 中配置的 ota_url；未配置时返回编译期 CONFIG_OTA_URL。
 * @details 运行时配置优先，便于测试环境或私有部署切换版本服务而无需重新编译固件。
 */
std::string Ota::GetCheckVersionUrl() {
    Settings settings("wifi", false);
    std::string url = settings.GetString("ota_url");
    if (url.empty()) {
        url = CONFIG_OTA_URL;
    }
    return url;
}

/**
 * @brief 创建带设备请求头和 TLS 配置的 HTTP 客户端。
 * @return 已设置激活版本、设备标识、语言和内容类型请求头的 HTTP 客户端。
 * @details HTTP 实现由当前板级网络对象创建，因此可自动适配 Wi-Fi 等网络承载。序列号存在时
 * 额外发送 Serial-Number，并把 Activation-Version 切换为 2。
 */
std::unique_ptr<Http> Ota::SetupHttp() {
    auto& board = Board::GetInstance();
    auto network = board.GetNetwork();
    auto http = network->CreateHttp(0);
    auto user_agent = SystemInfo::GetUserAgent();
    http->SetHeader("Activation-Version", has_serial_number_ ? "2" : "1");
    http->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    http->SetHeader("Client-Id", board.GetUuid());
    if (has_serial_number_) {
        http->SetHeader("Serial-Number", serial_number_.c_str());
        ESP_LOGI(TAG, "OTA HTTP 已配置，用户代理=%s，设备序列号=%s", user_agent.c_str(), serial_number_.c_str());
    }
    http->SetHeader("User-Agent", user_agent);
    http->SetHeader("Accept-Language", Lang::CODE);
    http->SetHeader("Content-Type", "application/json");

    return http;
}

/**
 * @brief 请求版本服务并解析固件、激活、协议配置和服务器时间。
 * @return 请求和 JSON 解析成功时返回 ESP_OK；URL、网络、HTTP 状态或响应格式异常时返回对应错误码。
 * @details 使用设备系统信息作为 POST 请求体。成功响应中的 MQTT/WebSocket 参数会写入 NVS，
 * 激活字段保存到对象供后续轮询，服务器时间用于校准系统时钟，固件字段用于判断是否升级。
 */
esp_err_t Ota::CheckVersion() {
    auto& board = Board::GetInstance();
    auto app_desc = esp_app_get_description();

    // 以当前运行镜像版本作为版本比较基准。
    current_version_ = app_desc->version;
    ESP_LOGI(TAG, "当前固件版本=%s", current_version_.c_str());

    std::string url = GetCheckVersionUrl();
    if (url.length() < 10) {
        ESP_LOGE(TAG, "版本检查地址配置无效");
        return ESP_ERR_INVALID_ARG;
    }

    auto http = SetupHttp();

    std::string data = board.GetSystemInfoJson();
    std::string method = data.length() > 0 ? "POST" : "GET";
    http->SetContent(std::move(data));

    if (!http->Open(method, url)) {
        int last_error = http->GetLastError();
        ESP_LOGE(TAG, "打开版本检查 HTTP 连接失败，错误码=0x%x", last_error);
        return last_error;
    }

    auto status_code = http->GetStatusCode();
    if (status_code != 200) {
        ESP_LOGE(TAG, "检查固件版本失败，HTTP 状态码=%d", status_code);
        return status_code;
    }

    data = http->ReadAll();
    http->Close();

    // 解析响应中的激活信息、协议配置、服务器时间和固件版本；字段缺失时保留对应“不可用”状态。
    
    cJSON *root = cJSON_Parse(data.c_str());
    if (root == NULL) {
        ESP_LOGE(TAG, "解析版本检查 JSON 响应失败");
        return ESP_ERR_INVALID_RESPONSE;
    }

    has_activation_code_ = false;
    has_activation_challenge_ = false;
    cJSON *activation = cJSON_GetObjectItem(root, "activation");
    if (cJSON_IsObject(activation)) {
        cJSON* message = cJSON_GetObjectItem(activation, "message");
        if (cJSON_IsString(message)) {
            activation_message_ = message->valuestring;
        }
        cJSON* code = cJSON_GetObjectItem(activation, "code");
        if (cJSON_IsString(code)) {
            activation_code_ = code->valuestring;
            has_activation_code_ = true;
        }
        cJSON* challenge = cJSON_GetObjectItem(activation, "challenge");
        if (cJSON_IsString(challenge)) {
            activation_challenge_ = challenge->valuestring;
            has_activation_challenge_ = true;
        }
        cJSON* timeout_ms = cJSON_GetObjectItem(activation, "timeout_ms");
        if (cJSON_IsNumber(timeout_ms)) {
            activation_timeout_ms_ = timeout_ms->valueint;
        }
    }

    has_mqtt_config_ = false;
    cJSON *mqtt = cJSON_GetObjectItem(root, "mqtt");
    if (cJSON_IsObject(mqtt)) {
        Settings settings("mqtt", true);
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, mqtt) {
            if (cJSON_IsString(item)) {
                if (settings.GetString(item->string) != item->valuestring) {
                    settings.SetString(item->string, item->valuestring);
                }
            } else if (cJSON_IsNumber(item)) {
                if (settings.GetInt(item->string) != item->valueint) {
                    settings.SetInt(item->string, item->valueint);
                }
            }
        }
        has_mqtt_config_ = true;
    } else {
        ESP_LOGI(TAG, "版本响应中未包含 MQTT 配置");
    }

    has_websocket_config_ = false;
    cJSON *websocket = cJSON_GetObjectItem(root, "websocket");
    if (cJSON_IsObject(websocket)) {
        Settings settings("websocket", true);
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, websocket) {
            if (cJSON_IsString(item)) {
                if (settings.GetString(item->string) != item->valuestring) {
                    settings.SetString(item->string, item->valuestring);
                }
            } else if (cJSON_IsNumber(item)) {
                if (settings.GetInt(item->string) != item->valueint) {
                    settings.SetInt(item->string, item->valueint);
                }
            }
        }
        has_websocket_config_ = true;
    } else {
        ESP_LOGI(TAG, "版本响应中未包含 WebSocket 配置");
    }

    has_server_time_ = false;
    cJSON *server_time = cJSON_GetObjectItem(root, "server_time");
    if (cJSON_IsObject(server_time)) {
        cJSON *timestamp = cJSON_GetObjectItem(server_time, "timestamp");
        cJSON *timezone_offset = cJSON_GetObjectItem(server_time, "timezone_offset");
        
        if (cJSON_IsNumber(timestamp)) {
            // 设置系统时间
            struct timeval tv;
            double ts = timestamp->valuedouble;
            
            // 如果有时区偏移，计算本地时间
            if (cJSON_IsNumber(timezone_offset)) {
                ts += (timezone_offset->valueint * 60 * 1000); // 转换分钟为毫秒
            }
            
            tv.tv_sec = (time_t)(ts / 1000);  // 转换毫秒为秒
            tv.tv_usec = (suseconds_t)((long long)ts % 1000) * 1000;  // 剩余的毫秒转换为微秒
            settimeofday(&tv, NULL);
            has_server_time_ = true;
        }
    } else {
        ESP_LOGW(TAG, "版本响应中未包含服务端时间");
    }

    has_new_version_ = false;
    cJSON *firmware = cJSON_GetObjectItem(root, "firmware");
    if (cJSON_IsObject(firmware)) {
        cJSON *version = cJSON_GetObjectItem(firmware, "version");
        if (cJSON_IsString(version)) {
            firmware_version_ = version->valuestring;
        }
        cJSON *url = cJSON_GetObjectItem(firmware, "url");
        if (cJSON_IsString(url)) {
            firmware_url_ = url->valuestring;
        }

        if (cJSON_IsString(version) && cJSON_IsString(url)) {
            // 按数字段比较版本，例如 0.1.0 高于 0.0.1。
            has_new_version_ = IsNewVersionAvailable(current_version_, firmware_version_);
            if (has_new_version_) {
                ESP_LOGI(TAG, "发现新固件版本=%s", firmware_version_.c_str());
            } else {
                ESP_LOGI(TAG, "当前已是最新固件版本");
            }
            // force=1 时忽略版本比较结果，强制安装服务端指定镜像。
            cJSON *force = cJSON_GetObjectItem(firmware, "force");
            if (cJSON_IsNumber(force) && force->valueint == 1) {
                has_new_version_ = true;
            }
        }
    } else {
        ESP_LOGW(TAG, "版本响应中未包含固件信息");
    }

    cJSON_Delete(root);
    return ESP_OK;
}

/**
 * @brief 将当前运行镜像标记为有效，取消 bootloader 回滚。
 * @details 工厂分区无需确认；仅当当前 OTA 分区处于 ESP_OTA_IMG_PENDING_VERIFY 状态时调用
 * ESP-IDF 接口确认镜像，其他状态保持不变。
 */
void Ota::MarkCurrentVersionValid() {
    auto partition = esp_ota_get_running_partition();
    if (strcmp(partition->label, "factory") == 0) {
        ESP_LOGI(TAG, "当前从 factory 分区运行，跳过 OTA 分区确认");
        return;
    }

    ESP_LOGI(TAG, "当前运行分区=%s", partition->label);
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(partition, &state) != ESP_OK) {
        ESP_LOGE(TAG, "获取当前 OTA 分区状态失败");
        return;
    }

    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "正在将当前固件标记为有效");
        esp_ota_mark_app_valid_cancel_rollback();
    }
}

/**
 * @brief 从任意 URL 执行 OTA 下载、校验和分区切换。
 * @param firmware_url 固件镜像的 HTTP/HTTPS 地址。
 * @param callback 下载进度和速度回调。
 * @return 写入并设置新启动分区成功时返回 true。
 * @details 固件按 4 KiB 页流式下载到内部 RAM，读取到完整应用头后启动顺序写入。每秒回调一次
 * 下载百分比和速度；结束后由 ESP-IDF 校验镜像，并仅在校验成功时切换下次启动分区。
 * 任一读取、写入或校验步骤失败都会释放缓冲区并中止 OTA 句柄。
 */
bool Ota::Upgrade(const std::string& firmware_url, std::function<void(int progress, size_t speed)> callback) {
    ESP_LOGI(TAG, "正在从指定地址升级固件，地址=%s", firmware_url.c_str());
    esp_ota_handle_t update_handle = 0;
    auto update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "获取 OTA 更新分区失败");
        return false;
    }

    ESP_LOGI(TAG, "正在写入 OTA 分区，分区=%s，偏移=0x%lx", update_partition->label, update_partition->address);
    bool image_header_checked = false;
    std::string image_header;

    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(0);
    if (!http->Open("GET", firmware_url)) {
        ESP_LOGE(TAG, "打开固件下载 HTTP 连接失败");
        return false;
    }

    if (http->GetStatusCode() != 200) {
        ESP_LOGE(TAG, "下载固件失败，HTTP 状态码=%d", http->GetStatusCode());
        return false;
    }

    size_t content_length = http->GetBodyLength();
    if (content_length == 0) {
        ESP_LOGE(TAG, "获取固件内容长度失败");
        return false;
    }

    constexpr size_t PAGE_SIZE = 4096;
    char* buffer = (char*)heap_caps_malloc(PAGE_SIZE, MALLOC_CAP_INTERNAL);
    if (buffer == nullptr) {
        ESP_LOGE(TAG, "分配固件下载缓冲区失败");
        return false;
    }

    size_t buffer_offset = 0;  // 当前 4 KiB 页缓冲区内尚未写入 Flash 的字节数。
    size_t total_read = 0, recent_read = 0;
    auto last_calc_time = esp_timer_get_time();
    while (true) {
        int ret = http->Read(buffer + buffer_offset, PAGE_SIZE - buffer_offset);
        if (ret < 0) {
            ESP_LOGE(TAG, "读取固件 HTTP 数据失败，错误=%s", esp_err_to_name(ret));
            heap_caps_free(buffer);
            return false;
        }

        // 每秒或下载结束时统计一次进度和最近一秒的接收速度。
        recent_read += ret;
        total_read += ret;
        buffer_offset += ret;
        if (esp_timer_get_time() - last_calc_time >= 1000000 || ret == 0) {
            size_t progress = total_read * 100 / content_length;
            ESP_LOGI(TAG, "固件下载进度=%u%%（%u/%u），速度=%uB/s", progress, total_read, content_length, recent_read);
            if (callback) {
                callback(progress, recent_read);
            }
            last_calc_time = esp_timer_get_time();
            recent_read = 0;
        }

        if (!image_header_checked) {
            image_header.append(buffer, buffer_offset);
            if (image_header.size() >= sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t)) {
                esp_app_desc_t new_app_info;
                memcpy(&new_app_info, image_header.data() + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t), sizeof(esp_app_desc_t));

                if (esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &update_handle)) {
                    esp_ota_abort(update_handle);
                    ESP_LOGE(TAG, "启动 OTA 写入失败");
                    heap_caps_free(buffer);
                    return false;
                }

                image_header_checked = true;
                std::string().swap(image_header);
            }
        }

        // 缓冲区满 4 KiB 或收到最后一块数据时再写 Flash，减少零碎写入。
        bool is_last_chunk = (ret == 0);
        if (buffer_offset == PAGE_SIZE || (is_last_chunk && buffer_offset > 0)) {
            auto err = esp_ota_write(update_handle, buffer, buffer_offset);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "写入 OTA 数据失败，错误=%s", esp_err_to_name(err));
                esp_ota_abort(update_handle);
                heap_caps_free(buffer);
                return false;
            }

            buffer_offset = 0;
        }

        if (is_last_chunk) {
            break;
        }
    }
    http->Close();
    heap_caps_free(buffer);

    esp_err_t err = esp_ota_end(update_handle);
    if (err != ESP_OK) {
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG, "固件镜像校验失败，镜像可能已损坏");
        } else {
            ESP_LOGE(TAG, "结束 OTA 写入失败，错误=%s", esp_err_to_name(err));
        }
        return false;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "设置启动分区失败，错误=%s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "固件升级成功");
    return true;
}

/**
 * @brief 使用 CheckVersion() 得到的固件地址开始升级。
 * @param callback 进度回调；progress 为 0-100，speed 为每秒下载字节数。
 * @return Upgrade() 完成镜像写入和启动分区切换时返回 true，否则返回 false。
 * @details 本方法不重新检查版本，只使用最近一次 CheckVersion() 保存的 firmware_url_。
 */
bool Ota::StartUpgrade(std::function<void(int progress, size_t speed)> callback) {
    return Upgrade(firmware_url_, callback);
}


/**
 * @brief 把点分版本号转换为整数序列。
 * @param version 例如 2.2.6。
 * @return 按顺序保存每个数字段的整数数组，例如 2.2.6 转换为 {2, 2, 6}。
 * @details 输入段由 std::stoi() 解析，因此调用者应提供只包含数字和点号的合法版本字符串。
 */
std::vector<int> Ota::ParseVersion(const std::string& version) {
    std::vector<int> versionNumbers;
    std::stringstream ss(version);
    std::string segment;
    
    while (std::getline(ss, segment, '.')) {
        versionNumbers.push_back(std::stoi(segment));
    }
    
    return versionNumbers;
}

/**
 * @brief 按数字段比较版本。
 * @param currentVersion 当前运行版本。
 * @param newVersion 服务端提供的候选版本。
 * @return newVersion 更新时返回 true。
 * @details 从左到右比较各数字段，首次出现不同值时即可确定新旧关系；公共前缀相同时，
 * 字段更多的候选版本被视为更新版本。
 */
bool Ota::IsNewVersionAvailable(const std::string& currentVersion, const std::string& newVersion) {
    std::vector<int> current = ParseVersion(currentVersion);
    std::vector<int> newer = ParseVersion(newVersion);
    
    for (size_t i = 0; i < std::min(current.size(), newer.size()); ++i) {
        if (newer[i] > current[i]) {
            return true;
        } else if (newer[i] < current[i]) {
            return false;
        }
    }
    
    return newer.size() > current.size();
}

/**
 * @brief 构造包含序列号、challenge 和 HMAC 签名的激活请求 JSON。
 * @return 有序列号时返回包含算法、序列号、challenge 和 HMAC 的 JSON；否则返回空对象文本 "{}"。
 * @details 在芯片支持 HMAC 外设时，使用受保护的 HMAC_KEY0 对服务端 challenge 计算 SHA-256
 * 签名，密钥不会离开硬件。结果转换为小写十六进制后加入激活请求。
 */
std::string Ota::GetActivationPayload() {
    if (!has_serial_number_) {
        return "{}";
    }

    std::string hmac_hex;
#ifdef SOC_HMAC_SUPPORTED
    uint8_t hmac_result[32]; // SHA-256 输出为32字节
    
    // 使用Key0计算HMAC
    esp_err_t ret = esp_hmac_calculate(HMAC_KEY0, (uint8_t*)activation_challenge_.data(), activation_challenge_.size(), hmac_result);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "计算激活 HMAC 失败，错误=%s", esp_err_to_name(ret));
        return "{}";
    }

    for (size_t i = 0; i < sizeof(hmac_result); i++) {
        char buffer[3];
        sprintf(buffer, "%02x", hmac_result[i]);
        hmac_hex += buffer;
    }
#endif

    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "algorithm", "hmac-sha256");
    cJSON_AddStringToObject(payload, "serial_number", serial_number_.c_str());
    cJSON_AddStringToObject(payload, "challenge", activation_challenge_.c_str());
    cJSON_AddStringToObject(payload, "hmac", hmac_hex.c_str());
    auto json_str = cJSON_PrintUnformatted(payload);
    std::string json(json_str);
    cJSON_free(json_str);
    cJSON_Delete(payload);

    ESP_LOGI(TAG, "设备激活请求内容=%s", json.c_str());
    return json;
}

/**
 * @brief 使用服务端 challenge 完成设备激活。
 * @return 激活完成返回 ESP_OK；服务端返回 202 表示仍在处理中并返回 ESP_ERR_TIMEOUT；其他失败返回 ESP_FAIL。
 * @details 在版本服务 URL 后追加 activate 路径，提交 GetActivationPayload() 生成的 JSON。
 * 该方法只执行一次轮询请求，重试间隔和次数由 Application::CheckNewVersion() 控制。
 */
esp_err_t Ota::Activate() {
    if (!has_activation_challenge_) {
        ESP_LOGW(TAG, "激活响应中未包含挑战值");
        return ESP_FAIL;
    }

    std::string url = GetCheckVersionUrl();
    if (url.back() != '/') {
        url += "/activate";
    } else {
        url += "activate";
    }

    auto http = SetupHttp();

    std::string data = GetActivationPayload();
    http->SetContent(std::move(data));

    if (!http->Open("POST", url)) {
        ESP_LOGE(TAG, "打开设备激活 HTTP 连接失败");
        return ESP_FAIL;
    }
    
    auto status_code = http->GetStatusCode();
    if (status_code == 202) {
        return ESP_ERR_TIMEOUT;
    }
    if (status_code != 200) {
        ESP_LOGE(TAG, "设备激活失败，HTTP 状态码=%d，响应=%s", status_code, http->ReadAll().c_str());
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "设备激活成功");
    return ESP_OK;
}
