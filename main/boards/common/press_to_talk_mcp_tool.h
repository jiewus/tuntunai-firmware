#ifndef PRESS_TO_TALK_MCP_TOOL_H
#define PRESS_TO_TALK_MCP_TOOL_H

#include "mcp_server.h"
#include "settings.h"

/**
 * @file press_to_talk_mcp_tool.h
 * @brief 可由云端 MCP 切换的按住说话功能。
 */

/**
 * @brief 注册 MCP 工具并把按住说话开关持久化到 NVS。
 */
class PressToTalkMcpTool {
private:
    bool press_to_talk_enabled_;

public:
    PressToTalkMcpTool();
    
    /**
     * @brief 从 NVS 读取开关，并向 McpServer 注册设置工具。
     */
    void Initialize();
    
    /**
     * @return true 表示按下 BOOT 开始录音、松开结束录音。
     */
    bool IsPressToTalkEnabled() const;

private:
    /**
     * @brief 解析 MCP 参数并修改开关。
     * @param properties 必须包含 enabled 布尔属性。
     */
    ReturnValue HandleSetPressToTalk(const PropertyList& properties);
    
    /**
     * @brief 更新内存状态并写入 NVS。
     * @param enabled 新状态。
     */
    void SetPressToTalkEnabled(bool enabled);
};

#endif // PRESS_TO_TALK_MCP_TOOL_H 
