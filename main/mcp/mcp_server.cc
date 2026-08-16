/**
 * @file mcp_server.cc
 * @brief MCP 工具注册、参数校验、调用和应答实现。
 */
/*
 * MCP Server Implementation
 * Reference: https://modelcontextprotocol.io/specification/2024-11-05
 */

#include "mcp/mcp_server.h"
#include <esp_log.h>
#include <esp_app_desc.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <esp_pthread.h>

#include "app/application.h"
#include "display.h"
#include "board.h"
#include "system/settings.h"

#define TAG "MCP"

/**
 * @brief 构造 McpServer 对象并初始化该模块运行所需的成员和系统资源。
 * @details 构造阶段只建立本模块自身资源；需要异步运行的任务由后续 Start 或 Initialize 方法启动。
 */
McpServer::McpServer() {
}

/**
 * @brief 析构 McpServer 对象并释放其持有的系统资源。
 * @details 释放顺序与创建顺序相反，先停止异步来源，再销毁句柄和动态内存，避免回调访问失效对象。
 */
McpServer::~McpServer() {
    std::lock_guard<std::mutex> lock(tools_mutex_);
    tools_.clear();
}

/**
 * @brief 注册音量、亮度、设备状态等可供模型调用的通用工具。
 * @details 常用工具会被移动到列表前部，以提高云端提示词缓存命中率。工具回调只捕获生命周期
 * 稳定的板级单例或显示对象，并在注册完成后把原有自定义工具接回列表尾部。
 */
void McpServer::AddCommonTools() {
    // 为提高云端提示词缓存命中率，常用工具固定放在工具列表前部。

    // 暂存板级自定义工具，通用工具注册完成后再追加回列表尾部。
    auto original_tools = std::move(tools_);
    auto& board = Board::GetInstance();

    // 板级自定义工具必须在对应板子的 InitializeTools() 中注册，避免公共层依赖具体硬件。

    AddTool("self.get_device_status",
        "Provides the real-time information of the device, including the current status of the audio speaker, screen, battery, network, etc.\n"
        "Use this tool for: \n"
        "1. Answering questions about current condition (e.g. what is the current volume of the audio speaker?)\n"
        "2. As the first step to control the device (e.g. turn up / down the volume of the audio speaker, etc.)",
        PropertyList(),
        [&board](const PropertyList& properties) -> ReturnValue {
            return board.GetDeviceStatusJson();
        });

    AddTool("self.audio_speaker.set_volume", 
        "Set the volume of the audio speaker. If the current volume is unknown, you must call `self.get_device_status` tool first and then call this tool.",
        PropertyList({
            Property("volume", kPropertyTypeInteger, 0, 100)
        }), 
        [&board](const PropertyList& properties) -> ReturnValue {
            auto codec = board.GetAudioCodec();
            codec->SetOutputVolume(properties["volume"].value<int>());
            board.WakeUpScreen();
            return true;
        });
    
    auto backlight = board.GetBacklight();
    if (backlight) {
        AddTool("self.screen.set_brightness",
            "Set the brightness of the screen.",
            PropertyList({
                Property("brightness", kPropertyTypeInteger, 0, 100)
            }),
            [&board, backlight](const PropertyList& properties) -> ReturnValue {
                uint8_t brightness = static_cast<uint8_t>(properties["brightness"].value<int>());
                board.WakeUpScreen();
                backlight->SetBrightness(brightness, true);
                return true;
            });

        AddTool("self.screen.set_screensaver_enabled",
            "Enable or disable the clock-face screensaver. When enabled, the screensaver appears after "
            "30 seconds without user interaction. This setting does not control automatic screen-off.",
            PropertyList({
                Property("enabled", kPropertyTypeBoolean)
            }),
            [&board](const PropertyList& properties) -> ReturnValue {
                return board.SetScreensaverEnabled(properties["enabled"].value<bool>());
            });

        AddTool("self.screen.set_auto_off_enabled",
            "Enable or disable automatic screen backlight-off. The delay is configured separately by "
            "self.screen.set_auto_off_timeout. Disabling automatic screen-off keeps the display illuminated.",
            PropertyList({
                Property("enabled", kPropertyTypeBoolean)
            }),
            [&board](const PropertyList& properties) -> ReturnValue {
                return board.SetScreenAutoOffEnabled(properties["enabled"].value<bool>());
            });

        AddTool("self.screen.set_auto_off_timeout",
            "Set how many seconds after the last user interaction the screen backlight turns off. "
            "A positive value also enables automatic screen-off. Use 0 to disable automatic screen-off. "
            "Valid non-zero values are 5 to 3600 seconds. This setting is independent of the screensaver.",
            PropertyList({
                Property("seconds", kPropertyTypeInteger, 0, 3600)
            }),
            [&board](const PropertyList& properties) -> ReturnValue {
                int seconds = properties["seconds"].value<int>();
                if (seconds != 0 && seconds < 5) {
                    throw std::invalid_argument("seconds must be 0 or between 5 and 3600");
                }
                return board.SetScreenAutoOffTimeout(seconds);
            });
    }

    auto display = board.GetDisplay();
    if (display && display->GetTheme() != nullptr) {
        AddTool("self.screen.set_theme",
            "Set the theme of the screen. The theme can be `light` or `dark`.",
            PropertyList({
                Property("theme", kPropertyTypeString)
            }),
            [&board, display](const PropertyList& properties) -> ReturnValue {
                auto theme_name = properties["theme"].value<std::string>();
                if (display->SetThemeByName(theme_name)) {
                    board.WakeUpScreen();
                    return true;
                }
                return false;
            });
    }

    // 恢复原工具列表，并保持通用工具优先的稳定顺序。
    tools_.insert(tools_.end(), original_tools.begin(), original_tools.end());
}

