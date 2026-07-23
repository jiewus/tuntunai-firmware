/**
 * @file application_lifecycle.cc
 * @brief 固件升级、重启、休眠和 MCP 外部入口实现。
 */
#include "app/application.h"
#include "board.h"
#include "display.h"
#include "system/system_info.h"
#include "audio_codec.h"
#include "assets/lang_config.h"
#include "mcp/mcp_server.h"
#include "tuntun/backend/backend_service.h"
#include "assets.h"
#include "system/settings.h"

#include <esp_log.h>
#include <driver/gpio.h>
#include <arpa/inet.h>
#include <font_awesome.h>

#define TAG "Application"

/**
 * @brief 等待必要数据落盘后重启 ESP32。
 * @details 先关闭音频通道、释放协议对象并停止音频后台任务，再等待一秒让网络和存储操作收尾，
 *          最后调用 esp_restart() 执行芯片复位。
 */
void Application::Reboot() {
    ESP_LOGI(TAG, "设备即将重启");
    // 重启前主动关闭音频会话，避免服务端继续保留设备在线状态。
    if (xiaozhi_client_ && xiaozhi_client_->IsAudioChannelOpened()) {
        xiaozhi_client_->CloseAudioChannel();
    }
    xiaozhi_client_.reset();
    audio_service_.Stop();

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

/**
 * @brief 从指定地址下载并安装固件。
 * @param url 固件二进制的 HTTP/HTTPS 地址。
 * @param version 用于界面显示的目标版本号，可为空。
 * @return 升级流程启动并完成成功时返回 true。
 * @details 关闭云端音频、播放升级提示、切换高性能模式并停止音频任务，再调用 Ota::Upgrade()。
 *          失败时恢复音频与低功耗；成功时显示结果并立即重启到新分区。
 */
bool Application::UpgradeFirmware(const std::string& url, const std::string& version) {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();

    std::string upgrade_url = url;
    std::string version_info = version.empty() ? "(Manual upgrade)" : version;

    // 写入固件前关闭云端音频，避免升级期间仍有网络和 Codec 活动。
    if (xiaozhi_client_ && xiaozhi_client_->IsAudioChannelOpened()) {
        ESP_LOGI(TAG, "固件升级前正在关闭音频通道");
        xiaozhi_client_->CloseAudioChannel();
    }
    ESP_LOGI(TAG, "开始从指定地址升级固件，地址=%s", upgrade_url.c_str());

    Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "download", Lang::Sounds::OGG_UPGRADE);
    vTaskDelay(pdMS_TO_TICKS(3000));

    SetDeviceState(kDeviceStateUpgrading);

    std::string message = std::string(Lang::Strings::NEW_VERSION) + version_info;
    display->SetChatMessage("system", message.c_str());

    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    audio_service_.Stop();
    vTaskDelay(pdMS_TO_TICKS(1000));

    bool upgrade_success = Ota::Upgrade(upgrade_url, [this, display](int progress, size_t speed) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
        Schedule([display, message = std::string(buffer)]() {
            display->SetChatMessage("system", message.c_str());
        });
    });

    if (!upgrade_success) {
        // 升级失败时恢复音频服务和低功耗，继续运行当前固件。
        ESP_LOGE(TAG, "固件升级失败，正在恢复音频服务并继续运行");
        audio_service_.Start(); // 重新创建并启动音频后台任务。
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER); // 恢复设备空闲时的功耗策略。
        Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        vTaskDelay(pdMS_TO_TICKS(3000));
        return false;
    } else {
        // 新镜像已校验并设置为启动分区，立即重启完成切换。
        ESP_LOGI(TAG, "固件升级成功，设备即将重启");
        display->SetChatMessage("system", "Upgrade successful, rebooting...");
        vTaskDelay(pdMS_TO_TICKS(1000)); // 短暂停留，让用户看到升级成功提示。
        Reboot();
        return true;
    }
}

/**
 * @brief 处理本地唤醒词触发。
 * @param wake_word 被识别出的唤醒词文本。
 * @details 这是供外部模块直接触发的兼容入口：空闲时建立会话，播报时调度打断，监听时调度关闭通道。
 */
