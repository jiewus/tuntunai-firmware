/**
 * @file press_to_talk_mcp_tool.cc
 * @brief press_to_talk_mcp_tool.cc 中各类和辅助函数的具体实现。
 */
#include "mcp/tools/press_to_talk_mcp_tool.h"
#include <esp_log.h>

static const char* TAG = "PressToTalkMcpTool";

/**
 * @brief 构造 PressToTalkMcpTool 对象并初始化该模块运行所需的成员和系统资源。
 * @details 构造阶段只建立本模块自身资源；需要异步运行的任务由后续 Start 或 Initialize 方法启动。
 */
PressToTalkMcpTool::PressToTalkMcpTool()
    : press_to_talk_enabled_(false) {
}

/**
 * @brief 从 NVS 读取开关，并向 McpServer 注册设置工具。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void PressToTalkMcpTool::Initialize() {
    // 从设置中读取当前状态
    Settings settings("vendor");
    press_to_talk_enabled_ = settings.GetInt("press_to_talk", 0) != 0;

    // 注册MCP工具
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddTool("self.set_press_to_talk",
        "Switch between press to talk mode (长按说话) and click to talk mode (单击说话).\n"
        "The mode can be `press_to_talk` or `click_to_talk`.",
        PropertyList({
            Property("mode", kPropertyTypeString)
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            return HandleSetPressToTalk(properties);
        });

    ESP_LOGI(TAG, "按住说话 MCP 工具已初始化，当前模式=%s",
        press_to_talk_enabled_ ? "press_to_talk" : "click_to_talk");
}

/**
 * @return true 表示按下 BOOT 开始录音、松开结束录音。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
bool PressToTalkMcpTool::IsPressToTalkEnabled() const {
    return press_to_talk_enabled_;
}

/**
 * @brief 解析 MCP 参数并修改开关。
 * @param properties 必须包含 enabled 布尔属性。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
ReturnValue PressToTalkMcpTool::HandleSetPressToTalk(const PropertyList& properties) {
    auto mode = properties["mode"].value<std::string>();

    if (mode == "press_to_talk") {
        SetPressToTalkEnabled(true);
        ESP_LOGI(TAG, "已切换为按住说话模式");
        return true;
    } else if (mode == "click_to_talk") {
        SetPressToTalkEnabled(false);
        ESP_LOGI(TAG, "已切换为单击对话模式");
        return true;
    }

    throw std::runtime_error("Invalid mode: " + mode);
}

/**
 * @brief 更新内存状态并写入 NVS。
 * @param enabled 新状态。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void PressToTalkMcpTool::SetPressToTalkEnabled(bool enabled) {
    press_to_talk_enabled_ = enabled;

    Settings settings("vendor", true);
    settings.SetInt("press_to_talk", enabled ? 1 : 0);
    ESP_LOGI(TAG, "按住说话模式状态=%d", enabled);
}