/**
 * @brief 注册仅供用户界面直接调用的工具。
 * @details 这些工具包含系统信息、重启、固件升级、屏幕操作和资源地址设置等管理能力，
 * 会标记为 user_only，默认不出现在提供给大模型的 tools/list 结果中。
 */
void McpServer::AddUserOnlyTools() {
    // 系统工具用于查询设备信息和执行受控重启。
    AddUserOnlyTool("self.get_system_info",
        "Get the system information",
        PropertyList(),
        [this](const PropertyList& properties) -> ReturnValue {
            auto& board = Board::GetInstance();
            return board.GetSystemInfoJson();
        });

    AddUserOnlyTool("self.reboot", "Reboot the system",
        PropertyList(),
        [this](const PropertyList& properties) -> ReturnValue {
            auto& app = Application::GetInstance();
            app.Schedule([&app]() {
                ESP_LOGW(TAG, "用户请求重启设备");
                vTaskDelay(pdMS_TO_TICKS(1000));

                app.Reboot();
            });
            return true;
        });

    // 固件升级实际在 Application 主任务中执行，避免阻塞 MCP 网络接收线程。
    AddUserOnlyTool("self.upgrade_firmware", "Upgrade firmware from a specific URL. This will download and install the firmware, then reboot the device.",
        PropertyList({
            Property("url", kPropertyTypeString, "The URL of the firmware binary file to download and install")
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            auto url = properties["url"].value<std::string>();
            ESP_LOGI(TAG, "用户请求从指定地址升级固件，地址=%s", url.c_str());
            
            auto& app = Application::GetInstance();
            app.Schedule([url, &app]() {
                bool success = app.UpgradeFirmware(url);
                if (!success) {
                    ESP_LOGE(TAG, "固件升级失败");
                }
            });
            
            return true;
        });

    auto display = Board::GetInstance().GetDisplay();
    if (display != nullptr && display->width() > 0 && display->height() > 0) {
        AddUserOnlyTool("self.screen.get_info", "Information about the screen, including width, height, etc.",
            PropertyList(),
            [display](const PropertyList& properties) -> ReturnValue {
                cJSON *json = cJSON_CreateObject();
                cJSON_AddNumberToObject(json, "width", display->width());
                cJSON_AddNumberToObject(json, "height", display->height());
                cJSON_AddBoolToObject(json, "monochrome", false);
                return json;
            });

#if CONFIG_LV_USE_SNAPSHOT
        AddUserOnlyTool("self.screen.snapshot", "Snapshot the screen and upload it to a specific URL",
            PropertyList({
                Property("url", kPropertyTypeString),
                Property("quality", kPropertyTypeInteger, 80, 1, 100)
            }),
            [display](const PropertyList& properties) -> ReturnValue {
                auto url = properties["url"].value<std::string>();
                auto quality = properties["quality"].value<int>();

                std::string jpeg_data;
                if (!display->SnapshotToJpeg(jpeg_data, quality)) {
                    throw std::runtime_error("Failed to snapshot screen");
                }

                ESP_LOGI(TAG, "正在上传屏幕截图，字节数=%u，地址=%s", jpeg_data.size(), url.c_str());
                
                // 构造 multipart/form-data 请求体。
                std::string boundary = "----ESP32_SCREEN_SNAPSHOT_BOUNDARY";
                
                auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
                http->SetHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
                if (!http->Open("POST", url)) {
                    throw std::runtime_error("Failed to open URL: " + url);
                }
                {
                    // 文件字段头部
                    std::string file_header;
                    file_header += "--" + boundary + "\r\n";
                    file_header += "Content-Disposition: form-data; name=\"file\"; filename=\"screenshot.jpg\"\r\n";
                    file_header += "Content-Type: image/jpeg\r\n";
                    file_header += "\r\n";
                    http->Write(file_header.c_str(), file_header.size());
                }

                // 写入 JPEG 文件数据本体。
                http->Write((const char*)jpeg_data.data(), jpeg_data.size());

                {
                    // 写入 multipart 结束边界。
                    std::string multipart_footer;
                    multipart_footer += "\r\n--" + boundary + "--\r\n";
                    http->Write(multipart_footer.c_str(), multipart_footer.size());
                }
                http->Write("", 0);

                if (http->GetStatusCode() != 200) {
                    throw std::runtime_error("Unexpected status code: " + std::to_string(http->GetStatusCode()));
                }
                std::string result = http->ReadAll();
                http->Close();
                ESP_LOGI(TAG, "屏幕截图上传结果=%s", result.c_str());
                return true;
            });
        
        AddUserOnlyTool("self.screen.preview_image", "Preview an image on the screen",
            PropertyList({
                Property("url", kPropertyTypeString)
            }),
            [display](const PropertyList& properties) -> ReturnValue {
                auto url = properties["url"].value<std::string>();
                auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);

                if (!http->Open("GET", url)) {
                    throw std::runtime_error("Failed to open URL: " + url);
                }
                int status_code = http->GetStatusCode();
                if (status_code != 200) {
                    throw std::runtime_error("Unexpected status code: " + std::to_string(status_code));
                }

                size_t content_length = http->GetBodyLength();
                char* data = (char*)heap_caps_malloc(content_length, MALLOC_CAP_8BIT);
                if (data == nullptr) {
                    throw std::runtime_error("Failed to allocate memory for image: " + url);
                }
                size_t total_read = 0;
                while (total_read < content_length) {
                    int ret = http->Read(data + total_read, content_length - total_read);
                    if (ret < 0) {
                        heap_caps_free(data);
                        throw std::runtime_error("Failed to download image: " + url);
                    }
                    if (ret == 0) {
                        break;
                    }
                    total_read += ret;
                }
                http->Close();

                if (!display->SetPreviewImageData(data, content_length)) {
                    throw std::runtime_error("Failed to preview image: " + url);
                }
                return true;
            });
#endif // CONFIG_LV_USE_SNAPSHOT
    }

    // 资源下载地址始终可写入 Settings，不依赖当前分区表是否启用了资源分区。
    AddUserOnlyTool("self.assets.set_download_url", "Set the download url for the assets",
            PropertyList({
                Property("url", kPropertyTypeString)
            }),
            [](const PropertyList& properties) -> ReturnValue {
                auto url = properties["url"].value<std::string>();
                Settings settings("assets", true);
                settings.SetString("download_url", url);
                return true;
            });
}

