# CREDITS · 致谢与第三方许可说明

本固件基于 `xiaozhi-esp32` v2.2.6 裁剪并二次开发。除特别说明外，源码遵循根目录
[LICENSE](LICENSE)（MIT）。以下列出直接上游、内嵌素材与随组件分发的第三方资源的
许可信息，供开源分发与合规审计使用。

## 直接上游代码

| 项目 | 版本 | 许可 | 说明 |
| --- | --- | --- | --- |
| [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) | v2.2.6 | MIT | 语音助手框架，本仓库在此基础上裁剪、扩展；根 LICENSE 保留其版权声明 |
| [xiaozhi-fonts](https://github.com/78/xiaozhi-fonts) | 1.6.0 | MIT | 字体 / 表情资源转换组件，本项目 cbin 字体、图标与表情集合均由其提供 |
| [esp-wifi-connect](https://github.com/78/esp-wifi-connect) | 3.1.5 | MIT | SoftAP + 网页配网组件，本仓库内嵌于 `components/esp-wifi-connect` |
| [esp-ml307](https://github.com/78/esp-ml307) | ~3.6.6 | Apache-2.0 | 提供项目使用的 Http / WebSocket / Mqtt / Udp 公共接口 |

## 随组件分发的字体与表情素材

以下素材随 `xiaozhi-fonts` 及板级资源内嵌进固件，各自的来源与许可如下：

- **阿里巴巴普惠体（Alibaba PuHuiTi）**：阿里巴巴官方宣布免费商用。
  本项目使用的 `font_puhui_*` 系列 cbin 字体以及板级屏保字体
  `puhui3_heavy_24_2.bin` 均由普惠体生成。
- **Noto 系列（Noto Sans CJK / Noto Emoji）**：SIL OFL-1.1 / Apache-2.0。
  对应 `ttf/noto-basic.ttf`、`ttf/noto-qwen.ttf` 与 `noto-emoji_*` 表情集合。
- **Twemoji**：© Twitter/X, Inc. and contributors，基于
  [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/) 许可。
  板型 `config.json` 的 `emoji_collection: "twemoji_64"` 即来自该集合，本文件即履行署名义务。
- **Font Awesome（Free）**：图标基于 [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/)、
  字体基于 [SIL OFL-1.1](https://scripts.sil.org/OFL)。用作状态图标
  （`builtin_icon_font: "font_awesome_20_4"`）。

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
