/**
 * @file application.cc
 * @brief 应用主事件循环、状态处理和模块编排实现。
 */
#include "application.h"
#include "board.h"
#include "display.h"
#include "system_info.h"
#include "audio_codec.h"
#include "mqtt_protocol.h"
#include "websocket_protocol.h"
#include "assets/lang_config.h"
#include "mcp_server.h"
#include "backend/backend_service.h"
#include "assets.h"
#include "settings.h"

#include <cctype>
#include <cstring>
#include <esp_log.h>
#include <cJSON.h>
#include <driver/gpio.h>
#include <arpa/inet.h>
#include <font_awesome.h>

#define TAG "Application"

namespace {

/**
 * @brief 判断云端 TTS 字幕是否为不应展示给用户的内部工具调用标记。
 * @param text 云端 `tts.sentence_start.text` 字段，可为空指针。
 * @return 去除前导空白后，文本以 `%` 开头且后面紧跟 ASCII 接口标识符时返回 true；
 *         普通自然语言或正文中间包含百分号时返回 false。
 * @details 部分云端模型会把 `% getWeather...`、`%self.tool(...)` 一类内部调用过程
 *          错误地作为 TTS 字幕下发。该判断只识别位于整段文本开头的工具标记，避免把
 *          “降水概率 30%”等正常回答误判为工具调用。
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
 * @brief 构造 Application 对象并初始化该模块运行所需的成员和系统资源。
 * @details 构造阶段只建立本模块自身资源；需要异步运行的任务由后续 Start 或 Initialize 方法启动。
 */
Application::Application() {
    event_group_ = xEventGroupCreate();

    esp_timer_create_args_t clock_timer_args = {
        .callback = [](void* arg) {
            Application* app = (Application*)arg;
            xEventGroupSetBits(app->event_group_, MAIN_EVENT_CLOCK_TICK);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);
}

/**
 * @brief 析构 Application 对象并释放其持有的系统资源。
 * @details 释放顺序与创建顺序相反，先停止异步来源，再销毁句柄和动态内存，避免回调访问失效对象。
 */
Application::~Application() {
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    vEventGroupDelete(event_group_);
}

/**
 * @brief 请求状态机切换到指定状态。
 * @param state 目标设备状态。
 * @return 转移符合状态规则并执行成功时返回 true，否则返回 false。
 * @details 本方法只委托状态机校验并提交转移；界面、音频和电源副作用由状态变化事件统一处理。
 */
bool Application::SetDeviceState(DeviceState state) {
    return state_machine_.TransitionTo(state);
}

/**
 * @brief 初始化显示、音频、网络回调、状态机和定时器。
 * 
 * 网络连接会异步启动，本方法完成并不表示云端已经连接。应在 app_main 中
 * 调用一次，随后调用 Run() 进入主事件循环。
 * @details 初始化顺序为显示、Codec 与音频任务、音频回调、状态机监听器、时钟定时器、MCP 工具和
 *          网络回调。最后异步启动网络，因此所有网络结果都通过事件组交给 Run() 处理。
 */
void Application::Initialize() {
    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    // 创建界面对象并完成状态栏、对话区域和表情区域布局。
    auto display = board.GetDisplay();
    display->SetupUI();
    // 启动阶段显示板型与固件版本，便于现场确认正在运行的镜像。
    display->SetChatMessage("system", SystemInfo::GetUserAgent().c_str());

    // 绑定板级 Codec，初始化编解码器并启动音频后台任务。
    auto codec = board.GetAudioCodec();
    audio_service_.Initialize(codec);
    audio_service_.Start();

    AudioServiceCallbacks callbacks;
    callbacks.on_send_queue_available = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
    };
    callbacks.on_wake_word_detected = [this](const std::string& wake_word) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
    };
    callbacks.on_vad_change = [this](bool speaking) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
    };
    audio_service_.SetCallbacks(callbacks);

    // 状态机回调只记录轻量状态并设置事件位，所有界面和音频副作用由主循环集中执行。
    state_machine_.AddStateChangeListener([this](DeviceState old_state, DeviceState new_state) {
        if (new_state == kDeviceStateIdle
            && (old_state == kDeviceStateListening || old_state == kDeviceStateSpeaking)) {
            enter_screensaver_after_conversation_.store(true);
        }
        xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
    });

    // 每秒触发时钟事件，用于刷新时间、电量和网络状态图标。
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    // MCP 工具只在应用初始化时注册一次，避免重复名称和重复对象。
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddCommonTools();
    auto& backend_service = BackendService::GetInstance();
    backend_service.Start();

    /*
     * 业务工具必须位于 user_only 管理工具之前。小智云端通过 tools/list 分页读取工具，
     * 单页上限为 8 KB；若管理工具排在前面，天气和备忘录可能落到后续分页而无法进入
     * 当前大模型上下文，最终错误调用云端自带的 get_weather。
     */
    backend_service.RegisterMcpTools(mcp_server);
    mcp_server.AddUserOnlyTools();

    // 将板级网络事件转换为界面提示和主循环事件。
    board.SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
        auto& board = Board::GetInstance();
        board.WakeUpScreen();
        auto display = board.GetDisplay();
        
        switch (event) {
            case NetworkEvent::Scanning:
                display->ShowNotification(Lang::Strings::SCANNING_WIFI, 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::Connecting: {
                if (data.empty()) {
                    // 蜂窝网络尚未获得运营商信息，显示正在注册网络。
                    display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                } else {
                    // Wi-Fi 或已获得运营商名称的蜂窝网络显示具体连接目标。
                    std::string msg = Lang::Strings::CONNECT_TO;
                    msg += data;
                    msg += "...";
                    display->ShowNotification(msg.c_str(), 30000);
                }
                break;
            }
            case NetworkEvent::Connected: {
                std::string msg = Lang::Strings::CONNECTED_TO;
                msg += data;
                display->ShowNotification(msg.c_str(), 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_CONNECTED);
                BackendService::GetInstance().OnNetworkConnected();
                break;
            }
            case NetworkEvent::Disconnected:
                BackendService::GetInstance().OnNetworkDisconnected();
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::WifiConfigModeEnter:
                // Wi-Fi 配网模式的进入流程由 WifiBoard 内部处理。
                break;
            case NetworkEvent::WifiConfigModeExit:
                // Wi-Fi 配网模式的退出流程由 WifiBoard 内部处理。
                break;
            // 以下事件只由蜂窝网络板型产生，本板型保留统一接口兼容性。
            case NetworkEvent::ModemDetecting:
                display->SetStatus(Lang::Strings::DETECTING_MODULE);
                break;
            case NetworkEvent::ModemErrorNoSim:
                Alert(Lang::Strings::ERROR, Lang::Strings::PIN_ERROR, "triangle_exclamation", Lang::Sounds::OGG_ERR_PIN);
                break;
            case NetworkEvent::ModemErrorRegDenied:
                Alert(Lang::Strings::ERROR, Lang::Strings::REG_ERROR, "triangle_exclamation", Lang::Sounds::OGG_ERR_REG);
                break;
            case NetworkEvent::ModemErrorInitFailed:
                Alert(Lang::Strings::ERROR, Lang::Strings::MODEM_INIT_ERROR, "triangle_exclamation", Lang::Sounds::OGG_EXCLAMATION);
                break;
            case NetworkEvent::ModemErrorTimeout:
                display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                break;
        }
    });

    // 异步启动网络，连接结果通过上方回调返回。
    board.StartNetwork();

    // 初始化结束后立即刷新一次状态栏，不等待首个周期定时事件。
    display->UpdateStatusBar(true);
}

