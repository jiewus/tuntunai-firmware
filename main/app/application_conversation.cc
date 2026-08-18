/**
 * @file application_conversation.cc
 * @brief 小智语音会话、交互事件和状态界面处理实现。
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

#include <cctype>
#include <esp_log.h>
#include <driver/gpio.h>
#include <arpa/inet.h>
#include <font_awesome.h>

#define TAG "Application"

namespace {

/**
 * @brief 判断云端 TTS 字幕是否为不应展示给用户的内部工具调用标记。
 * @param text 云端 `tts.sentence_start.text` 字段，可为空指针。
 * @return 去除前导空白后，文本以 `%` 开头且后面紧跟 ASCII 接口标识符时返回 true；
 * 普通自然语言或正文中间包含百分号时返回 false。
 * @details 部分云端模型会把 `% getWeather...`、`%self.tool(...)` 一类内部调用过程
 * 错误地作为 TTS 字幕下发。该判断只识别位于整段文本开头的工具标记，避免把
 * “降水概率 30%”等正常回答误判为工具调用。
 */
bool IsInternalToolCallText(const char* text) {
    if (text == nullptr) {
        return false;
    }

    const unsigned char* current = reinterpret_cast<const unsigned char*>(text);
    while (*current != '\0' && std::isspace(*current)) {
        ++current;
    }
    if (*current != '%') {
        return false;
    }

    ++current;
    while (*current != '\0' && std::isspace(*current)) {
        ++current;
    }

    return std::isalpha(*current) || *current == '_';
}

}  // 匿名命名空间

/**
 * @brief 根据 OTA 配置创建并配置云端通信协议。
 * @details 优先使用服务器下发的 MQTT 或 WebSocket 配置，未指定时回退到 MQTT。随后注册连接、
 * 错误、音频、JSON 控制消息和 MCP 广播回调，使协议线程只产生事件或调度主线程任务，
 * 避免直接跨线程修改显示与设备状态。
 */
void Application::InitializeXiaozhiClient() {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto codec = board.GetAudioCodec();

    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

    xiaozhi_client_ = std::make_unique<XiaozhiClient>();
    XiaozhiClientCallbacks callbacks;
    callbacks.on_connected = [this]() {
        DismissAlert();
    };
    callbacks.on_network_error = [this](const std::string& message) {
        last_error_message_ = message;
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
    };
    callbacks.on_incoming_audio = [this](std::unique_ptr<AudioStreamPacket> packet) {
        if (GetDeviceState() == kDeviceStateSpeaking) {
            audio_service_.PushPacketToDecodeQueue(std::move(packet));
        }
    };
    callbacks.on_audio_channel_opened = [this, codec]() {
        Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        if (xiaozhi_client_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG, "服务端采样率与设备输出采样率不一致，服务端=%d，设备=%d",
                xiaozhi_client_->server_sample_rate(), codec->output_sample_rate());
        }
    };
    callbacks.on_audio_channel_closed = [this]() {
        Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        Schedule([this]() {
            auto display = Board::GetInstance().GetDisplay();
            display->SetChatMessage("system", "");
            SetDeviceState(kDeviceStateIdle);
        });
    };
    callbacks.on_tts_started = [this]() {
        Schedule([this]() {
            aborted_ = false;
            SetDeviceState(kDeviceStateSpeaking);
        });
    };
    callbacks.on_tts_stopped = [this]() {
        Schedule([this]() {
            if (end_conversation_after_speaking_.exchange(false)) {
                audio_service_.WaitForPlaybackQueueEmpty();
                if (xiaozhi_client_ && xiaozhi_client_->IsAudioChannelOpened()) {
                    xiaozhi_client_->CloseAudioChannel();
                }
                SetDeviceState(kDeviceStateIdle);
                ESP_LOGI(TAG, "自定义 MCP 回复播报完成，已结束会话并进入屏保");
                return;
            }
            if (GetDeviceState() == kDeviceStateSpeaking) {
                if (listening_mode_ == kListeningModeManualStop) {
                    SetDeviceState(kDeviceStateIdle);
                } else {
                    SetDeviceState(kDeviceStateListening);
                }
            }
        });
    };
    callbacks.on_tts_sentence = [this, display](const std::string& text) {
        ESP_LOGI(TAG, "<< %s", text.c_str());
        if (IsInternalToolCallText(text.c_str())) {
            ESP_LOGD(TAG, "已过滤内部工具调用字幕");
            return;
        }
        Schedule([this, display, text]() {
            if (!aborted_ && GetDeviceState() == kDeviceStateSpeaking) {
                display->SetChatMessage("assistant", text.c_str());
            }
        });
    };
    callbacks.on_stt_text = [this, display](const std::string& text) {
        ESP_LOGI(TAG, ">> %s", text.c_str());
        Schedule([display, text]() {
            display->SetChatMessage("user", text.c_str());
        });
    };
    callbacks.on_llm_emotion = [this, display](const std::string& emotion) {
        Schedule([display, emotion]() {
            display->SetEmotion(emotion.c_str());
        });
    };
    callbacks.on_mcp_message = [](const std::string& payload) {
        if (!payload.empty()) {
            McpServer::GetInstance().ParseMessage(payload);
        }
    };
    callbacks.on_reboot_requested = [this]() {
        Schedule([this]() {
            Reboot();
        });
    };
    callbacks.on_alert = [this](
        const std::string& status,
        const std::string& message,
        const std::string& emotion) {
        Alert(
            status.c_str(),
            message.c_str(),
            emotion.c_str(),
            Lang::Sounds::OGG_VIBRATION);
    };
