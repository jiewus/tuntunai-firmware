#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

/**
 * @file config.h
 * @brief Tuntun Moji2 ESP32-C5 的固定硬件引脚与总线参数。
 *
 * 修改本文件前必须对照原理图。ESP32-C5 的 GPIO28 同时是下载模式绑带脚，
 * 上电/复位时按住 BOOT 会进入 ROM 烧录模式。
 */

#include <driver/gpio.h>

// ES8311 与 I2S 的原生采样率。网络编码需要 16 kHz 时由 AudioService 重采样。
#define AUDIO_INPUT_SAMPLE_RATE  24000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

// I2S 标准模式五线连接：MCLK、左右声道时钟、位时钟、Codec->MCU、MCU->Codec。
#define AUDIO_I2S_GPIO_MCLK         GPIO_NUM_25
#define AUDIO_I2S_GPIO_WS           GPIO_NUM_24
#define AUDIO_I2S_GPIO_BCLK         GPIO_NUM_11
#define AUDIO_I2S_GPIO_DIN          GPIO_NUM_12
#define AUDIO_I2S_GPIO_DOUT         GPIO_NUM_23

// 外置功放使能脚与 ES8311 控制总线。
#define AUDIO_CODEC_PA_PIN          GPIO_NUM_5
#define AUDIO_CODEC_I2C_SDA_PIN     GPIO_NUM_26
#define AUDIO_CODEC_I2C_SCL_PIN     GPIO_NUM_27
#define AUDIO_CODEC_ES8311_ADDR     ES8311_CODEC_DEFAULT_ADDR

// 单颗 WS2812 数据脚与兼作下载绑带脚的用户 BOOT 按键。
#define BUILTIN_LED_GPIO            GPIO_NUM_10
#define BOOT_BUTTON_GPIO            GPIO_NUM_28

// 圆形屏逻辑分辨率、镜像、坐标交换和可见区域偏移。
#define DISPLAY_WIDTH               360
#define DISPLAY_HEIGHT              360
#define DISPLAY_MIRROR_X            false
#define DISPLAY_MIRROR_Y            false
#define DISPLAY_SWAP_XY             false

#define DISPLAY_OFFSET_X            0
#define DISPLAY_OFFSET_Y            0

// 背光使用 LEDC PWM；本板为高电平增加亮度，因此不反相。
#define DISPLAY_BACKLIGHT_PIN       GPIO_NUM_2
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false

#define DISPLAY_QSPI_H_RES           (360)
#define DISPLAY_QSPI_V_RES           (360)
#define DISPLAY_QSPI_BIT_PER_PIXEL   (16)
// ST77916 QSPI 像素时钟，初始性能方案使用 80MHz。
#define DISPLAY_QSPI_PIXEL_CLOCK_HZ  (80 * 1000 * 1000)

// ST77916 使用 SPI2 的 QSPI 模式：一根时钟、四根数据和一根片选。
#define DISPLAY_QSPI_HOST           SPI2_HOST
#define DISPLAY_QSPI_SCLK_PIN       GPIO_NUM_0
#define DISPLAY_QSPI_RESET_PIN      GPIO_NUM_1
#define DISPLAY_QSPI_D0_PIN         GPIO_NUM_9
#define DISPLAY_QSPI_D1_PIN         GPIO_NUM_8
#define DISPLAY_QSPI_D2_PIN         GPIO_NUM_7
#define DISPLAY_QSPI_D3_PIN         GPIO_NUM_6
#define DISPLAY_QSPI_CS_PIN         GPIO_NUM_3


/**
 * @brief 生成 ST77916 QSPI 总线配置初始化器。
 * @param sclk QSPI 时钟 GPIO。
 * @param d0,d1,d2,d3 四根双向数据 GPIO。
 * @param max_trans_sz 单次 DMA 传输允许的最大字节数。
 */
#define MOJI2_ST77916_PANEL_BUS_QSPI_CONFIG(sclk, d0, d1, d2, d3, max_trans_sz) \
    {                                                                             \
        .data0_io_num = d0,                                                       \
        .data1_io_num = d1,                                                       \
        .sclk_io_num = sclk,                                                      \
        .data2_io_num = d2,                                                       \
        .data3_io_num = d3,                                                       \
        .max_transfer_sz = max_trans_sz,                                          \
    }

#endif // _BOARD_CONFIG_H_