/**
 * @brief 运行应用主事件循环。
 * 
 * 本方法阻塞等待事件组，依次处理网络、状态变化、按键和音频事件，正常情况
 * 下不会返回。所有业务状态切换均在这个调用上下文中完成。
 * @details 每次唤醒会清除已取得的事件位，并按固定顺序处理错误、网络、状态、用户输入和音频数据。
 *          调度队列中的闭包也在此处串行执行，从而把跨线程回调转换为单线程业务状态变更。
 */
void Application::Run() {
    // 提高主任务优先级，保证状态事件和音频发送得到及时处理。
    vTaskPrioritySet(nullptr, 10);

    const EventBits_t ALL_EVENTS = 
        MAIN_EVENT_SCHEDULE |
        MAIN_EVENT_SEND_AUDIO |
        MAIN_EVENT_WAKE_WORD_DETECTED |
        MAIN_EVENT_VAD_CHANGE |
        MAIN_EVENT_CLOCK_TICK |
        MAIN_EVENT_ERROR |
        MAIN_EVENT_NETWORK_CONNECTED |
        MAIN_EVENT_NETWORK_DISCONNECTED |
        MAIN_EVENT_TOGGLE_CHAT |
        MAIN_EVENT_START_LISTENING |
        MAIN_EVENT_STOP_LISTENING |
        MAIN_EVENT_ACTIVATION_DONE |
        MAIN_EVENT_STATE_CHANGED;

    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, ALL_EVENTS, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & MAIN_EVENT_ERROR) {
            SetDeviceState(kDeviceStateIdle);
            Alert(Lang::Strings::ERROR, last_error_message_.c_str(), "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        }

        if (bits & MAIN_EVENT_NETWORK_CONNECTED) {
            HandleNetworkConnectedEvent();
        }

        if (bits & MAIN_EVENT_NETWORK_DISCONNECTED) {
            HandleNetworkDisconnectedEvent();
        }

        if (bits & MAIN_EVENT_ACTIVATION_DONE) {
            HandleActivationDoneEvent();
        }

        if (bits & MAIN_EVENT_STATE_CHANGED) {
            HandleStateChangedEvent();
        }

        if (bits & MAIN_EVENT_TOGGLE_CHAT) {
            HandleToggleChatEvent();
        }

        if (bits & MAIN_EVENT_START_LISTENING) {
            HandleStartListeningEvent();
        }

        if (bits & MAIN_EVENT_STOP_LISTENING) {
            HandleStopListeningEvent();
        }

        if (bits & MAIN_EVENT_SEND_AUDIO) {
            while (auto packet = audio_service_.PopPacketFromSendQueue()) {
                if (protocol_ && !protocol_->SendAudio(std::move(packet))) {
                    break;
                }
            }
        }

        if (bits & MAIN_EVENT_WAKE_WORD_DETECTED) {
            HandleWakeWordDetectedEvent();
        }

        if (bits & MAIN_EVENT_VAD_CHANGE) {
            if (GetDeviceState() == kDeviceStateListening) {
                auto led = Board::GetInstance().GetLed();
                led->OnStateChanged();
            }
        }

        if (bits & MAIN_EVENT_SCHEDULE) {
            std::unique_lock<std::mutex> lock(mutex_);
            auto tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto& task : tasks) {
                task();
            }
        }

        if (bits & MAIN_EVENT_CLOCK_TICK) {
            clock_ticks_++;
            auto display = Board::GetInstance().GetDisplay();
            display->UpdateStatusBar();
        
            // 每十秒输出一次运行统计，便于诊断堆内存和音频队列状态。
            if (clock_ticks_ % 10 == 0) {
                SystemInfo::PrintHeapStats();
            }
        }
    }
}