#if CONFIG_RECEIVE_CUSTOM_MESSAGE
    callbacks.on_custom_message = [this, display](const std::string& payload) {
        ESP_LOGD(TAG, "收到自定义消息，负载字节数=%u",
                 static_cast<unsigned>(payload.size()));
        Schedule([display, payload]() {
            display->SetChatMessage("system", payload.c_str());
        });
    };
#endif
    xiaozhi_client_->SetCallbacks(std::move(callbacks));
    xiaozhi_client_->Start(
        ota_->HasMqttConfig(),
        ota_->HasWebsocketConfig());
}

/**
 * @brief 执行 ShowActivationCode 对应的模块内部流程。
 * @details 实现会维护 Application 的内部一致性；发生错误时记录日志，并避免向后续流程传播无效资源。
 */
void Application::ShowActivationCode(const std::string& code, const std::string& message) {
    struct digit_sound {
        char digit;
        const std::string_view& sound;
    };
    static const std::array<digit_sound, 10> digit_sounds{{
        digit_sound{'0', Lang::Sounds::OGG_0},
        digit_sound{'1', Lang::Sounds::OGG_1},
        digit_sound{'2', Lang::Sounds::OGG_2},
        digit_sound{'3', Lang::Sounds::OGG_3},
        digit_sound{'4', Lang::Sounds::OGG_4},
        digit_sound{'5', Lang::Sounds::OGG_5},
        digit_sound{'6', Lang::Sounds::OGG_6},
        digit_sound{'7', Lang::Sounds::OGG_7},
        digit_sound{'8', Lang::Sounds::OGG_8},
        digit_sound{'9', Lang::Sounds::OGG_9}
    }};

    // 数字播报单句约占用 9 KiB SRAM，因此逐个等待前一段音频处理完成。
    Alert(Lang::Strings::ACTIVATION, message.c_str(), "link", Lang::Sounds::OGG_ACTIVATION);

    for (const auto& digit : code) {
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
            [digit](const digit_sound& ds) { return ds.digit == digit; });
        if (it != digit_sounds.end()) {
            audio_service_.PlaySound(it->sound);
        }
    }
}

/**
 * @brief 显示一条告警并按需播放提示音。
 * @param status 状态栏文字，必须在调用期间有效。
 * @param message 告警正文。
 * @param emotion 表情资源名称，空字符串表示不改变表情。
 * @param sound 内嵌 OGG 数据视图，空视图表示静默告警。
 * @details 同步更新状态栏、表情和系统消息；sound 非空时把提示音交给音频服务异步解码播放。
 */
