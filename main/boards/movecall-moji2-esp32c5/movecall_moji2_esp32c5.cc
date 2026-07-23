/**
 * @file movecall_moji2_esp32c5.cc
 * @brief Tuntun Moji2 ESP32-C5 板级硬件初始化和外设绑定实现。
 */
#include "wifi_board.h"
#include "codecs/es8311_audio_codec.h"
#include "display/lcd_display.h"
#include "app/application.h"
#include "button.h"
#include "config.h"
#include "led/single_led.h"
#include "mcp/tools/press_to_talk_mcp_tool.h"
#include "system/settings.h"

#include <atomic>

#include <esp_log.h>
#include <driver/i2c_master.h>
#include "power_save_timer.h"
#include "tuntun/backend/backend_service.h"

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>

#include <esp_lcd_st77916.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"

#include "adc_battery_monitor.h"

#define TAG "TuntunMoji2ESP32C5"

static const st77916_lcd_init_cmd_t lcd_init_cmds[] = {
    {0xF0, (uint8_t []){0x28}, 1, 0},
    {0xF2, (uint8_t []){0x28}, 1, 0},
    {0x73, (uint8_t []){0xF0}, 1, 0},
    {0x7C, (uint8_t []){0xD1}, 1, 0},
    {0x83, (uint8_t []){0xE0}, 1, 0},
    {0x84, (uint8_t []){0x61}, 1, 0},
    {0xF2, (uint8_t []){0x82}, 1, 0},
    {0xF0, (uint8_t []){0x00}, 1, 0},
    {0xF0, (uint8_t []){0x01}, 1, 0},
    {0xF1, (uint8_t []){0x01}, 1, 0},
    {0xB0, (uint8_t []){0x56}, 1, 0},
    {0xB1, (uint8_t []){0x4D}, 1, 0},
    {0xB2, (uint8_t []){0x24}, 1, 0},
    {0xB4, (uint8_t []){0x87}, 1, 0},
    {0xB5, (uint8_t []){0x44}, 1, 0},
    {0xB6, (uint8_t []){0x8B}, 1, 0},
    {0xB7, (uint8_t []){0x40}, 1, 0},
    {0xB8, (uint8_t []){0x86}, 1, 0},
    {0xBA, (uint8_t []){0x00}, 1, 0},
    {0xBB, (uint8_t []){0x08}, 1, 0},
    {0xBC, (uint8_t []){0x08}, 1, 0},
    {0xBD, (uint8_t []){0x00}, 1, 0},
    {0xC0, (uint8_t []){0x80}, 1, 0},
    {0xC1, (uint8_t []){0x10}, 1, 0},
    {0xC2, (uint8_t []){0x37}, 1, 0},
    {0xC3, (uint8_t []){0x80}, 1, 0},
    {0xC4, (uint8_t []){0x10}, 1, 0},
    {0xC5, (uint8_t []){0x37}, 1, 0},
    {0xC6, (uint8_t []){0xA9}, 1, 0},
    {0xC7, (uint8_t []){0x41}, 1, 0},
    {0xC8, (uint8_t []){0x01}, 1, 0},
    {0xC9, (uint8_t []){0xA9}, 1, 0},
    {0xCA, (uint8_t []){0x41}, 1, 0},
    {0xCB, (uint8_t []){0x01}, 1, 0},
    {0xD0, (uint8_t []){0x91}, 1, 0},
    {0xD1, (uint8_t []){0x68}, 1, 0},
    {0xD2, (uint8_t []){0x68}, 1, 0},
    {0xF5, (uint8_t []){0x00, 0xA5}, 2, 0},
    {0xDD, (uint8_t []){0x4F}, 1, 0},
    {0xDE, (uint8_t []){0x4F}, 1, 0},
    {0xF1, (uint8_t []){0x10}, 1, 0},
    {0xF0, (uint8_t []){0x00}, 1, 0},
    {0xF0, (uint8_t []){0x02}, 1, 0},
    {0xE0, (uint8_t []){0xF0, 0x0A, 0x10, 0x09, 0x09, 0x36, 0x35, 0x33, 0x4A, 0x29, 0x15, 0x15, 0x2E, 0x34}, 14, 0},
    {0xE1, (uint8_t []){0xF0, 0x0A, 0x0F, 0x08, 0x08, 0x05, 0x34, 0x33, 0x4A, 0x39, 0x15, 0x15, 0x2D, 0x33}, 14, 0},
    {0xF0, (uint8_t []){0x10}, 1, 0},
    {0xF3, (uint8_t []){0x10}, 1, 0},
    {0xE0, (uint8_t []){0x07}, 1, 0},
    {0xE1, (uint8_t []){0x00}, 1, 0},
    {0xE2, (uint8_t []){0x00}, 1, 0},
    {0xE3, (uint8_t []){0x00}, 1, 0},
    {0xE4, (uint8_t []){0xE0}, 1, 0},
    {0xE5, (uint8_t []){0x06}, 1, 0},
    {0xE6, (uint8_t []){0x21}, 1, 0},
    {0xE7, (uint8_t []){0x01}, 1, 0},
    {0xE8, (uint8_t []){0x05}, 1, 0},
    {0xE9, (uint8_t []){0x02}, 1, 0},
    {0xEA, (uint8_t []){0xDA}, 1, 0},
    {0xEB, (uint8_t []){0x00}, 1, 0},
    {0xEC, (uint8_t []){0x00}, 1, 0},
    {0xED, (uint8_t []){0x0F}, 1, 0},
    {0xEE, (uint8_t []){0x00}, 1, 0},
    {0xEF, (uint8_t []){0x00}, 1, 0},
    {0xF8, (uint8_t []){0x00}, 1, 0},
    {0xF9, (uint8_t []){0x00}, 1, 0},
    {0xFA, (uint8_t []){0x00}, 1, 0},
    {0xFB, (uint8_t []){0x00}, 1, 0},
    {0xFC, (uint8_t []){0x00}, 1, 0},
    {0xFD, (uint8_t []){0x00}, 1, 0},
    {0xFE, (uint8_t []){0x00}, 1, 0},
    {0xFF, (uint8_t []){0x00}, 1, 0},
    {0x60, (uint8_t []){0x40}, 1, 0},
    {0x61, (uint8_t []){0x04}, 1, 0},
    {0x62, (uint8_t []){0x00}, 1, 0},
    {0x63, (uint8_t []){0x42}, 1, 0},
    {0x64, (uint8_t []){0xD9}, 1, 0},
    {0x65, (uint8_t []){0x00}, 1, 0},
    {0x66, (uint8_t []){0x00}, 1, 0},
    {0x67, (uint8_t []){0x00}, 1, 0},
    {0x68, (uint8_t []){0x00}, 1, 0},
    {0x69, (uint8_t []){0x00}, 1, 0},
    {0x6A, (uint8_t []){0x00}, 1, 0},
    {0x6B, (uint8_t []){0x00}, 1, 0},
    {0x70, (uint8_t []){0x40}, 1, 0},
    {0x71, (uint8_t []){0x03}, 1, 0},
    {0x72, (uint8_t []){0x00}, 1, 0},
    {0x73, (uint8_t []){0x42}, 1, 0},
    {0x74, (uint8_t []){0xD8}, 1, 0},
    {0x75, (uint8_t []){0x00}, 1, 0},
    {0x76, (uint8_t []){0x00}, 1, 0},
    {0x77, (uint8_t []){0x00}, 1, 0},
    {0x78, (uint8_t []){0x00}, 1, 0},
    {0x79, (uint8_t []){0x00}, 1, 0},
    {0x7A, (uint8_t []){0x00}, 1, 0},
    {0x7B, (uint8_t []){0x00}, 1, 0},
    {0x80, (uint8_t []){0x48}, 1, 0},
    {0x81, (uint8_t []){0x00}, 1, 0},
    {0x82, (uint8_t []){0x06}, 1, 0},
    {0x83, (uint8_t []){0x02}, 1, 0},
    {0x84, (uint8_t []){0xD6}, 1, 0},
    {0x85, (uint8_t []){0x04}, 1, 0},
    {0x86, (uint8_t []){0x00}, 1, 0},
    {0x87, (uint8_t []){0x00}, 1, 0},
    {0x88, (uint8_t []){0x48}, 1, 0},
    {0x89, (uint8_t []){0x00}, 1, 0},
    {0x8A, (uint8_t []){0x08}, 1, 0},
    {0x8B, (uint8_t []){0x02}, 1, 0},
    {0x8C, (uint8_t []){0xD8}, 1, 0},
    {0x8D, (uint8_t []){0x04}, 1, 0},
    {0x8E, (uint8_t []){0x00}, 1, 0},
    {0x8F, (uint8_t []){0x00}, 1, 0},
    {0x90, (uint8_t []){0x48}, 1, 0},
    {0x91, (uint8_t []){0x00}, 1, 0},
    {0x92, (uint8_t []){0x0A}, 1, 0},
    {0x93, (uint8_t []){0x02}, 1, 0},
    {0x94, (uint8_t []){0xDA}, 1, 0},
    {0x95, (uint8_t []){0x04}, 1, 0},
    {0x96, (uint8_t []){0x00}, 1, 0},
    {0x97, (uint8_t []){0x00}, 1, 0},
    {0x98, (uint8_t []){0x48}, 1, 0},
    {0x99, (uint8_t []){0x00}, 1, 0},
    {0x9A, (uint8_t []){0x0C}, 1, 0},
    {0x9B, (uint8_t []){0x02}, 1, 0},
    {0x9C, (uint8_t []){0xDC}, 1, 0},
    {0x9D, (uint8_t []){0x04}, 1, 0},
    {0x9E, (uint8_t []){0x00}, 1, 0},
    {0x9F, (uint8_t []){0x00}, 1, 0},
    {0xA0, (uint8_t []){0x48}, 1, 0},
    {0xA1, (uint8_t []){0x00}, 1, 0},
    {0xA2, (uint8_t []){0x05}, 1, 0},
    {0xA3, (uint8_t []){0x02}, 1, 0},
    {0xA4, (uint8_t []){0xD5}, 1, 0},
    {0xA5, (uint8_t []){0x04}, 1, 0},
    {0xA6, (uint8_t []){0x00}, 1, 0},
    {0xA7, (uint8_t []){0x00}, 1, 0},
    {0xA8, (uint8_t []){0x48}, 1, 0},
    {0xA9, (uint8_t []){0x00}, 1, 0},
    {0xAA, (uint8_t []){0x07}, 1, 0},
    {0xAB, (uint8_t []){0x02}, 1, 0},
    {0xAC, (uint8_t []){0xD7}, 1, 0},
    {0xAD, (uint8_t []){0x04}, 1, 0},
    {0xAE, (uint8_t []){0x00}, 1, 0},
    {0xAF, (uint8_t []){0x00}, 1, 0},
    {0xB0, (uint8_t []){0x48}, 1, 0},
    {0xB1, (uint8_t []){0x00}, 1, 0},
    {0xB2, (uint8_t []){0x09}, 1, 0},
    {0xB3, (uint8_t []){0x02}, 1, 0},
    {0xB4, (uint8_t []){0xD9}, 1, 0},
    {0xB5, (uint8_t []){0x04}, 1, 0},
    {0xB6, (uint8_t []){0x00}, 1, 0},
    {0xB7, (uint8_t []){0x00}, 1, 0},
    {0xB8, (uint8_t []){0x48}, 1, 0},
    {0xB9, (uint8_t []){0x00}, 1, 0},
    {0xBA, (uint8_t []){0x0B}, 1, 0},
    {0xBB, (uint8_t []){0x02}, 1, 0},
    {0xBC, (uint8_t []){0xDB}, 1, 0},
    {0xBD, (uint8_t []){0x04}, 1, 0},
    {0xBE, (uint8_t []){0x00}, 1, 0},
    {0xBF, (uint8_t []){0x00}, 1, 0},
    {0xC0, (uint8_t []){0x10}, 1, 0},
    {0xC1, (uint8_t []){0x47}, 1, 0},
    {0xC2, (uint8_t []){0x56}, 1, 0},
    {0xC3, (uint8_t []){0x65}, 1, 0},
    {0xC4, (uint8_t []){0x74}, 1, 0},
    {0xC5, (uint8_t []){0x88}, 1, 0},
    {0xC6, (uint8_t []){0x99}, 1, 0},
    {0xC7, (uint8_t []){0x01}, 1, 0},
    {0xC8, (uint8_t []){0xBB}, 1, 0},
    {0xC9, (uint8_t []){0xAA}, 1, 0},
    {0xD0, (uint8_t []){0x10}, 1, 0},
    {0xD1, (uint8_t []){0x47}, 1, 0},
    {0xD2, (uint8_t []){0x56}, 1, 0},
    {0xD3, (uint8_t []){0x65}, 1, 0},
    {0xD4, (uint8_t []){0x74}, 1, 0},
    {0xD5, (uint8_t []){0x88}, 1, 0},
    {0xD6, (uint8_t []){0x99}, 1, 0},
    {0xD7, (uint8_t []){0x01}, 1, 0},
    {0xD8, (uint8_t []){0xBB}, 1, 0},
    {0xD9, (uint8_t []){0xAA}, 1, 0},
    {0xF3, (uint8_t []){0x01}, 1, 0},
    {0xF0, (uint8_t []){0x00}, 1, 0},
    {0x21, (uint8_t []){}, 0, 0},
    {0x11, (uint8_t []){}, 0, 0},
    {0x00, (uint8_t []){}, 0, 120},
};