/**
 * @brief 处理对应的应用事件。
 * @details 实现会维护 Application 的内部一致性；发生错误时记录日志，并避免向后续流程传播无效资源。
 */
void Application::HandleNetworkConnectedEvent() {
    ESP_LOGI(TAG, "Network connected");
    auto state = GetDeviceState();

    if (state == kDeviceStateStarting || state == kDeviceStateWifiConfiguring) {
        // 网络就绪后启动后台激活任务，避免阻塞主事件循环。
        SetDeviceState(kDeviceStateActivating);
        if (activation_task_handle_ != nullptr) {
            ESP_LOGW(TAG, "Activation task already running");
            return;
        }

        xTaskCreate([](void* arg) {
            Application* app = static_cast<Application*>(arg);
            app->ActivationTask();
            app->activation_task_handle_ = nullptr;
            vTaskDelete(NULL);
        }, "activation", 4096 * 2, this, 2, &activation_task_handle_);
    }

    // 连接成功后立即刷新状态栏网络图标。
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

/**
 * @brief 处理对应的应用事件。
 * @details 实现会维护 Application 的内部一致性；发生错误时记录日志，并避免向后续流程传播无效资源。
 */
void Application::HandleNetworkDisconnectedEvent() {
    // 网络断开时关闭当前会话，防止继续向失效协议对象发送音频。
    auto state = GetDeviceState();
    if (state == kDeviceStateConnecting || state == kDeviceStateListening || state == kDeviceStateSpeaking) {
        ESP_LOGI(TAG, "Closing audio channel due to network disconnection");
        protocol_->CloseAudioChannel();
    }

    // 断网后立即刷新状态栏网络图标。
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

/**
 * @brief 完成开机激活流程并切换到可唤醒的屏保待机状态。
 * @details 本方法运行在应用主循环中。它先进入空闲状态、保存服务端时间状态、释放 OTA 临时
 *          对象并恢复低功耗等级，再排队播放启动成功提示音，最后在用户启用屏保时立即显示表盘。
 */
void Application::HandleActivationDoneEvent() {
    ESP_LOGI(TAG, "Activation done");

    SystemInfo::PrintHeapStats();
    SetDeviceState(kDeviceStateIdle);

    has_server_time_ = ota_->HasServerTime();

    auto display = Board::GetInstance().GetDisplay();
    std::string message = std::string(Lang::Strings::VERSION) + ota_->GetCurrentVersion();
    display->ShowNotification(message.c_str());
    display->SetChatMessage("system", "");

    // 激活和版本检查完成后释放 OTA 临时状态，降低常驻内存占用。
    ota_.reset();
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);

    Schedule([this]() {
        // 播放成功提示音，表示设备已进入可唤醒状态。
        audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS);
    });

    /*
     * 此处代表开机版本检查、资源应用、设备激活和协议初始化均已完成。直接进入表盘可以
     * 跳过普通待机的 30 秒等待；板级实现仍会检查用户是否已经关闭屏保功能。
     */
    board.EnterScreensaver();
}