void Application::Alert(const char* status, const char* message, const char* emotion, const std::string_view& sound) {
    ESP_LOGW(TAG, "设备告警，表情=%s，状态=%s", emotion, status);
    auto& board = Board::GetInstance();
    board.WakeUpScreen();
    auto display = board.GetDisplay();
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
    if (!sound.empty()) {
        audio_service_.PlaySound(sound);
    }
}

/**
 * @brief 关闭当前告警并恢复与设备状态对应的正常界面。
 * @details 仅在设备已回到空闲状态时恢复待机文字、中性表情并清空系统消息，避免覆盖连接或升级界面。
 */
void Application::DismissAlert() {
    if (GetDeviceState() == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetEmotion("neutral");
        display->SetChatMessage("system", "");
    }
}

/**
 * @brief 在线程安全的事件方式下切换对话状态。
 *
 * 空闲时开始聆听，聆听或播报时执行停止/中断。这里只设置事件位，实际操作
 * 由 Run() 中的 HandleToggleChatEvent() 完成。
 * @details 通过事件组把操作串行化到 Run() 主循环，调用线程不会直接改变协议或音频状态。
 */
void Application::ToggleChatState() {
    Board::GetInstance().WakeUpScreen(true);
    xEventGroupSetBits(event_group_, MAIN_EVENT_TOGGLE_CHAT);
}

/**
 * @brief 请求设备按指定停止策略开始录音和上传语音。
 * @param mode 本轮监听使用的停止策略。
 * @note 方法本身不直接操作音频硬件，可从按键回调等其他任务安全调用。
 * @details 先原子保存停止策略，再设置 MAIN_EVENT_START_LISTENING 事件位；实际状态判断和通道连接由
 * 主循环完成，避免后台任务直接操作协议和音频状态。
 */
void Application::StartListening(ListeningMode mode) {
    requested_listening_mode_.store(mode);
    Board::GetInstance().WakeUpScreen(true);
    xEventGroupSetBits(event_group_, MAIN_EVENT_START_LISTENING);
}

/**
 * @brief 请求设备停止录音并通知云端本轮输入结束。
 * @details 只设置 MAIN_EVENT_STOP_LISTENING 事件位，可从按键或其他任务安全调用；主循环会停止正在监听
 * 的会话，或取消尚在建立音频通道的监听请求。
 */
void Application::StopListening() {
    Board::GetInstance().WakeUpScreen(true);
    xEventGroupSetBits(event_group_, MAIN_EVENT_STOP_LISTENING);
}

/**
 * @brief 结束当前云端语音会话，为设备本地音频播放释放麦克风和扬声器。
 * @details 关闭动作通过主任务队列延迟执行，使 MCP 工具结果先发送给云端；执行时停止上行语音处理、
 * 清除可能已经到达的云端 TTS，再关闭音频通道并直接回到空闲状态。本次状态切换不进入屏保，
 * 避免通知页面显示前出现表盘闪烁。
 */
void Application::EndCurrentConversationForLocalPlayback() {
    Schedule([this]() {
        audio_service_.EnableVoiceProcessing(false);
        audio_service_.ResetDecoder();
        if (xiaozhi_client_ && xiaozhi_client_->IsAudioChannelOpened()) {
            xiaozhi_client_->CloseAudioChannel();
        }
        SetDeviceState(kDeviceStateIdle);
        enter_screensaver_after_conversation_.store(false);
        ESP_LOGI(TAG, "确认会话已结束，准备直接播放本地通知");
    });
}

/**
 * @brief 结束当前云端会话并立即进入屏保页面。
 * @details 该方法用于绑定码过期等后台终态。它会在应用主循环中停止语音处理、清空解码器、关闭音频
 * 通道并切换到空闲状态，即使当前已经是空闲状态也会直接显示屏保，避免继续等待普通超时。
 * 若收尾排队期间用户已发起新会话，则放弃强制收尾，只确保不把屏保盖回对话界面。
 */