/**
 * @brief 接管动态创建的工具指针并立即转换为 shared_ptr。
 * @param tool 使用 new 创建的工具对象；传入后调用方不再持有所有权。
 * @details 注册前按名称检查重复项。重复工具不会加入列表，其 shared_ptr 会在返回时自动释放，
 * 避免原实现重复注册路径泄漏堆对象。
 */
void McpServer::AddTool(McpTool* tool) {
    if (tool == nullptr) {
        return;
    }
    std::shared_ptr<McpTool> owned_tool(tool);
    std::lock_guard<std::mutex> lock(tools_mutex_);
    // 工具名称必须唯一，防止生成重复定义和调用歧义。
    if (std::find_if(tools_.begin(), tools_.end(), [&owned_tool](const auto& existing) {
            return existing->name() == owned_tool->name();
        }) != tools_.end()) {
        ESP_LOGW(TAG, "工具已经添加，工具名称=%s", tool->name().c_str());
        return;
    }

    ESP_LOGI(TAG, "添加工具，工具名称=%s%s", tool->name().c_str(), tool->user_only() ? " [仅用户]" : "");
    tools_.push_back(std::move(owned_tool));
}

/**
 * @brief 接管动态创建的工具指针，析构时统一释放。
 * @param name MCP 工具的全局唯一名称。
 * @param description 提供给调用方的工具用途说明。
 * @param properties 工具参数定义及默认值。
 * @param callback 参数校验通过后执行的工具回调。
 * @details 根据传入元数据创建 McpTool，并把对象所有权交给指针重载统一管理。
 */
