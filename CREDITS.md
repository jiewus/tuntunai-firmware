# CREDITS · 致谢与第三方许可说明

本固件基于 **阿慕希（MoveCall）Moji2.0 硬件**及其板级固件代码，结合 `xiaozhi-esp32`
v2.2.6 裁剪并二次开发。除特别说明外，源码遵循根目录 [LICENSE](LICENSE)（MIT）。
以下列出直接上游、内嵌素材与随组件分发的第三方资源的许可信息，供开源分发与合规审计使用。

## 直接上游代码

| 项目 | 版本 | 许可 | 说明 |
| --- | --- | --- | --- |
| [MoveCall（阿慕希）· moji2-esp32c5 板级固件](https://github.com/MoveCall/xiaozhi-esp32/tree/main/main/boards/movecall-moji2-esp32c5) | — | MIT | Moji2.0 硬件适配的板级驱动与 UI 参考实现，本项目板型代码的演进来源 |
| [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) | v2.2.6 | MIT | 语音助手框架，本仓库在此基础上裁剪、扩展；根 LICENSE 保留其版权声明 |
| [xiaozhi-fonts](https://github.com/78/xiaozhi-fonts) | 1.6.0 | MIT | 字体 / 表情资源转换组件，本项目 cbin 字体、图标与表情集合均由其提供 |
| [esp-wifi-connect](https://github.com/78/esp-wifi-connect) | 3.1.5 | MIT | SoftAP + 网页配网组件，本仓库内嵌于 `components/esp-wifi-connect` |
| [esp-ml307](https://github.com/78/esp-ml307) | ~3.6.6 | Apache-2.0 | 提供项目使用的 Http / WebSocket / Mqtt / Udp 公共接口 |

## 随组件分发的字体与表情素材

> 说明：本仓库基于上游 **xiaozhi-esp32** 固件体系裁剪与演进，以下字体、表情与图标素材均由该
> 固件体系（`xiaozhi-fonts` 组件）**引入并继承使用**，并非本项目自主开发。本项目仅沿用了其
> 字体选择、表情集合与图标字体，未对素材本身做二次创作。

以下素材随 `78/xiaozhi-fonts` 组件（[github.com/78/xiaozhi-fonts](https://github.com/78/xiaozhi-fonts)）下载并嵌入固件，
各自的来源、许可与使用位置如下：

- **阿里巴巴普惠体（Alibaba PuHuiTi）**
  - **来源**：阿里巴巴设计（Alibaba PuHuiTi，免费商用）。
  - **使用**：本项目选用的 `font_puhui_*` 系列 cbin 字体（板型 `config.json` 的 `builtin_text_font: "font_puhui_basic_30_4"`）
    以及板级屏保字体 `puhui3_heavy_24_2.bin` 均由普惠体生成，用于对话界面与表盘屏保的文字渲染。
- **Twemoji**
  - **来源**：© Twitter/X, Inc. and contributors，基于 [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/)。
  - **使用**：板型 `config.json` 的 `emoji_collection: "twemoji_64"` 选用 64px 集合，经
    `main/boards/movecall-moji2-esp32c5/display/lvgl_display/emoji_collection.{h,cc}` 的 `Twemoji64` 注册，
    嵌入资源分区，用于对话界面与界面元素中的表情显示。本文件即履行 CC BY 4.0 署名义务。
- **Font Awesome（Free）**
  - **来源**：Font Awesome，图标基于 [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/)、
    字体基于 [SIL OFL-1.1](https://scripts.sil.org/OFL)。
  - **使用**：板型 `config.json` 的 `builtin_icon_font: "font_awesome_20_4"` 选用；通过 `<font_awesome.h>`
    的 `FONT_AWESOME_*` 符号用于状态栏图标（如 `main/boards/common/wifi_board.cc` 中的 Wi-Fi 状态图标等）。

## 板上内嵌的第三方代码

- **gifdec**（`main/boards/.../display/lvgl_display/gif/`）：MIT，
  随附 [LICENSE.txt](main/boards/movecall-moji2-esp32c5/display/lvgl_display/gif/LICENSE.txt)。

## ESP-IDF 与组件管理器依赖

- **espressif/esp-sr**（WakeNet / AFE）：Espressif MIT License，仅限 ESPRESSIF 产品使用
  （组件自带 LICENSE）。使用 `esp-sr` 即表示接受其条款。
- 其余 Espressif 官方组件（`esp_audio_codec`、`esp_audio_effects`、`esp_codec_dev`、
  `esp_lcd_st77916`、`esp_new_jpeg`、`esp_image_effects`、`led_strip`、`button`、
  `adc_battery_estimation` 等）：许可见各组件随附的 LICENSE / README，多为 Apache-2.0 或 MIT。
- **lvgl/lvgl 9.5**：MIT，[LICENCE](https://github.com/lvgl/lvgl/blob/master/LICENCE.txt)。
- **esp_lvgl_port**：MIT / Apache-2.0（依版本）。

## 硬件

- 板型硬件原理图 / PCB：立创开源硬件平台
  [movecall/moji2](https://oshwhub.com/movecall/moji2)。
- 主控芯片：乐鑫 ESP32-C5；音频 Codec：ES8311；LCD：ST77916。

## 原创资源

- 屏保表盘、提示音等无第三方来源的素材为本项目制作，随项目 MIT 许可分发；
  若其中有继承自上游或第三方的内容，版权归原权利方所有，并以本文件所列许可为准。