void Application::EndCurrentConversationAndEnterScreensaver() {
    Schedule([this]() {
        // 先读取状态：若用户已在排队期间唤醒开始新对话，则不再强制结束新会话。
        const DeviceState current_state = GetDeviceState();
        if (current_state != kDeviceStateIdle) {
            enter_screensaver_after_conversation_.store(false);
            end_conversation_after_speaking_.store(false);
            Board::GetInstance().WakeUpScreen(true);
            ESP_LOGI(TAG, "绑定收尾时检测到进行中的会话(state=%d)，跳过强制结束并退出屏保",
                     static_cast<int>(current_state));
            return;
        }

        audio_service_.EnableVoiceProcessing(false);
        audio_service_.EnableWakeWordDetection(true);
        audio_service_.ResetDecoder();
        end_conversation_after_speaking_.store(false);
        if (xiaozhi_client_ && xiaozhi_client_->IsAudioChannelOpened()) {
            xiaozhi_client_->CloseAudioChannel();
        }
        SetDeviceState(kDeviceStateIdle);
        enter_screensaver_after_conversation_.store(false);
        Board::GetInstance().EnterScreensaver();
        ESP_LOGI(TAG, "绑定流程已结束，设备立即进入屏保");
    });
}

/**
 * @brief 请求在当前云端回复播报完整结束后关闭会话并进入屏保。
 * @details MCP 工具在向云端返回最终结果前设置一次性标记；TTS stop 到达后等待剩余播放数据消费完，
 * 再关闭音频通道。由播报状态转为空闲的既有状态监听器负责立即显示屏保。
 */
void Application::RequestConversationEndAfterSpeaking() {
    end_conversation_after_speaking_.store(true);
}

/**
 * @brief 根据当前设备状态切换对话的开始、打断或结束动作。
 * @details 激活状态下退出等待，空闲时建立音频通道，播报时发送打断命令，监听时关闭会话。
 * 连接动作通过 Schedule() 延迟执行，让“连接中”界面先得到刷新。
 */
void Application::HandleToggleChatEvent() {
    auto state = GetDeviceState();

    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    } else if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    if (!xiaozhi_client_) {
        ESP_LOGE(TAG, "小智客户端尚未初始化");
        return;
    }

    if (state == kDeviceStateIdle) {
        ListeningMode mode = GetDefaultListeningMode();
        if (!xiaozhi_client_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // 先让主循环刷新连接状态，再执行可能阻塞的网络握手。
            Schedule([this, mode]() {
                ContinueOpenAudioChannel(mode);
            });
            return;
        }
        SetListeningMode(mode);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
    } else if (state == kDeviceStateListening) {
        xiaozhi_client_->CloseAudioChannel();
    }
}

/**
 * @brief 在状态界面刷新后继续打开音频通道并进入指定监听模式。
 * @param mode 音频通道打开成功后使用的监听停止策略。
 * @details 调度执行前会再次确认设备仍处于连接状态，防止用户操作改变状态后继续建立过期连接。
 * 连接前切换高性能模式以降低握手延迟；打开失败时保留协议层错误处理结果。
 */
void Application::ContinueOpenAudioChannel(ListeningMode mode) {
    // 调度等待期间状态可能已改变，过期任务必须直接退出。
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }

    // 连接握手期间关闭低功耗限制，降低网络和 TLS 处理延迟。
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

    if (!xiaozhi_client_->IsAudioChannelOpened()) {
        if (!xiaozhi_client_->OpenAudioChannel()) {
            return;
        }
    }

    // 网络握手期间监听请求可能已经超时或被用户取消，不能让迟到的连接结果重新进入监听状态。
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }

    SetListeningMode(mode);
}

/**
 * @brief 处理用户主动开始监听事件。
 * @details 激活状态下该事件用于退出激活等待；配网状态下用于启动音频自检；空闲状态下打开云端
 * 音频通道并进入请求的停止模式；正在播报时先打断 TTS，再切换为请求的监听模式。
 */