void McpServer::AddTool(const std::string& name, const std::string& description, const PropertyList& properties, std::function<ReturnValue(const PropertyList&)> callback) {
    AddTool(new McpTool(name, description, properties, callback));
}

/**
 * @brief 注册由后台 Worker 执行的异步 MCP 工具。
 * @param name MCP 工具的全局唯一名称。
 * @param description 提供给大模型的用途、参数语义和确认规则。
 * @param properties 工具输入参数定义。
 * @param callback 接收已校验参数并负责最终调用完成回调的异步启动函数。
 * @details 本方法只创建工具描述对象，不创建任务。具体 Worker 的队列和生命周期由业务模块管理。
 */
void McpServer::AddAsyncTool(const std::string& name, const std::string& description,
                             const PropertyList& properties, AsyncMcpToolCallback callback) {
    AddTool(new McpTool(name, description, properties, std::move(callback)));
}

/**
 * @brief 在互斥锁保护下替换全部动态工具，并保留静态工具稳定顺序。
 * @param definitions 后端权威清单转换出的动态工具定义。
 * @return 所有名称均不与静态工具冲突时返回 true。
 * @details 旧动态工具由 shared_ptr 延迟释放，已经排入 Application 主任务的调用仍可安全完成。
 */
bool McpServer::ReplaceDynamicTools(std::vector<McpDynamicToolDefinition> definitions) {
    const auto definition_count = definitions.size();
    {
        std::lock_guard<std::mutex> lock(tools_mutex_);
        for (const auto& definition : definitions) {
            const bool conflicts = std::any_of(tools_.begin(), tools_.end(), [&definition](const auto& tool) {
                return !tool->dynamic() && tool->name() == definition.name;
            });
            if (conflicts) {
                ESP_LOGE(TAG, "动态工具与内置工具名称冲突，工具名称=%s", definition.name.c_str());
                return false;
            }
        }

        tools_.erase(
            std::remove_if(tools_.begin(), tools_.end(), [](const auto& tool) {
                return tool->dynamic();
            }),
            tools_.end());

        auto insertion = std::find_if(tools_.begin(), tools_.end(), [](const auto& tool) {
            return tool->user_only();
        });
        for (auto& definition : definitions) {
            auto tool = std::make_shared<McpTool>(
                definition.name,
                definition.description,
                definition.properties,
                definition.input_schema_json,
                std::move(definition.callback));
            insertion = tools_.insert(insertion, std::move(tool));
            ++insertion;
        }
    }
    ESP_LOGI(TAG, "动态 MCP 工具已替换，数量=%u",
             static_cast<unsigned>(definition_count));
    NotifyToolsListChanged();
    return true;
}