class TuntunMoji2ESP32C5 : public WifiBoard {
private:
    i2c_master_bus_handle_t codec_i2c_bus_;
    Button boot_button_;
    Display* display_ = nullptr;

    PressToTalkMcpTool* press_to_talk_tool_ = nullptr;

    PowerSaveTimer* power_save_timer_ = nullptr;
    PowerSaveTimer* screen_off_timer_ = nullptr;
    AdcBatteryMonitor* adc_battery_monitor_ = nullptr;
    bool screensaver_enabled_ = true;
    std::atomic<bool> screensaver_active_{false};
    bool screen_auto_off_enabled_ = false;
    bool screen_is_off_ = false;
    int screen_auto_off_timeout_ = 300;
    uint8_t screen_brightness_before_off_ = 75;
    static constexpr int kScreensaverTimeoutSeconds = 30;

    /**
     * @brief 表盘屏保开关在 display 命名空间中的 NVS 键名。
     * @details ESP-IDF 要求 NVS 键名最多包含 15 个字符，因此持久化配置必须使用此短键名，
     *          不能直接使用较长的业务字段名 screensaver_enabled。
     */
    static constexpr const char* kScreensaverEnabledKey = "scr_saver_en";

    /**
     * @brief 自动熄屏开关在 display 命名空间中的 NVS 键名。
     * @details 该键名同时用于启动读取和对话设置写入，确保两条路径访问同一份配置。
     */
    static constexpr const char* kScreenAutoOffEnabledKey = "scr_off_en";