/**
 * @brief 后台执行设备激活，完成后设置 MAIN_EVENT_ACTIVATION_DONE。
 * @details 该任务按顺序检查资源包、检查固件和激活状态、创建云端协议对象。所有耗时网络操作
 *          均在后台任务中执行，完成后只通过事件位通知主循环更新设备状态。
 */
void Application::ActivationTask() {
    // OTA 对象同时负责版本检查、设备激活和云端协议配置获取。
    ota_ = std::make_unique<Ota>();

    // 优先更新界面资源，确保后续状态页面使用与固件匹配的资源版本。
    CheckAssetsVersion();

    // 检查固件和设备激活状态；必要时执行升级或等待用户完成激活。
    CheckNewVersion();

    // 根据版本服务返回的配置选择 MQTT 或 WebSocket 协议。
    InitializeProtocol();

    // 通过事件组回到主循环收尾，避免后台任务直接修改前台状态。
    xEventGroupSetBits(event_group_, MAIN_EVENT_ACTIVATION_DONE);
}

/**
 * @brief 检查并应用待更新的界面资源包。
 * @details 本方法每次启动只执行一次。若 NVS 中存在下载地址，则临时切换到升级状态和高性能模式，
 *          下载资源分区并显示进度；下载失败时恢复激活状态，成功或无需下载时统一应用现有资源。
 */
void Application::CheckAssetsVersion() {
    // 防止网络重连或激活流程重复触发同一次资源检查。
    if (assets_version_checked_) {
        return;
    }
    assets_version_checked_ = true;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto& assets = Assets::GetInstance();

    if (!assets.partition_valid()) {
        ESP_LOGW(TAG, "Assets partition is disabled for board %s", BOARD_NAME);
        return;
    }
    
    Settings settings("assets", true);
    // 下载地址由 MCP 工具写入 NVS，读取后立即删除，避免重启后重复下载。
    std::string download_url = settings.GetString("download_url");

    if (!download_url.empty()) {
        settings.EraseKey("download_url");

        char message[256];
        snprintf(message, sizeof(message), Lang::Strings::FOUND_NEW_ASSETS, download_url.c_str());
        Alert(Lang::Strings::LOADING_ASSETS, message, "cloud_arrow_down", Lang::Sounds::OGG_UPGRADE);
        
        // 给提示音预留播放时间，再进入资源写入阶段。
        vTaskDelay(pdMS_TO_TICKS(3000));
        SetDeviceState(kDeviceStateUpgrading);
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        display->SetChatMessage("system", Lang::Strings::PLEASE_WAIT);

        bool success = assets.Download(download_url, [this, display](int progress, size_t speed) -> void {
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
            Schedule([display, message = std::string(buffer)]() {
                display->SetChatMessage("system", message.c_str());
            });
        });

        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!success) {
            Alert(Lang::Strings::ERROR, Lang::Strings::DOWNLOAD_ASSETS_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
            vTaskDelay(pdMS_TO_TICKS(2000));
            SetDeviceState(kDeviceStateActivating);
            return;
        }
    }

    // 从资源分区装载字体、图片和声音；无新版本时也需要执行一次初始化。
    assets.Apply();
    display->SetChatMessage("system", "");
    display->SetEmotion("microchip_ai");
}