/**
 * @brief 发送 MCP 标准工具清单变更通知。
 * @details Application 会把通知切换到主任务，并通过当前有效协议会话发送。没有活动会话时通知可以
 * 安全丢弃；下一次 MCP initialize 和 tools/list 会直接读取已经替换后的最新清单。
 */
void McpServer::NotifyToolsListChanged() {
    ESP_LOGI(TAG, "正在通知 MCP 客户端工具清单已变化");
    Application::GetInstance().SendMcpMessage(
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/tools/list_changed\"}");
}

/**
 * @brief 注册带 audience=user 注解的工具。
 * @param name MCP 工具的全局唯一名称。
 * @param description 提供给用户界面的工具用途说明。
 * @param properties 工具参数定义及默认值。
 * @param callback 参数校验通过后执行的工具回调。
 * @details 创建工具后设置 user_only 标志，使其仅在明确请求 withUserTools 时出现在工具列表中。
 */
void McpServer::AddUserOnlyTool(const std::string& name, const std::string& description, const PropertyList& properties, std::function<ReturnValue(const PropertyList&)> callback) {
    auto tool = new McpTool(name, description, properties, callback);
    tool->set_user_only(true);
    AddTool(tool);
}

/**
 * @brief 从文本解析并处理一条 MCP JSON-RPC 消息。
 * @param message 完整的 UTF-8 JSON 文本。
 * @details 创建临时 cJSON 文档并转交对象重载处理；无论处理结果如何，本方法都会释放解析树。
 * JSON 语法错误只记录日志，不向无法识别编号的请求发送错误应答。
 */
void McpServer::ParseMessage(const std::string& message) {
    cJSON* json = cJSON_Parse(message.c_str());
    if (json == nullptr) {
        ESP_LOGE(TAG, "MCP 消息解析失败，长度=%u",
                 static_cast<unsigned>(message.size()));
        return;
    }
    ParseMessage(json);
    cJSON_Delete(json);
}

/**
 * @brief 校验并分发已解析的 MCP JSON-RPC 请求。
 * @param json JSON-RPC 根对象，仅在本次调用期间有效，调用者保留所有权。
 * @details 依次校验协议版本、方法、参数和数字请求编号；通知类消息无需应答。当前支持 initialize、
 * tools/list 和 tools/call，其他方法返回 JSON-RPC error。
 */