    /**
     * @brief 自动熄屏延时时间在 display 命名空间中的 NVS 键名。
     * @details 保存值的单位为秒，键名长度不超过 ESP-IDF NVS 规定的 15 个字符。
     */
    static constexpr const char* kScreenAutoOffTimeoutKey = "scr_off_sec";

    /**
     * @brief 统一切换表盘屏保的可见状态并同步后端天气任务生命周期。
     * @param active true 立即显示表盘；false 退出表盘并恢复普通对话界面。
     * @details 方法使用原子状态过滤重复进入或退出，确保 30 秒定时器、对话结束事件和用户唤醒
     *          同时到达时只切换一次界面，也只向 BackendService 发送一次对应状态通知。
     *          进入通知允许 BackendService 使用设备 Token 按需刷新天气；退出通知会禁止创建
     *          新天气请求。备忘录在对应接口接入前继续显示本地占位文本。
     */
    void SetScreensaverActive(bool active) {
        if (active) {
            if (!screensaver_enabled_ || screen_is_off_ || display_ == nullptr
                || screensaver_active_.exchange(true)) {
                return;
            }
            display_->SetScreensaverMode(true);
            BackendService::GetInstance().OnScreensaverChanged(true);
            return;
        }

        if (!screensaver_active_.exchange(false)) {
            return;
        }
        BackendService::GetInstance().OnScreensaverChanged(false);
        if (display_ != nullptr) {
            display_->SetScreensaverMode(false);
        }
    }