void Application::HandleStartListeningEvent() {
    auto state = GetDeviceState();
    const ListeningMode requested_mode = requested_listening_mode_.exchange(
        kListeningModeManualStop);

    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    }

    if (!xiaozhi_client_) {
        ESP_LOGE(TAG, "小智客户端尚未初始化");
        return;
    }

    if (state == kDeviceStateIdle) {
        if (!xiaozhi_client_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // 先让主循环处理“连接中”界面，再执行可能阻塞的网络握手。
            Schedule([this, requested_mode]() {
                ContinueOpenAudioChannel(requested_mode);
            });
            return;
        }
        SetListeningMode(requested_mode);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
        SetListeningMode(requested_mode);
    }
}

/**
 * @brief 处理用户主动停止监听事件。
 * @details 音频自检状态下停止测试并返回配网状态；连接状态下取消尚未完成的音频通道建立；正常监听
 * 状态下通知云端停止接收音频，然后回到空闲状态。其他状态收到该事件时不执行操作。
 */
void Application::HandleStopListeningEvent() {
    auto state = GetDeviceState();

    if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    } else if (state == kDeviceStateConnecting) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateListening) {
        if (xiaozhi_client_) {
            xiaozhi_client_->SendStopListening();
        }
        SetDeviceState(kDeviceStateIdle);
    }
}

/**
 * @brief 处理本地唤醒词检测事件。
 * @details 空闲时建立音频通道并开始自动停止监听；播报或监听期间再次唤醒会打断当前会话、清空
 * 待发送残留音频并重新开始监听；激活期间唤醒则退出激活等待。通道握手通过调度任务执行，
 * 以便主循环先刷新“连接中”状态。
 */
void Application::HandleWakeWordDetectedEvent() {
    // 唤醒词命中后先恢复背光，再进行可能耗时的云端音频通道握手。
    Board::GetInstance().WakeUpScreen(true);
    if (BackendService::GetInstance().InterruptNotificationPlayback()) {
        // 主动通知属于后台音频，用户唤醒必须立即清空已解码内容；下载任务会观察中断标志并停止读取。
        audio_service_.ResetDecoder();
        ESP_LOGI(TAG, "用户唤醒已中断主动通知播放");
    }

    if (!xiaozhi_client_) {
        return;
    }

    auto state = GetDeviceState();
    auto wake_word = audio_service_.GetLastWakeWord();
    ESP_LOGI(TAG, "检测到唤醒词：%s，当前状态=%d", wake_word.c_str(), (int)state);

    if (state == kDeviceStateIdle) {
        if (!xiaozhi_client_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // 先刷新“连接中”界面，再执行可能阻塞约一秒的音频通道握手。
            Schedule([this, wake_word]() {
                ContinueWakeWordInvoke(wake_word);
            });
            return;
        }
        // 已存在可用音频通道时无需重新握手，直接进入唤醒会话。
        ContinueWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateSpeaking || state == kDeviceStateListening) {
        AbortSpeaking(kAbortReasonWakeWordDetected);
        // 清空上一轮残留音频，避免云端把旧语音误认为新会话内容。
        while (audio_service_.PopPacketFromSendQueue());

        if (state == kDeviceStateListening) {
            xiaozhi_client_->SendStartListening(GetDefaultListeningMode());
#if CONFIG_SEND_WAKE_WORD_DATA
            audio_service_.EncodeWakeWord();
            xiaozhi_client_->SendWakeWordDetected(wake_word);
#endif
            audio_service_.ResetDecoder();
            audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
            // 唤醒词模块命中后会自行暂停，需要显式恢复以支持监听期间再次唤醒。
            audio_service_.EnableWakeWordDetection(true);
        } else {
            // 播报状态先打断 TTS，待进入监听状态后再播放提示音。
            play_popup_on_listening_ = true;
            SetListeningMode(GetDefaultListeningMode());
        }
    } else if (state == kDeviceStateActivating) {
        // 激活期间允许用户用唤醒操作退出等待，回到可交互的空闲状态。
        SetDeviceState(kDeviceStateIdle);
    }
}

/**
 * @brief 在连接状态下完成唤醒词触发的音频通道建立。
 * @param wake_word 本次命中的唤醒词文本，仅用于日志和会话追踪。
 * @details 方法会过滤过期调度任务、切换高性能模式并打开音频通道。连接失败时重新启用唤醒词
 * 检测，连接成功时设置延迟播放提示音标志，再进入默认自动停止监听模式。
 */