void McpServer::ParseMessage(const cJSON* json) {
    // 只接受 MCP 当前使用的 JSON-RPC 2.0 消息。
    auto version = cJSON_GetObjectItem(json, "jsonrpc");
    if (version == nullptr || !cJSON_IsString(version) || strcmp(version->valuestring, "2.0") != 0) {
        ESP_LOGE(TAG, "JSON-RPC 版本无效，版本=%s", version ? version->valuestring : "空");
        return;
    }
    
    // method 决定初始化、工具列表或工具调用等后续分支。
    auto method = cJSON_GetObjectItem(json, "method");
    if (method == nullptr || !cJSON_IsString(method)) {
        ESP_LOGE(TAG, "MCP 消息缺少方法名称");
        return;
    }
    
    auto method_str = std::string(method->valuestring);
    if (method_str.find("notifications") == 0) {
        return;
    }
    
    // params 可省略，但存在时必须是 JSON 对象。
    auto params = cJSON_GetObjectItem(json, "params");
    if (params != nullptr && !cJSON_IsObject(params)) {
        ESP_LOGE(TAG, "MCP 方法参数无效，方法=%s", method_str.c_str());
        return;
    }

    auto id = cJSON_GetObjectItem(json, "id");
    if (id == nullptr || !cJSON_IsNumber(id)) {
        ESP_LOGE(TAG, "MCP 方法请求编号无效，方法=%s", method_str.c_str());
        return;
    }
    auto id_int = id->valueint;
    
    if (method_str == "initialize") {
        auto app_desc = esp_app_get_description();
        std::string message = "{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"tools\":{\"listChanged\":true}},\"serverInfo\":{\"name\":\"" BOARD_NAME "\",\"version\":\"";
        message += app_desc->version;
        message += "\"}}";
        ReplyResult(id_int, message);
    } else if (method_str == "tools/list") {
        std::string cursor_str = "";
        bool list_user_only_tools = false;
        if (params != nullptr) {
            auto cursor = cJSON_GetObjectItem(params, "cursor");
            if (cJSON_IsString(cursor)) {
                cursor_str = std::string(cursor->valuestring);
            }
            auto with_user_tools = cJSON_GetObjectItem(params, "withUserTools");
            if (cJSON_IsBool(with_user_tools)) {
                list_user_only_tools = with_user_tools->valueint == 1;
            }
        }
        GetToolsList(id_int, cursor_str, list_user_only_tools);
    } else if (method_str == "tools/call") {
        if (!cJSON_IsObject(params)) {
            ESP_LOGE(TAG, "工具调用缺少参数对象");
            ReplyError(id_int, "Missing params");
            return;
        }
        auto tool_name = cJSON_GetObjectItem(params, "name");
        if (!cJSON_IsString(tool_name)) {
            ESP_LOGE(TAG, "工具调用缺少工具名称");
            ReplyError(id_int, "Missing name");
            return;
        }
        auto tool_arguments = cJSON_GetObjectItem(params, "arguments");
        if (tool_arguments != nullptr && !cJSON_IsObject(tool_arguments)) {
            ESP_LOGE(TAG, "工具调用参数无效");
            ReplyError(id_int, "Invalid arguments");
            return;
        }
        DoToolCall(id_int, std::string(tool_name->valuestring), tool_arguments);
    } else {
        ESP_LOGE(TAG, "MCP 方法尚未实现，方法=%s", method_str.c_str());
        ReplyError(id_int, "Method not implemented: " + method_str);
    }
}

/**
 * @brief 回复 JSON-RPC result。
 * @param id 请求编号。
 * @param result 已序列化结果对象。
 * @details result 必须已经是合法 JSON 值，本方法不会再次转义或加引号。组装后的完整应答通过
 * Application 转交当前云端协议发送。
 */
void McpServer::ReplyResult(int id, const std::string& result) {
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
    payload += std::to_string(id) + ",\"result\":";
    payload += result;
    payload += "}";
    Application::GetInstance().SendMcpMessage(payload);
}

/**
 * @brief 回复 JSON-RPC error。
 * @param id 请求编号。
 * @param message 可诊断错误说明。
 * @details 将错误文本放入 error.message 后通过当前云端协议发送。调用者应避免在 message 中传入
 * 未转义的引号等 JSON 特殊字符。
 */
void McpServer::ReplyError(int id, const std::string& message) {
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
    payload += std::to_string(id);
    payload += ",\"error\":{\"message\":\"";
    payload += message;
    payload += "\"}}";
    Application::GetInstance().SendMcpMessage(payload);
}

/**
 * @brief 将异步业务结果转换为符合 MCP tools/call 的 result 对象。
 * @param id 原始 JSON-RPC 数字请求编号。
 * @param result 后台 Worker 返回的 UTF-8 文本和 isError 标志。
 * @details 使用 cJSON 完成字符串转义，避免天气描述、备忘录或错误文本中的引号破坏协议结构。
 */
void McpServer::ReplyToolResult(int id, const McpToolResult& result) {
    cJSON* root = cJSON_CreateObject();
    cJSON* content = cJSON_CreateArray();
    cJSON* text = cJSON_CreateObject();
    cJSON_AddStringToObject(text, "type", "text");
    cJSON_AddStringToObject(text, "text", result.text.c_str());
    cJSON_AddItemToArray(content, text);
    cJSON_AddItemToObject(root, "content", content);
    cJSON_AddBoolToObject(root, "isError", result.is_error);

    char* serialized = cJSON_PrintUnformatted(root);
    if (serialized != nullptr) {
        ReplyResult(id, serialized);
        cJSON_free(serialized);
    } else {
        ReplyError(id, "Failed to serialize asynchronous tool result");
    }
    cJSON_Delete(root);
}

