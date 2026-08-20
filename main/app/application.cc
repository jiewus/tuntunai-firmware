/**
 * @file application.cc
 * @brief 应用初始化、主事件循环和联网激活流程实现。
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
 * 网络回调。最后异步启动网络，因此所有网络结果都通过事件组交给 Run() 处理。
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
        (void)wake_word;
        xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
    };
    callbacks.on_vad_change = [this](bool speaking) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
    };
    callbacks.on_playback_drained = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_PLAYBACK_DRAINED);
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

    /**
     * 注册囤囤AI的设备绑定工具；天气同步由 BackendService 根据屏保生命周期自动执行，
     * 不暴露为需要大模型主动调用的 MCP 工具。
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
 * 调度队列中的闭包也在此处串行执行，从而把跨线程回调转换为单线程业务状态变更。
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
        MAIN_EVENT_STATE_CHANGED |
        MAIN_EVENT_PLAYBACK_DRAINED;

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

        if (bits & MAIN_EVENT_PLAYBACK_DRAINED) {
            // 自动停止模式下延迟的开始监听：下行播放已排空，此时可安全启动语音处理。
            if (pending_listening_start_ && GetDeviceState() == kDeviceStateListening &&
                audio_service_.IsPlaybackIdle()) {
                pending_listening_start_ = false;
                StartListeningAudio();
            }
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
                if (xiaozhi_client_ && !xiaozhi_client_->SendAudio(std::move(packet))) {
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
    ESP_LOGI(TAG, "网络已连接");
    auto state = GetDeviceState();

    if (state == kDeviceStateStarting || state == kDeviceStateWifiConfiguring) {
        // 网络就绪后启动后台激活任务，避免阻塞主事件循环。
        SetDeviceState(kDeviceStateActivating);
        if (activation_task_handle_ != nullptr) {
            ESP_LOGW(TAG, "设备激活任务已在运行");
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
        ESP_LOGI(TAG, "网络已断开，正在关闭音频通道");
        xiaozhi_client_->CloseAudioChannel();
    }

    // 断网后立即刷新状态栏网络图标。
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

/**
 * @brief 完成开机激活流程并切换到可唤醒的屏保待机状态。
 * @details 本方法运行在应用主循环中。它先进入空闲状态、保存服务端时间状态、释放 OTA 临时
 * 对象并恢复低功耗等级，再排队播放启动成功提示音，最后在用户启用屏保时立即显示表盘。
 */
void Application::HandleActivationDoneEvent() {
    ESP_LOGI(TAG, "设备激活完成");

    SystemInfo::PrintHeapStats();
    SetDeviceState(kDeviceStateIdle);

    has_server_time_ = ota_ != nullptr && ota_->HasServerTime();

    auto display = Board::GetInstance().GetDisplay();
    std::string message = std::string(Lang::Strings::VERSION) +
        (ota_ != nullptr ? ota_->GetCurrentVersion() : SystemInfo::GetUserAgent());
    display->ShowNotification(message.c_str());
    display->SetChatMessage("system", "");

    ota_.reset();
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);

    Schedule([this]() {
        // 播放成功提示音，表示设备已进入可唤醒状态。
        audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS);
    });

    /*
     * 此处代表资源应用和协议初始化均已完成。直接进入表盘可以
     * 跳过普通待机的 30 秒等待；板级实现仍会检查用户是否已经关闭屏保功能。
     */
    board.EnterScreensaver();
}

/**
 * @brief 后台执行启动初始化，完成后设置 MAIN_EVENT_ACTIVATION_DONE。
 * @details 该任务按顺序检查固件、资源包并创建云端协议对象。所有耗时网络操作
 * 均在后台任务中执行，完成后只通过事件位通知主循环更新设备状态。
 */
void Application::ActivationTask() {
    ota_ = std::make_unique<Ota>();

    // 自有 OTA 检查失败只记录日志并继续启动，不阻塞语音协议初始化。
    CheckNewVersion();

    // 优先更新界面资源，确保后续状态页面使用与固件匹配的资源版本。
    CheckAssetsVersion();

    // 根据 OTA 下发或 NVS 中已保存的配置启动语音协议。
    InitializeXiaozhiClient();

    // 通过事件组回到主循环收尾，避免后台任务直接修改前台状态。
    xEventGroupSetBits(event_group_, MAIN_EVENT_ACTIVATION_DONE);
}

/**
 * @brief 检查并应用待更新的界面资源包。
 * @details 本方法每次启动只执行一次。若 NVS 中存在下载地址，则临时切换到升级状态和高性能模式，
 * 下载资源分区并显示进度；下载失败时恢复激活状态，成功或无需下载时统一应用现有资源。
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
        ESP_LOGW(TAG, "板型 %s 未启用资源分区", BOARD_NAME);
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
 * @brief 检查自有 OTA 版本、协议配置和设备激活状态。
 * @details 每次启动最多请求一次自有 OTA 服务。请求失败时只记录警告并跳过，不提示用户、不重试，
 * 继续使用 NVS 中已有配置启动语音协议。发现新版本时执行静默升级，失败后继续运行当前固件。
 */
void Application::CheckNewVersion() {
    if (ota_ == nullptr) {
        ESP_LOGW(TAG, "OTA 管理器未初始化，跳过自有 OTA 检查");
        return;
    }

    const esp_err_t check_error = ota_->CheckVersion();
    if (check_error != ESP_OK) {
        ESP_LOGW(TAG, "自有 OTA 版本检查跳过，错误码=%d", static_cast<int>(check_error));
        return;
    }

    if (ota_->HasNewVersion()) {
        auto& board = Board::GetInstance();
        ESP_LOGI(TAG, "发现自有 OTA 新版本=%s，开始静默升级", ota_->GetFirmwareVersion().c_str());
        SetDeviceState(kDeviceStateUpgrading);
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        audio_service_.Stop();

        const bool success = ota_->StartUpgrade(nullptr);
        if (success) {
            ESP_LOGI(TAG, "自有 OTA 升级成功，设备即将重启");
            Reboot();
            return;
        }

        ESP_LOGW(TAG, "自有 OTA 下载或写入失败，保留当前固件并继续启动");
        audio_service_.Start();
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        SetDeviceState(kDeviceStateActivating);
    }

    ota_->MarkCurrentVersionValid();
    if (!ota_->HasActivationCode() && !ota_->HasActivationChallenge()) {
        return;
    }

    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(Lang::Strings::ACTIVATION);
    if (ota_->HasActivationCode()) {
        ShowActivationCode(ota_->GetActivationCode(), ota_->GetActivationMessage());
    }

    for (int i = 0; i < 10; ++i) {
        ESP_LOGI(TAG, "正在激活设备（%d/%d）", i + 1, 10);
        const esp_err_t activate_error = ota_->Activate();
        if (activate_error == ESP_OK) {
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(activate_error == ESP_ERR_TIMEOUT ? 3000 : 10000));
        if (GetDeviceState() == kDeviceStateIdle) {
            break;
        }
    }
}