    /**
     * @brief 创建电池 ADC 监视器并联动屏幕活动计时。
     * @details 本板使用 ADC1 通道 3 和 5.1M/5.1M 分压。充电状态发生变化时会恢复背光并
     *          重置自动熄屏计时；如果表盘屏保正在显示，则只刷新屏保中的电量状态，不退出屏保。
     */
    void InitializeBatteryMonitor() {
        adc_battery_monitor_ = new AdcBatteryMonitor(ADC_UNIT_1, ADC_CHANNEL_3, 5100000, 5100000, GPIO_NUM_NC);
        adc_battery_monitor_->OnChargingStatusChanged([this](bool is_charging) {
            (void)is_charging;
            WakeUpScreen();
        });
    }

    /**
     * @brief 初始化互相独立的表盘屏保与自动熄屏策略。
     * @details 屏保默认开启并在 30 秒无互动后显示；自动熄屏默认关闭，开启后按照
     *          auto_off_timeout 指定的总空闲秒数关闭背光。两个定时器均不降低 CPU 频率、
     *          不进入 Light Sleep，也不会停止麦克风和唤醒词检测。
     */
    void InitializePowerSaveTimer() {
        Settings settings("display", false);
        screensaver_enabled_ = settings.GetBool(kScreensaverEnabledKey, true);
        screen_auto_off_enabled_ = settings.GetBool(kScreenAutoOffEnabledKey, false);
        screen_auto_off_timeout_ = settings.GetInt(kScreenAutoOffTimeoutKey, 300);
        if (screen_auto_off_timeout_ < 5 || screen_auto_off_timeout_ > 3600) {
            ESP_LOGW(TAG, "Invalid screen-off timeout %d, reset to 300 seconds", screen_auto_off_timeout_);
            screen_auto_off_timeout_ = 300;
        }

        power_save_timer_ = new PowerSaveTimer(
            -1, screensaver_enabled_ ? kScreensaverTimeoutSeconds : -1, -1);
        power_save_timer_->OnEnterSleepMode([this]() {
            SetScreensaverActive(true);
        });
        power_save_timer_->OnExitSleepMode([this]() {
            SetScreensaverActive(false);
        });
        power_save_timer_->SetEnabled(true);

        screen_off_timer_ = new PowerSaveTimer(
            -1, screen_auto_off_enabled_ ? screen_auto_off_timeout_ : -1, -1);
        screen_off_timer_->OnEnterSleepMode([this]() {
            auto backlight = GetBacklight();
            if (backlight == nullptr || screen_is_off_) {
                return;
            }
            screen_brightness_before_off_ = backlight->brightness();
            screen_is_off_ = true;
            backlight->SetBrightness(0);
        });
        screen_off_timer_->OnExitSleepMode([this]() {
            auto backlight = GetBacklight();
            if (backlight == nullptr || !screen_is_off_) {
                return;
            }
            screen_is_off_ = false;
            backlight->SetBrightness(screen_brightness_before_off_);
        });
        screen_off_timer_->SetEnabled(true);
    }