void Application::ContinueWakeWordInvoke(const std::string& wake_word) {
    // 调度等待期间状态可能已改变，过期唤醒任务不得继续建立连接。
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }

    // 网络握手前切换高性能模式，减少建立会话的可感知等待时间。
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

    if (!xiaozhi_client_->IsAudioChannelOpened()) {
        if (!xiaozhi_client_->OpenAudioChannel()) {
            audio_service_.EnableWakeWordDetection(true);
            return;
        }
    }

    ESP_LOGI(TAG, "检测到唤醒词：%s", wake_word.c_str());
#if CONFIG_SEND_WAKE_WORD_DATA
    // 编码并上报检测窗口内的唤醒词音频，随后发送唤醒词 detect 事件，供云端对齐唤醒词语义。
    audio_service_.EncodeWakeWord();
    xiaozhi_client_->SendWakeWordDetected(wake_word);
#endif
    // 提示音必须延迟到监听状态完成 ResetDecoder() 后播放，否则会被清空。
    play_popup_on_listening_ = true;
    SetListeningMode(GetDefaultListeningMode());
}

/**
 * @brief 根据最新设备状态同步界面、音频处理器、唤醒词和电源策略。
 * @details 状态机只负责校验转移，本方法负责各状态的实际副作用。所有显示更新、音频启停和提示音
 * 均集中在主循环上下文执行，从而避免网络回调与音频任务并发操作 UI 或 Codec。
 */
void Application::HandleStateChangedEvent() {
    DeviceState new_state = state_machine_.GetState();
    const bool enter_screensaver = enter_screensaver_after_conversation_.exchange(false);
    clock_ticks_ = 0;
    // 任何状态变化都会使之前延迟开始监听的标记失效；下面的 Listening 分支需要时再重新挂起。
    pending_listening_start_ = false;

    auto& board = Board::GetInstance();
    // 状态变化会刷新背光计时；如果表盘已经显示，该后台事件不会退出屏保。
    board.WakeUpScreen();
    auto display = board.GetDisplay();
    if (new_state != kDeviceStateSpeaking) {
        display->HideCustomMcpList();
    }
    auto led = board.GetLed();
    led->OnStateChanged();

    switch (new_state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
            display->SetStatus(Lang::Strings::STANDBY);
            display->ClearChatMessages();  // Clear messages first
            display->SetEmotion("neutral"); // Then set emotion (wechat mode checks child count)
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(true);
            break;
        case kDeviceStateConnecting:
            display->SetStatus(Lang::Strings::CONNECTING);
            display->SetEmotion("neutral");
            display->SetChatMessage("system", "");
            break;
        case kDeviceStateListening:
            display->SetStatus(Lang::Strings::LISTENING);
            display->SetEmotion("neutral");

            // 确保进入监听状态后语音处理器已启动。
            if (play_popup_on_listening_ || !audio_service_.IsAudioProcessorRunning()) {
                /*
                 * 正常从 TTS 播放结束进入自动监听时，需要等待剩余音频播放完，避免截断尾音。
                 * play_popup_on_listening_ 为 true 表示用户主动唤醒或打断上一轮对话，此时响应速度
                 * 优先，不能等待旧播放队列，否则主任务以及后续字幕更新都会被旧音频阻塞。
                 * 自动停止模式在未排空时不阻塞状态机主循环，而是挂起 pending_listening_start_，
                 * 待 MAIN_EVENT_PLAYBACK_DRAINED 事件到来后再真正启动语音处理。
                 */
                if (listening_mode_ == kListeningModeAutoStop && !play_popup_on_listening_ &&
                    !audio_service_.IsPlaybackIdle()) {
                    pending_listening_start_ = true;
                    audio_service_.RequestPlaybackDrainedNotification();
                } else {
                    StartListeningAudio();
                }
            }

#ifdef CONFIG_WAKE_WORD_DETECTION_IN_LISTENING
            // 根据 Kconfig 配置决定监听期间是否继续运行唤醒词检测（AFE 唤醒词）。
            audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
#elif defined(CONFIG_WAKE_WORD_BARGE_IN)
            // 监听期间保持本地唤醒词检测，允许再次说“你好小智”打断并重开对话。
            audio_service_.EnableWakeWordDetection(true);
#else
            // 默认在监听期间关闭唤醒词检测，减少算力占用和误触发。
            audio_service_.EnableWakeWordDetection(false);
#endif
            break;
        case kDeviceStateSpeaking:
            display->SetStatus(Lang::Strings::SPEAKING);

            if (listening_mode_ != kListeningModeRealtime) {
                audio_service_.EnableVoiceProcessing(false);
#ifdef CONFIG_WAKE_WORD_BARGE_IN
                // 播放回复期间保持本地唤醒词检测，允许用唤醒词随时打断（barge-in）。
                audio_service_.EnableWakeWordDetection(true);
#else
                audio_service_.EnableWakeWordDetection(false);
#endif
            }
            audio_service_.ResetDecoder();
            break;
        case kDeviceStateWifiConfiguring:
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(false);
            break;
        default:
            // 其他状态没有需要同步的音频或界面副作用。
            break;
    }

    /*
     * 先完成空闲状态对应的字幕清理、表情恢复和唤醒词启用，再覆盖显示表盘。只有监听或
     * 播报真正转为空闲才由本路径立即进入屏保；开机初始化完成由激活收尾路径单独处理，
     * 升级失败、连接失败等其他空闲状态仍使用 30 秒延时。
     */
    if (new_state == kDeviceStateIdle && enter_screensaver) {
        board.EnterScreensaver();
    }
}

