#ifndef _SYSTEM_INFO_H_
#define _SYSTEM_INFO_H_

#include <string>

#include <esp_err.h>
#include <freertos/FreeRTOS.h>

/**
 * @file system_info.h
 * @brief 芯片身份、内存和 FreeRTOS 运行状态诊断工具。
 */

/**
 * @brief 仅包含静态方法的系统信息工具类。
 */
class SystemInfo {
public:
    /**
     * @return 外部 SPI Flash 容量，单位字节。
     */
    static size_t GetFlashSize();
    /**
     * @return 启动以来内部堆的历史最小剩余字节数。
     */
    static size_t GetMinimumFreeHeapSize();
    /**
     * @return 当前所有可分配堆的剩余字节数。
     */
    static size_t GetFreeHeapSize();
    /**
     * @return 出厂 MAC 地址的大写十六进制文本。
     */
    static std::string GetMacAddress();
    /**
     * @return 当前 ESP32 芯片型号名称。
     */
    static std::string GetChipModelName();
    /**
     * @return 用于 HTTP 请求头的产品、版本和芯片组合字符串。
     */
    static std::string GetUserAgent();
    /**
     * @brief 在采样窗口内统计各任务 CPU 占用。
     * @param xTicksToWait 采样 tick 数。
     */
    static esp_err_t PrintTaskCpuUsage(TickType_t xTicksToWait);
    /**
     * @brief 将任务名称、状态、优先级和栈余量输出到日志。
     */
    static void PrintTaskList();
    /**
     * @brief 输出内部 RAM、PSRAM 及历史低水位统计。
     */
    static void PrintHeapStats();
    /**
     * @brief 输出当前 ESP-IDF 电源管理锁及持有者。
     */
    static void PrintPmLocks();
};

#endif // _SYSTEM_INFO_H_