/**
 * @brief 检查固件版本、云端连接配置和设备激活状态。
 * @details 版本请求失败时最多重试十次并采用指数退避。返回新固件时直接进入 OTA 升级；
 *          没有新版本时确认当前分区有效，并根据服务器返回的激活码或挑战数据循环执行激活。
 */
void Application::CheckNewVersion() {
    const int MAX_RETRY = 10;
    int retry_count = 0;
    int retry_delay = 10; // 首次重试等待 10 秒，后续失败时按指数退避增加。

    auto& board = Board::GetInstance();
    while (true) {
        auto display = board.GetDisplay();
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);

        esp_err_t err = ota_->CheckVersion();
        if (err != ESP_OK) {
            retry_count++;
            if (retry_count >= MAX_RETRY) {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                return;
            }

            char error_message[128];
            snprintf(error_message, sizeof(error_message), "code=%d, url=%s", err, ota_->GetCheckVersionUrl().c_str());
            char buffer[256];
            snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED, retry_delay, error_message);
            Alert(Lang::Strings::ERROR, buffer, "cloud_slash", Lang::Sounds::OGG_EXCLAMATION);

            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)", retry_delay, retry_count, MAX_RETRY);
            for (int i = 0; i < retry_delay; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (GetDeviceState() == kDeviceStateIdle) {
                    break;
                }
            }
            retry_delay *= 2; // 每次失败后将等待时间加倍，降低服务异常时的请求压力。
            continue;
        }
        retry_count = 0;
        retry_delay = 10; // 一次请求成功后恢复初始重试间隔。

        if (ota_->HasNewVersion()) {
            if (UpgradeFirmware(ota_->GetFirmwareUrl(), ota_->GetFirmwareVersion())) {
                return; // 正常升级会重启设备，因此通常不会执行到这里。
            }
            // 升级失败时不阻塞设备启动，继续使用当前固件进入正常流程。
        }

        // 当前固件能够运行到此处，可将待确认 OTA 分区标记为有效，防止下次启动回滚。
        ota_->MarkCurrentVersionValid();
        if (!ota_->HasActivationCode() && !ota_->HasActivationChallenge()) {
            // 无激活任务时版本检查流程已经完成。
            break;
        }

        display->SetStatus(Lang::Strings::ACTIVATION);
        // 有激活码时显示给用户，由用户在管理端完成设备绑定。
        if (ota_->HasActivationCode()) {
            ShowActivationCode(ota_->GetActivationCode(), ota_->GetActivationMessage());
        }

        // 轮询激活接口，直到成功、设备状态被用户中止或本轮尝试结束。
        for (int i = 0; i < 10; ++i) {
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t err = ota_->Activate();
            if (err == ESP_OK) {
                break;
            } else if (err == ESP_ERR_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(3000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
            if (GetDeviceState() == kDeviceStateIdle) {
                break;
            }
        }
    }
}

/**
 * @brief 根据 OTA 配置创建并配置云端通信协议。
 * @details 优先使用服务器下发的 MQTT 或 WebSocket 配置，未指定时回退到 MQTT。随后注册连接、
 *          错误、音频、JSON 控制消息和 MCP 广播回调，使协议线程只产生事件或调度主线程任务，
 *          避免直接跨线程修改显示与设备状态。
 */