    /**
     * @brief 创建 ES8311 使用的 I2C0 主总线。
     * @details SDA/SCL 取自板级 config.h，并启用内部上拉和 7 个时钟周期毛刺过滤；创建失败会由 ESP_ERROR_CHECK 终止启动。
     */
    void InitializeCodecI2c() {
        // 初始化 Codec 控制总线，句柄保存在 codec_i2c_bus_ 供音频对象长期使用。
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &codec_i2c_bus_));
    }

    /**
     * @brief 初始化 ST77916 使用的 SPI2 QSPI 总线。
     * @details 四根数据线与时钟线由板级配置提供，最大 DMA 传输量按 80 行 RGB565 像素计算。
     */
    void InitializeSpi() {
        ESP_LOGI(TAG, "Initialize SPI bus");
        const spi_bus_config_t bus_config = MOJI2_ST77916_PANEL_BUS_QSPI_CONFIG(DISPLAY_QSPI_SCLK_PIN,
                                                                                  DISPLAY_QSPI_D0_PIN,
                                                                                  DISPLAY_QSPI_D1_PIN,
                                                                                  DISPLAY_QSPI_D2_PIN,
                                                                                  DISPLAY_QSPI_D3_PIN,
                                                                                  DISPLAY_QSPI_H_RES * 80 * sizeof(uint16_t));
        ESP_ERROR_CHECK(spi_bus_initialize(DISPLAY_QSPI_HOST, &bus_config, SPI_DMA_CH_AUTO));
    }

    /**
     * @brief 创建 ST77916 面板 IO、执行厂商寄存器序列并接入 LVGL。
     * @details 初始化顺序为创建 QSPI IO、创建面板、硬件复位、发送初始化命令、开屏、设置方向，最后构造 SpiLcdDisplay。
     */
    void InitializeSt77916Display() {
        ESP_LOGI(TAG, "Init St77916 display");
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;
        ESP_LOGI(TAG, "Install panel IO");
        
        esp_lcd_panel_io_spi_config_t io_config = ST77916_PANEL_IO_QSPI_CONFIG(DISPLAY_QSPI_CS_PIN, NULL, NULL);
        io_config.pclk_hz = DISPLAY_QSPI_PIXEL_CLOCK_HZ;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)DISPLAY_QSPI_HOST, &io_config, &panel_io));

    
        ESP_LOGI(TAG, "Install St77916 panel driver");
        st77916_vendor_config_t vendor_config = {
            .init_cmds = lcd_init_cmds,
            .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(st77916_lcd_init_cmd_t),
            .flags = {
                .use_qspi_interface = 1,
            },
        };
        const esp_lcd_panel_dev_config_t panel_config = {
            .reset_gpio_num = DISPLAY_QSPI_RESET_PIN,
            .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
            .bits_per_pixel = DISPLAY_QSPI_BIT_PER_PIXEL,
            .vendor_config = &vendor_config,
        };
        ESP_ERROR_CHECK(esp_lcd_new_panel_st77916(panel_io, &panel_config, &panel));

        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_disp_on_off(panel, true);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);

        display_ = new SpiLcdDisplay(panel_io, panel,
                                    DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

    /**
     * @brief 注册 BOOT 键的单击、按下和释放行为。
     * @details 启动阶段单击进入配网；普通模式单击切换对话；按住说话模式下按下开始录音、释放停止录音，同时按下会唤醒省电屏幕。
     */
    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (BackendService::GetInstance().ExitBindingPage()) {
                return;
            }
            // 启动且尚未联网时，单击 BOOT 直接进入配网，不执行重启。
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            if (!press_to_talk_tool_ || !press_to_talk_tool_->IsPressToTalkEnabled()) {
                app.ToggleChatState();
            }
        });
        boot_button_.OnPressDown([this]() {
            if (BackendService::GetInstance().IsBindingPageVisible()) {
                WakeUpScreen(true);
                return;
            }
            if (power_save_timer_) {
                power_save_timer_->WakeUp();
            }
            if (press_to_talk_tool_ && press_to_talk_tool_->IsPressToTalkEnabled()) {
                Application::GetInstance().StartListening();
            }
        });
        boot_button_.OnPressUp([this]() {
            if (BackendService::GetInstance().IsBindingPageVisible()) {
                return;
            }
            if (press_to_talk_tool_ && press_to_talk_tool_->IsPressToTalkEnabled()) {
                Application::GetInstance().StopListening();
            }
        });
    }

    /**
     * @brief 创建并注册本板的按住说话 MCP 工具。
     * @details 工具对象在板实例整个生命周期内保持有效，并负责从 NVS 恢复用户设置。
     */
    void InitializeTools() {
        press_to_talk_tool_ = new PressToTalkMcpTool();
        press_to_talk_tool_->Initialize();
    }