void Application::WakeWordInvoke(const std::string& wake_word) {
    // 兼容入口同样属于明确的用户唤醒，必须允许退出当前屏保。
    Board::GetInstance().WakeUpScreen(true);

    if (!xiaozhi_client_) {
        return;
    }

    auto state = GetDeviceState();

    if (state == kDeviceStateIdle) {
        if (!xiaozhi_client_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // 先显示连接状态，再执行可能阻塞的网络握手。
            Schedule([this, wake_word]() {
                ContinueWakeWordInvoke(wake_word);
            });
            return;
        }
        // 通道已存在时直接继续唤醒流程，无需再次握手。
        ContinueWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
        });
    } else if (state == kDeviceStateListening) {
        Schedule([this]() {
            if (xiaozhi_client_) {
                xiaozhi_client_->CloseAudioChannel();
            }
        });
    }
}

/**
 * @brief 判断当前状态是否允许进入省电模式。
 * @return 设备空闲、云端音频通道关闭且所有音频队列为空时返回 true。
 * @details 任一会话或音频任务仍在运行都拒绝休眠，防止丢失待发送或待播放数据。
 */
bool Application::CanEnterSleepMode() {
    if (GetDeviceState() != kDeviceStateIdle) {
        return false;
    }

    if (xiaozhi_client_ && xiaozhi_client_->IsAudioChannelOpened()) {
        return false;
    }

    if (!audio_service_.IsIdle()) {
        return false;
    }

    // 状态、网络会话和音频流水线均已空闲，此时可以进入休眠。
    return true;
}

/**
 * @brief 注册 MCP 广播发送器。
 * @param callback 接收待广播 JSON 文本的回调。
 * @details 新回调会替换旧回调，用于把 MCP 应答同步发送到调试或本地控制通道。
 */
void Application::RegisterMcpBroadcastCallback(std::function<void(const std::string&)> callback) {
    mcp_broadcast_callback_ = std::move(callback);
}

/**
 * @brief 通过当前云端协议发送 MCP JSON 消息。
 * @param payload 完整 JSON 文本。
 * @details 无论调用线程来自网络还是工具任务，都先调度到应用主线程，再分别发送到云端协议和已注册
 *          的本地广播回调，保证协议对象生命周期访问串行化。
 */
void Application::SendMcpMessage(const std::string& payload) {
    // 始终切换到应用主线程，避免与断网释放协议对象的流程并发。
    Schedule([this, payload](){
        if (xiaozhi_client_) {
            xiaozhi_client_->SendMcpMessage(payload);
        }
        if (mcp_broadcast_callback_) {
            mcp_broadcast_callback_(payload);
        }
    });
}

/**
 * @brief 异步播放一段内嵌 OGG 音频。
 * @param sound 指向完整 OGG 数据的视图。
 * @details 直接交给 AudioService 解封装并加入播放队列，调用本身不等待扬声器播放完成。
 */
void Application::PlaySound(const std::string_view& sound) {
    audio_service_.PlaySound(sound);
}

/**
 * @brief 线程安全地释放协议和 OTA 资源。
 *
 * 会关闭音频通道并销毁联网后创建的对象。可从任意任务调用，但真正的资源
 * 生命周期变更会与主任务同步，防止断网回调与协议收包同时访问悬空对象。
 * @details 实际释放动作始终通过 Schedule() 在主线程执行；先关闭活动音频通道，再销毁协议对象，
 *          使断线回调和收包回调不会与资源释放并发。
 */
void Application::ResetProtocol() {
    BackendService::GetInstance().OnMcpDisconnected();
    Schedule([this]() {
        // 先关闭活动音频通道，让协议层触发正常的会话结束回调。
        if (xiaozhi_client_ && xiaozhi_client_->IsAudioChannelOpened()) {
            xiaozhi_client_->CloseAudioChannel();
        }
        // 通道关闭后再释放协议对象及其网络资源。
        xiaozhi_client_.reset();
    });
}
