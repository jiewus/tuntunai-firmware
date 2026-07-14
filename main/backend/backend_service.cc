/**
 * @file backend_service.cc
 * @brief 新版业务 API 完成前的无网络后端占位实现。
 */

#include "backend_service.h"

#include <esp_log.h>

#include "boards/common/board.h"
#include "display/display.h"

namespace {

/**
 * @brief 后端占位服务使用的日志标签。
 */
constexpr const char* kTag = "BackendPlaceholder";

/**
 * @brief 屏保天气位置区域显示的临时说明。
 */
constexpr const char* kWeatherLocationPlaceholder = "天气服务开发中";

/**
 * @brief 屏保当前温度区域显示的临时内容。
 */
constexpr const char* kWeatherTemperaturePlaceholder = "--℃";

/**
 * @brief 屏保天气描述区域显示的临时内容。
 */
constexpr const char* kWeatherDescriptionPlaceholder = "--";

/**
 * @brief 屏保最低和最高温度区域显示的临时内容。
 */
constexpr const char* kWeatherRangePlaceholder = "--/--";

/**
 * @brief 屏保备忘录区域显示的临时说明。
 */
constexpr const char* kMemoPlaceholder = "备忘录服务开发中";

}  // namespace

/**
 * @brief 获取固件生命周期内唯一的后端占位服务。
 * @return BackendService 单例引用。
 */
BackendService& BackendService::GetInstance() {
    static BackendService instance;
    return instance;
}

/**
 * @brief 初始化不包含任何网络能力的后端占位服务。
 */
void BackendService::Start() {
    if (started_) {
        return;
    }

    started_ = true;
    ESP_LOGI(kTag, "Custom backend API is disabled; screensaver placeholders are active");
}

/**
 * @brief 保留自定义 MCP 工具注册入口，但暂不注册业务工具。
 * @param server 设备 MCP 服务器；当前版本不会修改该对象。
 */
void BackendService::RegisterMcpTools(McpServer& server) {
    (void)server;
}

/**
 * @brief 接收网络连接通知，当前不触发任何后端操作。
 */
void BackendService::OnNetworkConnected() {
}

/**
 * @brief 接收网络断开通知，当前没有业务连接需要释放。
 */
void BackendService::OnNetworkDisconnected() {
}

/**
 * @brief 接收 MCP 会话断开通知，当前没有业务请求需要取消。
 */
void BackendService::OnMcpDisconnected() {
}

/**
 * @brief 在进入屏保时写入天气和备忘录占位内容。
 * @param active true 表示屏保可见；false 表示屏保已经退出。
 */
void BackendService::OnScreensaverChanged(bool active) {
    if (!active) {
        return;
    }

    Display* display = Board::GetInstance().GetDisplay();
    if (display == nullptr) {
        return;
    }

    display->SetScreensaverWeatherText(
        kWeatherLocationPlaceholder,
        kWeatherTemperaturePlaceholder,
        kWeatherDescriptionPlaceholder,
        kWeatherRangePlaceholder);
    display->SetScreensaverMemos({kMemoPlaceholder});
}
