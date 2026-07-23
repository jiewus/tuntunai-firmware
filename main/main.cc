/**
 * @file main.cc
 * @brief main.cc 中各类和辅助函数的具体实现。
 */
#include <esp_log.h>
#include <esp_err.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_event.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "app/application.h"

#define TAG "main"

/**
 * @brief ESP-IDF 应用程序入口。
 * @details 先初始化用于保存 Wi-Fi 和设备配置的 NVS 分区；当检测到分区空间不足或
 *          数据版本不兼容时，会擦除并重新初始化 NVS。随后初始化 Application 单例，
 *          并进入不会主动返回的应用主事件循环。
 */
extern "C" void app_main(void)
{
    // 初始化保存 Wi-Fi 配置、云端连接参数和设备设置的 NVS 分区。
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS flash to fix corruption");
        // 仅在 NVS 已满或格式版本不兼容时擦除，以恢复可用的键值存储空间。
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Application 为进程级单例，负责统一编排显示、音频、网络和云端协议模块。
    auto& app = Application::GetInstance();
    app.Initialize();
    app.Run();  // 进入主事件循环；设备正常运行期间本调用不会返回。
}
