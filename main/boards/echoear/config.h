#ifndef ECHOEAR_BOARD_CONFIG_H
#define ECHOEAR_BOARD_CONFIG_H

#include <driver/gpio.h>
#include <driver/spi_master.h>

#define AUDIO_INPUT_SAMPLE_RATE 24000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000
#define AUDIO_INPUT_REFERENCE true

#define CODEC_POWER_CTRL GPIO_NUM_48
#define POWER_CTRL GPIO_NUM_9

#define AUDIO_I2S_GPIO_MCLK GPIO_NUM_42
#define AUDIO_I2S_GPIO_WS GPIO_NUM_39
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_40
#define AUDIO_I2S_GPIO_DIN_V10 GPIO_NUM_15
#define AUDIO_I2S_GPIO_DIN_V12 GPIO_NUM_3
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_41

#define AUDIO_CODEC_PA_PIN_V10 GPIO_NUM_4
#define AUDIO_CODEC_PA_PIN_V12 GPIO_NUM_15
#define AUDIO_CODEC_I2C_SDA_PIN GPIO_NUM_2
#define AUDIO_CODEC_I2C_SCL_PIN GPIO_NUM_1

#define BOOT_BUTTON_GPIO GPIO_NUM_0

#define DISPLAY_WIDTH 360
#define DISPLAY_HEIGHT 360
#define DISPLAY_OFFSET_X 0
#define DISPLAY_OFFSET_Y 0
#define DISPLAY_MIRROR_X false
#define DISPLAY_MIRROR_Y false
#define DISPLAY_SWAP_XY false
#define DISPLAY_BACKLIGHT_PIN GPIO_NUM_44
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false

#define DISPLAY_QSPI_HOST SPI2_HOST
#define DISPLAY_QSPI_PIXEL_CLOCK_HZ (40 * 1000 * 1000)
#define DISPLAY_QSPI_BIT_PER_PIXEL 16
#define DISPLAY_QSPI_SCLK_PIN GPIO_NUM_18
#define DISPLAY_QSPI_CS_PIN GPIO_NUM_14
#define DISPLAY_QSPI_D0_PIN GPIO_NUM_46
#define DISPLAY_QSPI_D1_PIN GPIO_NUM_13
#define DISPLAY_QSPI_D2_PIN GPIO_NUM_11
#define DISPLAY_QSPI_D3_PIN GPIO_NUM_12
#define DISPLAY_QSPI_RESET_PIN_V10 GPIO_NUM_3
#define DISPLAY_QSPI_RESET_PIN_V12 GPIO_NUM_47

#define TOUCH_I2C_ADDRESS 0x15
#define TOUCH_INT_PIN GPIO_NUM_10
#define BATTERY_GAUGE_I2C_ADDRESS 0x55
#define PCB_VERSION_I2C_ADDRESS 0x18

#define ECHOEAR_ST77916_PANEL_BUS_QSPI_CONFIG(sclk, d0, d1, d2, d3, max_trans_sz) \
    {                                                                             \
        .data0_io_num = d0,                                                       \
        .data1_io_num = d1,                                                       \
        .sclk_io_num = sclk,                                                      \
        .data2_io_num = d2,                                                       \
        .data3_io_num = d3,                                                       \
        .max_transfer_sz = max_trans_sz,                                          \
    }

#endif