/**
 * @brief 将回调投递到主任务执行。
 * @param callback 待执行的一次性回调；所有权通过右值移动到内部队列。
 * @note 可从任意任务调用，适合把网络或音频回调转换为主任务操作。
 * @details 使用 mutex_ 保护任务队列，入队后设置 MAIN_EVENT_SCHEDULE 唤醒主循环顺序执行。
 */
void Application::Schedule(std::function<void()>&& callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

/**
 * @brief 中断云端正在进行的语音播报。
 * @param reason 中断原因，会发送给服务器。
 * @details 先设置本地 aborted_ 标志，使后续状态回调识别本次中断，再通过当前协议发送 abort 消息。
 */
void Application::AbortSpeaking(AbortReason reason) {
    ESP_LOGI(TAG, "正在中断云端语音播报");
    end_conversation_after_speaking_.store(false);
    aborted_ = true;
    if (xiaozhi_client_) {
        xiaozhi_client_->SendAbortSpeaking(reason);
    }
}

/**
 * @brief 保存监听停止策略并切换到监听状态。
 * @param mode 手动停止、自动停止或其他协议支持的监听模式。
 * @details 实际音频处理启用和 start-listening 消息发送由状态变化处理函数统一完成。
 */
void Application::SetListeningMode(ListeningMode mode) {
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
}

/**
 * @brief 获取当前板型默认使用的监听停止策略。
 * @return 固定返回 kListeningModeAutoStop，由服务端 VAD 或超时结束用户输入。
 */
ListeningMode Application::GetDefaultListeningMode() const {
    return kListeningModeAutoStop;
}

/**
 * @brief 完成监听开始的真实动作。
 * @details 发送 start-listening、启用麦克风语音处理并在解码器重置后播放提示音。既可从状态变化
 * 处理器直接调用，也可经 MAIN_EVENT_PLAYBACK_DRAINED 延迟调用（此时下行播放已排空，不会
 * 截断上一轮 TTS 尾音）。离开监听状态后不再执行，避免在错误时刻启用上传。
 */
void Application::StartListeningAudio() {
    if (GetDeviceState() != kDeviceStateListening) {
        return;
    }

    // 通知云端开始接收本轮麦克风音频。
    if (xiaozhi_client_) {
        xiaozhi_client_->SendStartListening(listening_mode_);
    }
    audio_service_.EnableVoiceProcessing(true);

    // EnableVoiceProcessing() 重置解码器后再播放提示音，避免提示音被清空。
    if (play_popup_on_listening_) {
        play_popup_on_listening_ = false;
        audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
    }
}