void Application::InitializeProtocol() {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto codec = board.GetAudioCodec();

    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

    if (ota_->HasMqttConfig()) {
        protocol_ = std::make_unique<MqttProtocol>();
    } else if (ota_->HasWebsocketConfig()) {
        protocol_ = std::make_unique<WebsocketProtocol>();
    } else {
        ESP_LOGW(TAG, "No protocol specified in the OTA config, using MQTT");
        protocol_ = std::make_unique<MqttProtocol>();
    }

    protocol_->OnConnected([this]() {
        DismissAlert();
    });

    protocol_->OnNetworkError([this](const std::string& message) {
        last_error_message_ = message;
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
    });
    
    protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
        if (GetDeviceState() == kDeviceStateSpeaking) {
            audio_service_.PushPacketToDecodeQueue(std::move(packet));
        }
    });
    
    protocol_->OnAudioChannelOpened([this, codec, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG, "Server sample rate %d does not match device output sample rate %d, resampling may cause distortion",
                protocol_->server_sample_rate(), codec->output_sample_rate());
        }
    });
    
    protocol_->OnAudioChannelClosed([this, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        Schedule([this]() {
            auto display = Board::GetInstance().GetDisplay();
            display->SetChatMessage("system", "");
            SetDeviceState(kDeviceStateIdle);
        });
    });
    
    protocol_->OnIncomingJson([this, display](const cJSON* root) {
        // 按 type 字段分发云端控制消息：TTS、STT、LLM 表情、MCP 和系统命令。
        auto type = cJSON_GetObjectItem(root, "type");
        if (strcmp(type->valuestring, "tts") == 0) {
            auto state = cJSON_GetObjectItem(root, "state");
            if (strcmp(state->valuestring, "start") == 0) {
                Schedule([this]() {
                    aborted_ = false;
                    SetDeviceState(kDeviceStateSpeaking);
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                Schedule([this]() {
                    if (GetDeviceState() == kDeviceStateSpeaking) {
                        if (listening_mode_ == kListeningModeManualStop) {
                            SetDeviceState(kDeviceStateIdle);
                        } else {
                            SetDeviceState(kDeviceStateListening);
                        }
                    }
                });
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                auto text = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(text)) {
                    ESP_LOGI(TAG, "<< %s", text->valuestring);
                    if (IsInternalToolCallText(text->valuestring)) {
                        /*
                         * 工具调用仍由云端和 MCP 正常执行，这里只阻止内部接口标记进入字幕。
                         * 不清空当前字幕，避免工具执行期间屏幕突然变为空白。
                         */
                        ESP_LOGD(TAG, "Filtered internal tool call subtitle: %s", text->valuestring);
                    } else {
                        Schedule([this, display, message = std::string(text->valuestring)]() {
                            /*
                             * 用户已经打断上一轮播报并进入新一轮监听时，云端仍可能送达少量
                             * 上一轮残留的 sentence_start。只有当前仍处于说话状态且没有执行
                             * 本地打断时才显示助手字幕，避免旧回答覆盖新一轮用户字幕。
                             */
                            if (!aborted_ && GetDeviceState() == kDeviceStateSpeaking) {
                                display->SetChatMessage("assistant", message.c_str());
                            }
                        });
                    }
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                ESP_LOGI(TAG, ">> %s", text->valuestring);
                Schedule([display, message = std::string(text->valuestring)]() {
                    display->SetChatMessage("user", message.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(emotion)) {
                Schedule([display, emotion_str = std::string(emotion->valuestring)]() {
                    display->SetEmotion(emotion_str.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "mcp") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(payload)) {
                McpServer::GetInstance().ParseMessage(payload);
            }
        } else if (strcmp(type->valuestring, "system") == 0) {
            auto command = cJSON_GetObjectItem(root, "command");
            if (cJSON_IsString(command)) {
                ESP_LOGI(TAG, "System command: %s", command->valuestring);
                if (strcmp(command->valuestring, "reboot") == 0) {
                    // 云端系统命令要求重启时，调度到主线程执行受控重启。
                    Schedule([this]() {
                        Reboot();
                    });
                } else {
                    ESP_LOGW(TAG, "Unknown system command: %s", command->valuestring);
                }
            }
        } else if (strcmp(type->valuestring, "alert") == 0) {
            auto status = cJSON_GetObjectItem(root, "status");
            auto message = cJSON_GetObjectItem(root, "message");
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(status) && cJSON_IsString(message) && cJSON_IsString(emotion)) {
                Alert(status->valuestring, message->valuestring, emotion->valuestring, Lang::Sounds::OGG_VIBRATION);
            } else {
                ESP_LOGW(TAG, "Alert command requires status, message and emotion");
            }
#if CONFIG_RECEIVE_CUSTOM_MESSAGE
        } else if (strcmp(type->valuestring, "custom") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            ESP_LOGI(TAG, "Received custom message: %s", cJSON_PrintUnformatted(root));
            if (cJSON_IsObject(payload)) {
                Schedule([this, display, payload_str = std::string(cJSON_PrintUnformatted(payload))]() {
                    display->SetChatMessage("system", payload_str.c_str());
                });
            } else {
                ESP_LOGW(TAG, "Invalid custom message format: missing payload");
            }
#endif
        } else {
            ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring);
        }
    });
    
    protocol_->Start();
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
    ESP_LOGW(TAG, "Alert [%s] %s: %s", emotion, status, message);
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
 * @brief 请求设备开始录音和上传语音。
 * @note 方法本身不直接操作音频硬件，可从按键回调等其他任务安全调用。
 * @details 只设置 MAIN_EVENT_START_LISTENING 事件位，实际状态判断和通道连接由主循环完成。
 */
void Application::StartListening() {
    Board::GetInstance().WakeUpScreen(true);
    xEventGroupSetBits(event_group_, MAIN_EVENT_START_LISTENING);
}

/**
 * @brief 请求设备停止录音并通知云端本轮输入结束。
 * @details 只设置 MAIN_EVENT_STOP_LISTENING 事件位，可从按键或其他任务安全调用。
 */
void Application::StopListening() {
    Board::GetInstance().WakeUpScreen(true);
    xEventGroupSetBits(event_group_, MAIN_EVENT_STOP_LISTENING);
}

/**
 * @brief 根据当前设备状态切换对话的开始、打断或结束动作。
 * @details 激活状态下退出等待，空闲时建立音频通道，播报时发送打断命令，监听时关闭会话。
 *          连接动作通过 Schedule() 延迟执行，让“连接中”界面先得到刷新。
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

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (state == kDeviceStateIdle) {
        ListeningMode mode = GetDefaultListeningMode();
        if (!protocol_->IsAudioChannelOpened()) {
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
        protocol_->CloseAudioChannel();
    }
}

/**
 * @brief 在状态界面刷新后继续打开音频通道并进入指定监听模式。
 * @param mode 音频通道打开成功后使用的监听停止策略。
 * @details 调度执行前会再次确认设备仍处于连接状态，防止用户操作改变状态后继续建立过期连接。
 *          连接前切换高性能模式以降低握手延迟；打开失败时保留协议层错误处理结果。
 */
void Application::ContinueOpenAudioChannel(ListeningMode mode) {
    // 调度等待期间状态可能已改变，过期任务必须直接退出。
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }

    // 连接握手期间关闭低功耗限制，降低网络和 TLS 处理延迟。
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

    if (!protocol_->IsAudioChannelOpened()) {
        if (!protocol_->OpenAudioChannel()) {
            return;
        }
    }

    SetListeningMode(mode);
}

/**
 * @brief 处理用户主动开始监听事件。
 * @details 激活状态下该事件用于退出激活等待；配网状态下用于启动音频自检；空闲状态下打开云端
 *          音频通道并进入手动停止模式；正在播报时先打断 TTS，再切换为手动监听。
 */
void Application::HandleStartListeningEvent() {
    auto state = GetDeviceState();
    
    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }
    
    if (state == kDeviceStateIdle) {
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // 先让主循环处理“连接中”界面，再执行可能阻塞的网络握手。
            Schedule([this]() {
                ContinueOpenAudioChannel(kListeningModeManualStop);
            });
            return;
        }
        SetListeningMode(kListeningModeManualStop);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
        SetListeningMode(kListeningModeManualStop);
    }
}

/**
 * @brief 处理用户主动停止监听事件。
 * @details 音频自检状态下停止测试并返回配网状态；正常监听状态下通知云端停止接收音频，
 *          然后回到空闲状态。其他状态收到该事件时不执行操作。
 */
void Application::HandleStopListeningEvent() {
    auto state = GetDeviceState();
    
    if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    } else if (state == kDeviceStateListening) {
        if (protocol_) {
            protocol_->SendStopListening();
        }
        SetDeviceState(kDeviceStateIdle);
    }
}

/**
 * @brief 处理本地唤醒词检测事件。
 * @details 空闲时建立音频通道并开始自动停止监听；播报或监听期间再次唤醒会打断当前会话、清空
 *          待发送残留音频并重新开始监听；激活期间唤醒则退出激活等待。通道握手通过调度任务执行，
 *          以便主循环先刷新“连接中”状态。
 */
void Application::HandleWakeWordDetectedEvent() {
    // 唤醒词命中后先恢复背光，再进行可能耗时的云端音频通道握手。
    Board::GetInstance().WakeUpScreen(true);

    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();
    auto wake_word = audio_service_.GetLastWakeWord();
    ESP_LOGI(TAG, "Wake word detected: %s (state: %d)", wake_word.c_str(), (int)state);

    if (state == kDeviceStateIdle) {
        if (!protocol_->IsAudioChannelOpened()) {
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
            protocol_->SendStartListening(GetDefaultListeningMode());
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
 *          检测，连接成功时设置延迟播放提示音标志，再进入默认自动停止监听模式。
 */
void Application::ContinueWakeWordInvoke(const std::string& wake_word) {
    // 调度等待期间状态可能已改变，过期唤醒任务不得继续建立连接。
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }

    // 网络握手前切换高性能模式，减少建立会话的可感知等待时间。
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

    if (!protocol_->IsAudioChannelOpened()) {
        if (!protocol_->OpenAudioChannel()) {
            audio_service_.EnableWakeWordDetection(true);
            return;
        }
    }

    ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
    // 提示音必须延迟到监听状态完成 ResetDecoder() 后播放，否则会被清空。
    play_popup_on_listening_ = true;
    SetListeningMode(GetDefaultListeningMode());
}

/**
 * @brief 根据最新设备状态同步界面、音频处理器、唤醒词和电源策略。
 * @details 状态机只负责校验转移，本方法负责各状态的实际副作用。所有显示更新、音频启停和提示音
 *          均集中在主循环上下文执行，从而避免网络回调与音频任务并发操作 UI 或 Codec。
 */
void Application::HandleStateChangedEvent() {
    DeviceState new_state = state_machine_.GetState();
    const bool enter_screensaver = enter_screensaver_after_conversation_.exchange(false);
    clock_ticks_ = 0;

    auto& board = Board::GetInstance();
    // 状态变化会刷新背光计时；如果表盘已经显示，该后台事件不会退出屏保。
    board.WakeUpScreen();
    auto display = board.GetDisplay();
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
                 */
                if (listening_mode_ == kListeningModeAutoStop && !play_popup_on_listening_) {
                    audio_service_.WaitForPlaybackQueueEmpty();
                }
                
                // 通知云端开始接收本轮麦克风音频。
                protocol_->SendStartListening(listening_mode_);
                audio_service_.EnableVoiceProcessing(true);
            }

#ifdef CONFIG_WAKE_WORD_DETECTION_IN_LISTENING
            // 根据 Kconfig 配置决定监听期间是否继续运行唤醒词检测。
            audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
#else
            // 默认在监听期间关闭唤醒词检测，减少算力占用和误触发。
            audio_service_.EnableWakeWordDetection(false);
#endif
            
            // EnableVoiceProcessing() 重置解码器后再播放提示音，避免提示音被清空。
            if (play_popup_on_listening_) {
                play_popup_on_listening_ = false;
                audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
            }
            break;
        case kDeviceStateSpeaking:
            display->SetStatus(Lang::Strings::SPEAKING);

            if (listening_mode_ != kListeningModeRealtime) {
                audio_service_.EnableVoiceProcessing(false);
                audio_service_.EnableWakeWordDetection(false);
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
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true;
    if (protocol_) {
        protocol_->SendAbortSpeaking(reason);
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
 * @brief 等待必要数据落盘后重启 ESP32。
 * @details 先关闭音频通道、释放协议对象并停止音频后台任务，再等待一秒让网络和存储操作收尾，
 *          最后调用 esp_restart() 执行芯片复位。
 */
void Application::Reboot() {
    ESP_LOGI(TAG, "Rebooting...");
    // 重启前主动关闭音频会话，避免服务端继续保留设备在线状态。
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
    protocol_.reset();
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
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        ESP_LOGI(TAG, "Closing audio channel before firmware upgrade");
        protocol_->CloseAudioChannel();
    }
    ESP_LOGI(TAG, "Starting firmware upgrade from URL: %s", upgrade_url.c_str());

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
        ESP_LOGE(TAG, "Firmware upgrade failed, restarting audio service and continuing operation...");
        audio_service_.Start(); // 重新创建并启动音频后台任务。
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER); // 恢复设备空闲时的功耗策略。
        Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        vTaskDelay(pdMS_TO_TICKS(3000));
        return false;
    } else {
        // 新镜像已校验并设置为启动分区，立即重启完成切换。
        ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
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

    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();
    
    if (state == kDeviceStateIdle) {
        if (!protocol_->IsAudioChannelOpened()) {
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
            if (protocol_) {
                protocol_->CloseAudioChannel();
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

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
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
        if (protocol_) {
            protocol_->SendMcpMessage(payload);
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
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
        // 通道关闭后再释放协议对象及其网络资源。
        protocol_.reset();
    });
}
