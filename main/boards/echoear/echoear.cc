/**
 * @file echoear.cc
 * @brief EchoEar ESP32-S3 板级硬件初始化。
 */
#include "wifi_board.h"
#include "echoear_audio_codec.h"
#include "echoear_lcd_init.h"
#include "config.h"
#include "lcd_display.h"
#include "pwm_backlight.h"
#include "button.h"
#include "power_save_timer.h"
#include "app/application.h"
#include "mcp/tools/press_to_talk_mcp_tool.h"
#include "system/settings.h"
#include "tuntun/backend/backend_service.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <driver/spi_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_st77916.h>
#include <esp_err.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#define TAG "EchoEar"

namespace {

// EchoEar 电量计当前提供电压和电流，未发现可读取的 SOC 寄存器。
// 依据单节锂电池的典型放电曲线进行分段插值，避免 3.3-4.2V 线性换算造成中段偏差。
int EstimateBatteryLevel(int voltage_mv) {
    struct VoltageLevel {
        int voltage_mv;
        int level;
    };
    constexpr VoltageLevel kCurve[] = {
        {3300, 0}, {3500, 5}, {3600, 15}, {3700, 30}, {3800, 50},
        {3900, 70}, {4000, 85}, {4100, 95}, {4200, 100},
    };
    constexpr size_t kCurveSize = sizeof(kCurve) / sizeof(kCurve[0]);

    if (voltage_mv <= kCurve[0].voltage_mv) {
        return kCurve[0].level;
    }
    for (size_t i = 1; i < kCurveSize; ++i) {
        if (voltage_mv <= kCurve[i].voltage_mv) {
            const auto& lower = kCurve[i - 1];
            const auto& upper = kCurve[i];
            return lower.level +
                   (voltage_mv - lower.voltage_mv) * (upper.level - lower.level) /
                       (upper.voltage_mv - lower.voltage_mv);
        }
    }
    return kCurve[kCurveSize - 1].level;
}

}  // namespace