public:
    /**
     * @brief 按硬件依赖顺序完成整块 Moji2 开发板初始化。
     * @details I2C 必须先于 Codec，SPI 必须先于 LCD；显示和按键就绪后注册 MCP 工具，最后从 NVS 恢复背光亮度。
     */
    TuntunMoji2ESP32C5() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializeCodecI2c();
        InitializePowerSaveTimer();
        InitializeBatteryMonitor();
        InitializeSpi();
        InitializeSt77916Display();
        InitializeButtons();
        InitializeTools();
        GetBacklight()->RestoreBrightness();
    }

    /**
     * @brief 获取板载 WS2812 状态灯。
     * @return 进程生命周期内唯一的 SingleLed 实例，调用者不得释放。
     */
    virtual Led* GetLed() override {
        static SingleLed led_strip(BUILTIN_LED_GPIO);
        return &led_strip;
    }

    /**
     * @brief 获取初始化完成的圆形 LCD 显示对象。
     * @return 由板对象持有的 Display 指针，调用者不得释放。
     */
    virtual Display* GetDisplay() override {
        return display_;
    }
    
    /**
     * @brief 获取 GPIO2 上的 PWM 背光控制器。
     * @return 静态 PwmBacklight 实例，首次调用时创建。
     */
    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    /**
     * @brief 根据活动来源恢复背光，并决定是否允许退出表盘屏保。
     * @param user_initiated true 表示唤醒词或按键等用户主动唤醒，允许退出屏保并重新计算
     *                       30 秒无互动时间；false 表示后台状态变化，屏保显示期间保持表盘。
     * @details 屏保尚未显示时，普通活动仍会重置进入屏保的计时。屏保已经显示后，只有
     *          user_initiated 为 true 才调用表盘定时器的 WakeUp()，避免网络、电量、通知、
     *          对话状态变化或 MCP 调用把界面切回普通对话页。自动熄屏定时器始终可以恢复
     *          背光，但恢复后仍显示原来的屏保界面。
     */
    virtual void WakeUpScreen(bool user_initiated = false) override {
        if (user_initiated && display_ != nullptr) {
            display_->HideCustomMcpList();
        }
        if (power_save_timer_ != nullptr && (!screensaver_active_ || user_initiated)) {
            power_save_timer_->WakeUp();
        }
        /*
         * 对话结束触发的立即屏保并未等待 PowerSaveTimer 进入内部 sleep 状态，因此用户在
         * 30 秒内再次唤醒时，WakeUp() 不会产生 OnExitSleepMode 回调，需要在这里显式退出。
         */
        if (user_initiated && screensaver_active_.load()) {
            SetScreensaverActive(false);
        }
        if (screen_off_timer_ != nullptr) {
            screen_off_timer_->WakeUp();
        }
    }

    /**
     * @brief 在屏保配置允许时立即显示金属黑表盘。
     * @details 本方法跳过常规 30 秒空闲等待，但不修改用户保存的屏保开关，也不改变自动熄屏
     *          的独立计时；主要由应用层在监听或播报状态结束并回到空闲状态后调用。
     */
    virtual void EnterScreensaver() override {
        SetScreensaverActive(true);
    }

    /**
     * @brief 更新并持久化自动熄屏等待时间。
     * @param seconds 从最后一次互动开始计算的等待秒数；0 关闭自动熄屏，
     *                有效非零范围为 5 至 3600 秒。
     * @return 参数合法且配置成功时返回 true，否则返回 false。
     */
    virtual bool SetScreenAutoOffTimeout(int seconds) override {
        if (seconds != 0 && (seconds < 5 || seconds > 3600)) {
            return false;
        }

        Settings settings("display", true);
        if (seconds == 0) {
            screen_auto_off_enabled_ = false;
            settings.SetBool(kScreenAutoOffEnabledKey, false);
        } else {
            screen_auto_off_timeout_ = seconds;
            screen_auto_off_enabled_ = true;
            settings.SetInt(kScreenAutoOffTimeoutKey, seconds);
            settings.SetBool(kScreenAutoOffEnabledKey, true);
        }

        if (screen_off_timer_ != nullptr) {
            screen_off_timer_->SetSleepTimeout(
                screen_auto_off_enabled_ ? screen_auto_off_timeout_ : -1);
        }
        return true;
    }

    /**
     * @brief 获取当前自动熄屏等待时间。
     * @return 已保存的等待秒数；自动熄屏关闭时仍返回最近一次配置值。
     */
    virtual int GetScreenAutoOffTimeout() const override {
        return screen_auto_off_timeout_;
    }

    /**
     * @brief 通过运行时配置开启或关闭 30 秒表盘屏保。
     * @param enabled true 开启；false 关闭并立即退出当前屏保。
     * @return 配置成功时返回 true。
     */
    virtual bool SetScreensaverEnabled(bool enabled) override {
        screensaver_enabled_ = enabled;
        Settings settings("display", true);
        settings.SetBool(kScreensaverEnabledKey, enabled);
        if (power_save_timer_ != nullptr) {
            power_save_timer_->SetSleepTimeout(enabled ? kScreensaverTimeoutSeconds : -1);
        }
        if (!enabled) {
            SetScreensaverActive(false);
        }
        return true;
    }

    /**
     * @return true 表示 30 秒无互动后会显示表盘屏保。
     */
    virtual bool IsScreensaverEnabled() const override {
        return screensaver_enabled_;
    }

    /**
     * @return true 表示本板的金属黑表盘当前覆盖在普通对话界面之上。
     */
    virtual bool IsScreensaverActive() const override {
        return screensaver_active_.load();
    }

    /**
     * @brief 开启或关闭自动熄屏，并保留最近一次设置的延时时间。
     * @param enabled true 开启；false 关闭并立即恢复背光。
     * @return 配置成功时返回 true。
     */
    virtual bool SetScreenAutoOffEnabled(bool enabled) override {
        screen_auto_off_enabled_ = enabled;
        Settings settings("display", true);
        settings.SetBool(kScreenAutoOffEnabledKey, enabled);
        if (screen_off_timer_ != nullptr) {
            screen_off_timer_->SetSleepTimeout(enabled ? screen_auto_off_timeout_ : -1);
        }
        return true;
    }

    /**
     * @return true 表示达到配置的空闲时间后会关闭背光。
     */
    virtual bool IsScreenAutoOffEnabled() const override {
        return screen_auto_off_enabled_;
    }

    /**
     * @brief 获取绑定本板 I2C、I2S 和功放引脚的 ES8311 Codec。
     * @return 静态 Es8311AudioCodec 实例，调用者不得释放。
     */
    virtual AudioCodec* GetAudioCodec() override {
        static Es8311AudioCodec audio_codec(codec_i2c_bus_, I2C_NUM_0, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR);
        return &audio_codec;
    }

    /**
     * @brief 读取当前电量和充放电状态。
     * @param level 输出 0-100 的估算电量百分比。
     * @param charging 输出当前是否充电。
     * @param discharging 输出当前是否放电。
     * @return 电池监视器已创建，因此固定返回 true。
     */
    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        charging = adc_battery_monitor_->IsCharging();
        discharging = adc_battery_monitor_->IsDischarging();
        level = adc_battery_monitor_->GetBatteryLevel();
        return true;
    }

};

DECLARE_BOARD(TuntunMoji2ESP32C5);