/**
 * @brief 分页返回工具列表。
 * @param id tools/list 请求编号。
 * @param cursor 起始游标。
 * @param list_user_only_tools 是否把标记为 user_only 的工具包含在结果中。
 * @details 从 cursor 指定的工具开始序列化，单条响应限制为 8000 字节。达到限制时通过 nextCursor
 * 返回下一页起点；默认过滤 user_only 管理工具，避免向大模型暴露重启和升级等管理能力。
 */
void McpServer::GetToolsList(int id, const std::string& cursor, bool list_user_only_tools) {
    std::lock_guard<std::mutex> lock(tools_mutex_);
    const int max_payload_size = 8000;
    std::string json = "{\"tools\":[";
    
    bool found_cursor = cursor.empty();
    auto it = tools_.begin();
    std::string next_cursor = "";
    
    while (it != tools_.end()) {
        // 尚未到达游标指定位置时跳过前面的工具。
        if (!found_cursor) {
            if ((*it)->name() == cursor) {
                found_cursor = true;
            } else {
                ++it;
                continue;
            }
        }

        if (!list_user_only_tools && (*it)->user_only()) {
            ++it;
            continue;
        }
        
        // 添加工具前预留 JSON 结尾和 nextCursor 所需空间。
        std::string tool_json = (*it)->to_json() + ",";
        if (json.length() + tool_json.length() + 30 > max_payload_size) {
            // 当前工具放不下时把它作为下一页起点，本页不消费该工具。
            next_cursor = (*it)->name();
            break;
        }
        
        json += tool_json;
        ++it;
    }
    
    if (json.back() == ',') {
        json.pop_back();
    }
    
    if (json.back() == '[' && !tools_.empty()) {
        // 单个工具自身就超过负载上限时无法分页，直接返回可诊断错误。
        ESP_LOGE(TAG, "工具清单超过负载限制，无法添加工具，工具名称=%s", next_cursor.c_str());
        ReplyError(id, "Failed to add tool " + next_cursor + " because of payload size limit");
        return;
    }

    if (next_cursor.empty()) {
        json += "]}";
    } else {
        json += "],\"nextCursor\":\"" + next_cursor + "\"}";
    }

    ESP_LOGI(
        TAG,
        "工具清单已返回，游标=%s，包含用户工具=%d，下一游标=%s，负载字节数=%u",
        cursor.empty() ? "<首页>" : cursor.c_str(),
        list_user_only_tools ? 1 : 0,
        next_cursor.empty() ? "<末页>" : next_cursor.c_str(),
        static_cast<unsigned>(json.size()));
    
    ReplyResult(id, json);
}

/**
 * @brief 查找工具、解析和校验参数，再执行工具回调。
 * @param id tools/call 请求编号。
 * @param tool_name 需要调用的已注册工具名称。
 * @param tool_arguments 参数 JSON 对象，可为空指针表示未提供参数。
 * @details 先复制工具的参数定义，再按布尔、整数和字符串类型读取实参；缺少无默认值的参数时立即
 * 返回错误。实际工具回调统一调度到应用主线程，避免网络接收线程直接操作显示、Codec 或 NVS。
 */