class EchoEarBoard : public WifiBoard {
public:
    EchoEarBoard() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializePowerControl();
        InitializeI2c();
        DetectPcbVersion();
        InitializeBatteryGauge();
        InitializeTouch();
        InitializeSpi();
        InitializeDisplay();
        InitializePowerSaveTimer();
        InitializeButtons();
        InitializeTools();
        GetBacklight()->RestoreBrightness();
    }

    Display* GetDisplay() override {
        return display_;
    }

    Backlight* GetBacklight() override {
        static PwmBacklight backlight(
            DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    void WakeUpScreen(bool user_initiated = false) override {
        if (user_initiated && display_ != nullptr) {
            display_->HideCustomMcpList();
            display_->SetScreensaverMode(false);
            ESP_LOGI(TAG, "用户唤醒：强制隐藏屏保容器，board_active=%d, custom_mcp=%d",
                     screensaver_active_.load(), display_->IsCustomMcpListActive());
        }
        if (power_save_timer_ != nullptr && (!screensaver_active_ || user_initiated)) {
            power_save_timer_->WakeUp();
        }
        if (user_initiated && screensaver_active_.load()) {
            SetScreensaverActive(false);
        }
        if (screen_off_timer_ != nullptr) {
            screen_off_timer_->WakeUp();
        }
    }

    void EnterScreensaver() override {
        SetScreensaverActive(true);
    }

    bool SetScreenAutoOffTimeout(int seconds) override {
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

    int GetScreenAutoOffTimeout() const override {
        return screen_auto_off_timeout_;
    }

    bool SetScreensaverEnabled(bool enabled) override {
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

    bool IsScreensaverEnabled() const override {
        return screensaver_enabled_;
    }

    bool IsScreensaverActive() const override {
        return screensaver_active_.load();
    }

    bool SetScreenAutoOffEnabled(bool enabled) override {
        screen_auto_off_enabled_ = enabled;
        Settings settings("display", true);
        settings.SetBool(kScreenAutoOffEnabledKey, enabled);
        if (screen_off_timer_ != nullptr) {
            screen_off_timer_->SetSleepTimeout(enabled ? screen_auto_off_timeout_ : -1);
        }
        return true;
    }

    bool IsScreenAutoOffEnabled() const override {
        return screen_auto_off_enabled_;
    }

    AudioCodec* GetAudioCodec() override {
        static EchoEarAudioCodec codec(
            i2c_bus_, I2C_NUM_0,
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT, audio_din_pin_, audio_pa_pin_,
            ES8311_CODEC_DEFAULT_ADDR, ES7210_CODEC_DEFAULT_ADDR,
            AUDIO_INPUT_REFERENCE);
        return &codec;
    }

    bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        if (battery_device_ == nullptr) {
            return false;
        }

        uint8_t voltage_data[2] = {};
        uint8_t current_data[2] = {};
        if (!ReadI2cRegister(battery_device_, 0x08, voltage_data, sizeof(voltage_data)) ||
            !ReadI2cRegister(battery_device_, 0x0c, current_data, sizeof(current_data))) {
            return false;
        }

        const int voltage_mv = voltage_data[0] | (voltage_data[1] << 8);
        const int16_t current_ma = static_cast<int16_t>(
            current_data[0] | (current_data[1] << 8));
        level = EstimateBatteryLevel(voltage_mv);
        charging = current_ma < -20;
        discharging = current_ma > 20;
        return true;
    }

private:
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    i2c_master_dev_handle_t touch_device_ = nullptr;
    i2c_master_dev_handle_t battery_device_ = nullptr;
    SemaphoreHandle_t touch_semaphore_ = nullptr;
    Button boot_button_;
    Display* display_ = nullptr;
    PressToTalkMcpTool* press_to_talk_tool_ = nullptr;
    PowerSaveTimer* power_save_timer_ = nullptr;
    PowerSaveTimer* screen_off_timer_ = nullptr;
    gpio_num_t audio_din_pin_ = AUDIO_I2S_GPIO_DIN_V10;
    gpio_num_t audio_pa_pin_ = AUDIO_CODEC_PA_PIN_V10;
    gpio_num_t display_reset_pin_ = DISPLAY_QSPI_RESET_PIN_V10;
    bool pcb_v12_ = false;
    std::atomic<bool> touch_was_pressed_{false};
    bool screensaver_enabled_ = true;
    std::atomic<bool> screensaver_active_{false};
    bool screen_auto_off_enabled_ = false;
    std::atomic<bool> screen_is_off_{false};
    int screen_auto_off_timeout_ = 300;
    uint8_t screen_brightness_before_off_ = 75;

    static constexpr int kScreensaverTimeoutSeconds = 30;
    static constexpr const char* kScreensaverEnabledKey = "scr_saver_en";
    static constexpr const char* kScreenAutoOffEnabledKey = "scr_off_en";
    static constexpr const char* kScreenAutoOffTimeoutKey = "scr_off_sec";

    void SetScreensaverActive(bool active) {
        if (active) {
            if (!screensaver_enabled_ || screen_is_off_ || display_ == nullptr ||
                screensaver_active_.exchange(true)) {
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

    void InitializePowerSaveTimer() {
        Settings settings("display", false);
        screensaver_enabled_ = settings.GetBool(kScreensaverEnabledKey, true);
        screen_auto_off_enabled_ = settings.GetBool(kScreenAutoOffEnabledKey, false);
        screen_auto_off_timeout_ = settings.GetInt(kScreenAutoOffTimeoutKey, 300);
        if (screen_auto_off_timeout_ < 5 || screen_auto_off_timeout_ > 3600) {
            ESP_LOGW(TAG, "Invalid screen-off timeout %d, reset to 300 seconds",
                     screen_auto_off_timeout_);
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
            auto* backlight = GetBacklight();
            if (backlight == nullptr || screen_is_off_) {
                return;
            }
            screen_brightness_before_off_ = backlight->brightness();
            screen_is_off_ = true;
            backlight->SetBrightness(0);
        });
        screen_off_timer_->OnExitSleepMode([this]() {
            auto* backlight = GetBacklight();
            if (backlight == nullptr || !screen_is_off_) {
                return;
            }
            screen_is_off_ = false;
            backlight->SetBrightness(screen_brightness_before_off_);
        });
        screen_off_timer_->SetEnabled(true);
    }

    void InitializePowerControl() {
        const gpio_config_t config = {
            .pin_bit_mask = 1ULL << POWER_CTRL,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&config));
        ESP_ERROR_CHECK(gpio_set_level(POWER_CTRL, 0));
    }

    void InitializeI2c() {
        const i2c_master_bus_config_t config = {
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
        ESP_ERROR_CHECK(i2c_new_master_bus(&config, &i2c_bus_));
    }

    void DetectPcbVersion() {
        if (i2c_master_probe(i2c_bus_, PCB_VERSION_I2C_ADDRESS, 100) == ESP_OK) {
            ESP_LOGI(TAG, "Detected PCB V1.0");
            return;
        }

        const gpio_config_t codec_power_config = {
            .pin_bit_mask = 1ULL << CODEC_POWER_CTRL,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&codec_power_config));
        ESP_ERROR_CHECK(gpio_set_level(CODEC_POWER_CTRL, 1));
        vTaskDelay(pdMS_TO_TICKS(100));

        if (i2c_master_probe(i2c_bus_, PCB_VERSION_I2C_ADDRESS, 100) == ESP_OK) {
            pcb_v12_ = true;
            audio_din_pin_ = AUDIO_I2S_GPIO_DIN_V12;
            audio_pa_pin_ = AUDIO_CODEC_PA_PIN_V12;
            display_reset_pin_ = DISPLAY_QSPI_RESET_PIN_V12;
            ESP_LOGI(TAG, "Detected PCB V1.2");
        } else {
            ESP_LOGW(TAG, "PCB version detection failed, using V1.0 pin map");
        }
    }

    i2c_master_dev_handle_t AddI2cDevice(uint8_t address) {
        const i2c_device_config_t config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = address,
            .scl_speed_hz = 400000,
            .scl_wait_us = 0,
            .flags = {},
        };
        i2c_master_dev_handle_t device = nullptr;
        const esp_err_t result = i2c_master_bus_add_device(i2c_bus_, &config, &device);
        if (result != ESP_OK) {
            ESP_LOGW(TAG, "Failed to add I2C device 0x%02x: %s",
                     address, esp_err_to_name(result));
            return nullptr;
        }
        return device;
    }

    static bool ReadI2cRegister(i2c_master_dev_handle_t device, uint8_t reg,
                                uint8_t* data, size_t size) {
        return i2c_master_transmit_receive(
                   device, &reg, 1, data, size, 100) == ESP_OK;
    }

    void InitializeBatteryGauge() {
        if (i2c_master_probe(i2c_bus_, BATTERY_GAUGE_I2C_ADDRESS, 100) != ESP_OK) {
            ESP_LOGW(TAG, "Battery gauge not detected");
            return;
        }
        battery_device_ = AddI2cDevice(BATTERY_GAUGE_I2C_ADDRESS);
    }

    static void TouchIsr(void* arg) {
        auto* board = static_cast<EchoEarBoard*>(arg);
        BaseType_t task_woken = pdFALSE;
        xSemaphoreGiveFromISR(board->touch_semaphore_, &task_woken);
        if (task_woken == pdTRUE) {
            portYIELD_FROM_ISR(task_woken);
        }
    }

    static void TouchTask(void* arg) {
        auto* board = static_cast<EchoEarBoard*>(arg);
        while (true) {
            if (xSemaphoreTake(board->touch_semaphore_, portMAX_DELAY) != pdTRUE) {
                continue;
            }
            uint8_t data[6] = {};
            if (!ReadI2cRegister(board->touch_device_, 0x02, data, sizeof(data))) {
                continue;
            }
            const bool pressed = (data[0] & 0x0f) != 0;
            const bool was_pressed = board->touch_was_pressed_.exchange(pressed);
            if (!pressed && was_pressed) {
                if (board->screen_is_off_.load()) {
                    board->WakeUpScreen();
                    ESP_LOGI(TAG, "熄屏触摸仅恢复屏幕，不启动对话");
                    continue;
                }
                auto& app = Application::GetInstance();
                if (BackendService::GetInstance().ExitBindingPage()) {
                    continue;
                }
                if (app.GetDeviceState() == kDeviceStateStarting) {
                    board->EnterWifiConfigMode();
                } else {
                    app.ToggleChatState();
                }
            }
        }
    }

    void InitializeTouch() {
        if (i2c_master_probe(i2c_bus_, TOUCH_I2C_ADDRESS, 100) != ESP_OK) {
            ESP_LOGW(TAG, "CST816S touch controller not detected");
            return;
        }
        touch_device_ = AddI2cDevice(TOUCH_I2C_ADDRESS);
        if (touch_device_ == nullptr) {
            return;
        }
        touch_semaphore_ = xSemaphoreCreateBinary();
        ESP_ERROR_CHECK(touch_semaphore_ == nullptr ? ESP_ERR_NO_MEM : ESP_OK);

        const gpio_config_t config = {
            .pin_bit_mask = 1ULL << TOUCH_INT_PIN,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_ANYEDGE,
        };
        ESP_ERROR_CHECK(gpio_config(&config));
        const esp_err_t isr_result = gpio_install_isr_service(0);
        if (isr_result != ESP_OK && isr_result != ESP_ERR_INVALID_STATE) {
            ESP_ERROR_CHECK(isr_result);
        }
        ESP_ERROR_CHECK(gpio_isr_handler_add(TOUCH_INT_PIN, TouchIsr, this));
        BaseType_t task_result = xTaskCreatePinnedToCore(
            TouchTask, "echoear_touch", 4096, this, 5, nullptr, 1);
        ESP_ERROR_CHECK(task_result == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    }

    void InitializeSpi() {
        const spi_bus_config_t config = ECHOEAR_ST77916_PANEL_BUS_QSPI_CONFIG(
            DISPLAY_QSPI_SCLK_PIN, DISPLAY_QSPI_D0_PIN, DISPLAY_QSPI_D1_PIN,
            DISPLAY_QSPI_D2_PIN, DISPLAY_QSPI_D3_PIN,
            DISPLAY_WIDTH * 80 * sizeof(uint16_t));
        ESP_ERROR_CHECK(spi_bus_initialize(
            DISPLAY_QSPI_HOST, &config, SPI_DMA_CH_AUTO));
    }

    void InitializeDisplay() {
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_QSPI_CS_PIN;
        io_config.dc_gpio_num = GPIO_NUM_NC;
        io_config.spi_mode = 0;
        io_config.pclk_hz = DISPLAY_QSPI_PIXEL_CLOCK_HZ;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 32;
        io_config.lcd_param_bits = 8;
        io_config.flags.quad_mode = true;

        esp_lcd_panel_io_handle_t panel_io = nullptr;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
            DISPLAY_QSPI_HOST,
            &io_config, &panel_io));

        st77916_vendor_config_t vendor_config = {
            .init_cmds = lcd_init_cmds,
            .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]),
            .flags = {
                .use_qspi_interface = 1,
            },
        };
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = DISPLAY_QSPI_BIT_PER_PIXEL;
        panel_config.reset_gpio_num = display_reset_pin_;
        panel_config.vendor_config = &vendor_config;
        panel_config.flags.reset_active_high = pcb_v12_;

        esp_lcd_panel_handle_t panel = nullptr;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st77916(
            panel_io, &panel_config, &panel));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
        ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY));
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(
            panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y));

        display_ = new SpiLcdDisplay(
            panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT,
            DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
            DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (BackendService::GetInstance().ExitBindingPage()) {
                return;
            }
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            if (press_to_talk_tool_ == nullptr ||
                !press_to_talk_tool_->IsPressToTalkEnabled()) {
                app.ToggleChatState();
            }
        });
        boot_button_.OnPressDown([this]() {
            if (press_to_talk_tool_ != nullptr &&
                press_to_talk_tool_->IsPressToTalkEnabled()) {
                Application::GetInstance().StartListening();
            }
        });
        boot_button_.OnPressUp([this]() {
            if (press_to_talk_tool_ != nullptr &&
                press_to_talk_tool_->IsPressToTalkEnabled()) {
                Application::GetInstance().StopListening();
            }
        });
    }

    void InitializeTools() {
        press_to_talk_tool_ = new PressToTalkMcpTool();
        press_to_talk_tool_->Initialize();
    }
};

DECLARE_BOARD(EchoEarBoard);