void McpServer::DoToolCall(int id, const std::string& tool_name, const cJSON* tool_arguments) {
    /*
     * 只记录工具名称，不记录参数。天气位置、备忘录正文等业务参数可能包含用户隐私，
     * 但工具名称能够明确区分云端是否真正调用了设备 MCP。
     */
    ESP_LOGI(TAG, "调用工具，工具名称=%s", tool_name.c_str());

    std::shared_ptr<McpTool> tool;
    {
        std::lock_guard<std::mutex> lock(tools_mutex_);
        auto tool_iter = std::find_if(tools_.begin(), tools_.end(),
                                      [&tool_name](const auto& candidate) {
                                          return candidate->name() == tool_name;
                                      });
        if (tool_iter != tools_.end()) {
            tool = *tool_iter;
        }
    }

    if (!tool) {
        ESP_LOGE(TAG, "工具不存在，工具名称=%s", tool_name.c_str());
        ReplyError(id, "Unknown tool: " + tool_name);
        return;
    }

    const bool end_conversation_after_response = tool->dynamic();
    const auto reply_error = [this, id, end_conversation_after_response](const std::string& message) {
        if (end_conversation_after_response) {
            Application::GetInstance().RequestConversationEndAfterSpeaking();
        }
        ReplyError(id, message);
    };

    PropertyList arguments = tool->properties();
    try {
        for (auto& argument : arguments) {
            bool found = false;
            bool provided = false;
            if (cJSON_IsObject(tool_arguments)) {
                auto value = cJSON_GetObjectItem(tool_arguments, argument.name().c_str());
                provided = value != nullptr;
                if (argument.type() == kPropertyTypeBoolean && cJSON_IsBool(value)) {
                    argument.set_value<bool>(value->valueint == 1);
                    found = true;
                } else if (argument.type() == kPropertyTypeInteger && cJSON_IsNumber(value)
                           && std::floor(value->valuedouble) == value->valuedouble
                           && value->valuedouble >= std::numeric_limits<int>::min()
                           && value->valuedouble <= std::numeric_limits<int>::max()) {
                    argument.set_value<int>(static_cast<int>(value->valuedouble));
                    found = true;
                } else if (argument.type() == kPropertyTypeString && cJSON_IsString(value)) {
                    argument.set_value<std::string>(value->valuestring);
                    found = true;
                }
            }

            if (provided && !found) {
                ESP_LOGE(TAG, "工具参数类型无效，参数名称=%s", argument.name().c_str());
                reply_error("Invalid argument type: " + argument.name());
                return;
            }
            if (argument.required() && !argument.has_default_value() && !found) {
                ESP_LOGE(TAG, "工具缺少有效参数，参数名称=%s", argument.name().c_str());
                reply_error("Missing valid argument: " + argument.name());
                return;
            }
        }
        if (tool->dynamic() && cJSON_IsObject(tool_arguments)) {
            cJSON* supplied = nullptr;
            cJSON_ArrayForEach(supplied, tool_arguments) {
                const std::string supplied_name = supplied->string == nullptr
                    ? std::string()
                    : std::string(supplied->string);
                const bool known = std::any_of(arguments.begin(), arguments.end(), [&supplied_name](const Property& property) {
                    return property.name() == supplied_name;
                });
                if (!known) {
                    ESP_LOGE(TAG, "动态工具包含未知参数，参数名称=%s", supplied_name.c_str());
                    reply_error("Unknown argument: " + supplied_name);
                    return;
                }
            }
        }
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "工具调用参数处理异常，原因=%s", e.what());
        reply_error(e.what());
        return;
    }

    // 硬件和界面工具统一从应用主线程启动，避免网络收包任务直接访问板级资源。
    auto& app = Application::GetInstance();
    app.Schedule([this, id, tool = std::move(tool), arguments = std::move(arguments),
                  end_conversation_after_response]() {
        try {
            if (tool->is_async()) {
                tool->CallAsync(arguments, [this, id, end_conversation_after_response](McpToolResult result) {
                    if (end_conversation_after_response) {
                        Application::GetInstance().RequestConversationEndAfterSpeaking();
                    }
                    ReplyToolResult(id, result);
                });
            } else {
                if (end_conversation_after_response) {
                    Application::GetInstance().RequestConversationEndAfterSpeaking();
                }
                ReplyResult(id, tool->Call(arguments));
            }
        } catch (const std::exception& e) {
            ESP_LOGE(TAG, "工具执行异常，原因=%s", e.what());
            if (end_conversation_after_response) {
                Application::GetInstance().RequestConversationEndAfterSpeaking();
            }
            ReplyError(id, e.what());
        }
    });
}
