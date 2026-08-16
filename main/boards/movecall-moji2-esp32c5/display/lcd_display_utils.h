#ifndef LCD_DISPLAY_UTILS_H
#define LCD_DISPLAY_UTILS_H

/**
 * @file lcd_display_utils.h
 * @brief LCD 页面实现共享的布局常量、日期换算和 LVGL 更新工具。
 */
#include "lcd_display.h"
#include "fonts/ui_fonts.h"
#include "gif/lvgl_gif.h"
#include "system/settings.h"
#include "lvgl_theme.h"
#include "lvgl_font.h"
#include "assets/lang_config.h"
#include "assets.h"

#include <array>
#include <vector>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <font_awesome.h>
#include <esp_log.h>
#include <esp_err.h>
#include <esp_lvgl_port.h>
#include <esp_psram.h>
#include <cstring>
#include <src/misc/cache/lv_cache.h>

#include "board.h"

#define TAG "LcdDisplay"

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);
LV_FONT_DECLARE(BUILTIN_ICON_FONT);
LV_FONT_DECLARE(font_puhui_basic_20_4);
LV_FONT_DECLARE(font_awesome_30_4);

namespace display::internal {

extern const uint8_t screensaver_dial_rgb565_start[]
    asm("_binary_screensaver_dial_rgb565_start");
constexpr uint32_t kScreensaverDialPixelBytes = 360 * 360 * sizeof(uint16_t);

const lv_image_dsc_t kScreensaverDialImage = {
    .header = {
        .magic = LV_IMAGE_HEADER_MAGIC,
        .cf = LV_COLOR_FORMAT_RGB565,
        .flags = 0,
        .w = 360,
        .h = 360,
        .stride = 360 * sizeof(uint16_t),
        .reserved_2 = 0,
    },
    .data_size = kScreensaverDialPixelBytes,
    .data = screensaver_dial_rgb565_start,
    .reserved = nullptr,
    .reserved_2 = nullptr,
};

/**
 * @brief 360x360 圆屏中不会被圆形边缘裁切的固定布局参数。
 * @details 顶部图标栏下移到圆弦宽度足够的位置；字幕区域收窄并上移，
 * 确保容器的四角仍位于可见圆内。
 */

// 顶部状态栏容器的总宽度。当前状态栏水平居中，因此：屏幕宽度：360px；状态栏宽度：170px；左右剩余：(360-170)/2=95px
// 状态栏大致覆盖 X=95～265。减小它可以避免圆屏左右边缘裁切，但网络、时间、电量之间会更拥挤。
constexpr int kRoundTopBarWidth = 160;
// 顶部状态栏容器高度，同时用于计算圆角字幕区域的最大高度，避免被圆形边缘裁切。
constexpr int kRoundTopBarHeight = 40;
// 状态栏顶部距离屏幕顶部的距离，数值减小会让状态栏向上移动，设为0就紧贴屏幕顶部。但圆屏顶部非常窄，太靠上可能导致网络和电量图标被圆形边缘裁切。
constexpr int kRoundTopBarOffsetY = 5;
// 网络图标和电量图标各自占用的固定宽度。
// | 32 px 网络 | 中间剩余空间显示时间 | 32 px 电量 |
constexpr int kRoundTopBarSideWidth = 32;
// 底部字幕背景容器的实际显示宽度。它控制字幕半透明背景的宽度，不完全等于文字测量宽度。
// 当前容器水平居中，大约覆盖：X = (360 - 248) / 2 = 56，范围：56～304；
// 减小后更适合圆屏边缘，但每行可显示的文字会减少。
constexpr int kRoundSubtitleWidth = 248;
// 字幕文字在缩放前用于测量、换行和截断的逻辑宽度。
// 之所以比容器的248px更大，是因为文字最终还会按照 205/256 缩小：264×205/256≈220px
// 也就是说：逻辑测量宽度：264px；缩放后的视觉宽度：约220px；字幕容器宽度：248px
// 剩余空间用于容器内边距，防止文字贴近背景边缘。
// 这个值增大后，每行允许保留更多文字，但过大可能导致缩放后的文字超出容器。
constexpr int kRoundSubtitleLogicalTextWidth = 264;
// 字幕容器距离屏幕底部的上移距离，负数表示从屏幕底部向上移动 40px。
// 数值增大，字幕会继续向上移动；数值减小，字幕会向屏幕底部移动。
// 太小容易被圆形屏幕下方边缘裁切，太大则可能遮挡中央表情。
constexpr int kRoundSubtitleBottomOffset = 40;
// 两行字幕之间的行间距，单位为像素。它同时参与：LVGL实际文字行间距设置；字幕总高度计算；文本是否能完整放入两行的测量。
// 例如字体行高为 30px，则两行逻辑高度大约为：30×2+4=64px
// 增大后两行文字更疏朗，但字幕区域会变高。
constexpr int kRoundSubtitleLineSpacing = 4;
/**
 * @brief 字幕纵向滚动速度，单位为屏幕像素每秒。
 * @details 速度按缩放后的实际可见距离计算，数值越小越便于阅读，滚动耗时越长。
 * 16：当前速度，较慢；22：稍快，适合阅读；26：推荐值；32：明显加快；40：较快。
 */
constexpr int kRoundSubtitleScrollPixelsPerSecond = 22;
/**
 * @brief 每轮字幕开始滚动前在第一行位置停留的时间，单位为毫秒。
 */
constexpr int kRoundSubtitleScrollStartDelayMs = 1200;
/**
 * @brief 字幕滚动到最后一行后停留的时间，单位为毫秒。
 */
constexpr int kRoundSubtitleScrollRepeatDelayMs = 1600;
// 字幕文字的 LVGL 缩放比例。
// LVGL 使用256表示原始大小的100%，所以：205/256≈0.801
// 当前使用的是约30px的字体，缩放后视觉大小为：30×205/256≈23px
// 修改它时通常还需要检查 kRoundSubtitleLogicalTextWidth ，因为文字视觉宽度也会随缩放比例变化。
constexpr int kRoundSubtitleScale = 205;
// 中央表情相对屏幕正中心的 Y 轴偏移。0 严格居中、-16 向上移动16px、+16 向下移动16px
constexpr int kRoundEmojiOffsetY = -16;
/** NOMI 风格双眼的固定布局和刷新参数。 */
constexpr int kConversationFaceTimerPeriodMs = 100;
constexpr int kConversationFaceCenterOffsetY = -18;
constexpr int kConversationFaceEyeCenterOffsetX = 52;
constexpr uint32_t kConversationFaceBackgroundColor = 0x000000;
constexpr uint32_t kConversationFaceEyeColor = 0xFFFFFF;
// 低电量等提示弹窗的宽度。
// 弹窗水平居中，因此当前范围大约是：X = (360 - 220) / 2 = 70，范围：70～290
constexpr int kRoundPopupWidth = 220;

// 整体的布局关系如下：

// 顶部 5px
// ┌──── 170×40 状态栏 ─────┐
// │ 网络      时间     电量 │
// └───────────────────────┘

//          状态提示
//        中央表情上移 16 px

// ┌──── 248 px 字幕区域 ─────┐
// │       两行 25 px 字幕    │
// └─────────────────────────┘
// 距离屏幕底部向上 50 px

/**
 * @brief 表盘最外层金属圆盘的直径，单位为物理像素。
 * @details 当前圆屏逻辑尺寸为 360x360，设置为 348 后，圆盘四周各保留约 6px 边距。
 * 增大该值会让金属外圈更靠近屏幕边缘，过大会被圆形面板裁切；减小则会扩大
 * 表盘与屏幕边缘之间的黑色留白。
 */
constexpr int kScreensaverDialSize = 348;

/**
 * @brief 负责绘制 60 个分钟和秒钟刻度的 LVGL Scale 控件直径，单位为物理像素。
 * @details 该尺寸小于外层金属圆盘直径，使刻度完整落在金属边框内部。增大后刻度更靠近
 * 外圈，减小后刻度更靠近中心；修改时需要同时检查圆屏边缘是否发生裁切。
 */
constexpr int kScreensaverScaleSize = 330;

/**
 * @brief 单个秒刻度在表盘中的预计算布局。
 */
struct SecondMarkerLayout {
    int16_t x;
    int16_t y;
    int16_t rotation;
    uint8_t width;
    uint8_t height;
    bool major;
    bool visible;
};

/**
 * @brief 获取 60 个秒刻度预计算布局。
 * @return 按秒数索引的固定布局数组。
 * @details 坐标、尺寸和角度只在首次调用时计算一次，后续每秒刷新不再执行三角函数、
 * 半径换算和中心对齐计算。
 */
inline const std::array<SecondMarkerLayout, 60>& GetSecondMarkerLayouts() {
    static const std::array<SecondMarkerLayout, 60> layouts = [] {
        std::array<SecondMarkerLayout, 60> result = {};
        for (int second = 0; second < static_cast<int>(result.size()); ++second) {
            const bool major = second % 5 == 0;
            const int width = major ? 4 : 3;
            const int height = major ? 14 : 9;
            const int radius = kScreensaverScaleSize / 2 - height / 2;
            const int angle = second * 6;
            const int x_offset =
                (radius * lv_trigo_sin(angle)) >> LV_TRIGO_SHIFT;
            const int y_offset =
                -(radius * lv_trigo_cos(angle) >> LV_TRIGO_SHIFT);

            result[second] = {
                .x = static_cast<int16_t>((LV_HOR_RES - width) / 2 + x_offset),
                .y = static_cast<int16_t>((LV_VER_RES - height) / 2 + y_offset),
                .rotation = static_cast<int16_t>(angle * 10),
                .width = static_cast<uint8_t>(width),
                .height = static_cast<uint8_t>(height),
                .major = major,
                .visible = second != 0 && second != 30,
            };
        }
        return result;
    }();
    return layouts;
}

/**
 * @brief LVGL 渲染任务优先级，高于非实时后端同步任务但低于音频输入输出任务。
 */
constexpr int kResponsiveLvglTaskPriority = 2;

/**
 * @brief QSPI 屏幕单个 DMA 绘制缓冲包含的行数。
 * @details 24 行约占 17.3KB 内部 DMA SRAM，可在保留 TLS 内存余量的同时减少 40MHz QSPI
 * 下的 flush 次数；仍然使用单缓冲，避免双缓冲在当前硬件上的稳定性问题。
 */
constexpr uint32_t kSpiLcdDrawBufferLines = 24;

/**
 * @brief 时分秒组合容器的 LVGL 缩放比例。
 * @details LVGL 使用 256 表示原始尺寸的 100%，608/256 等于 237.5%。当前时间使用约 30px
 * 的内置字体，最终视觉高度约为 71.25px。增大该值会同时放大时分、秒数、描边和
 * 两者总宽度，过大可能超出圆屏左右安全区域。
 */
constexpr int kScreensaverTimeScale = 608;

/**
 * @brief 时分与秒数之间的最终可见间距，单位为物理像素。
 * @details 时间组合容器会按照 kScreensaverTimeScale 整体放大，因此创建 Flex 布局时会将
 * 该值反算为缩放前的逻辑间距，避免直接设置 10 导致实际间距被同步放大到 22.5px。
 */
constexpr int kScreensaverTimeColumnGap = 10;

/**
 * @brief 表盘普通辅助文字的目标视觉字号，单位为像素。
 * @details 当前主要用于时间下方的备忘录正文。完整主题字体通常为 30px，代码会根据字体
 * 实际行高自动换算 LVGL 缩放比例，使最终视觉字号接近 28px。大字号能够提升圆屏
 * 阅读距离，同时仍为三行短备忘录保留足够垂直空间。
 */
constexpr int kScreensaverSmallTextPixelSize = 28;

/**
 * @brief 时间下方三行备忘录区域的最大可见宽度，单位为物理像素。
 * @details 该宽度同时作为文本自动换行的最终视觉宽度。实际绘制时再由三条宽度逐渐收窄
 * 的水平裁切带限制每行两侧，使整体轮廓顺应圆屏下半部分的弧形。
 */
constexpr int kScreensaverMemoWidth = 228;

/**
 * @brief 备忘录固定显示的行数。
 * @details 结构化备忘录固定使用一行时间和两行正文，第三行末尾按需显示省略号；加载状态等
 * 兼容文本仍可使用原有纵向滚动，不启用左右滚动。
 */
constexpr int kScreensaverMemoVisibleLines = 3;

/**
 * @brief 三行备忘录从上到下各自允许显示的物理宽度。
 * @details 备忘录位于圆心下方，越靠近屏幕底部，可用圆弦宽度越小，因此依次使用
 * 228px、208px 和 180px。三条裁切带只限制文字可见区域，不绘制背景，也不会
 * 覆盖其外侧的表盘刻度。
 */
constexpr int kScreensaverMemoRowWidths[kScreensaverMemoVisibleLines] = {
    228,
    208,
    180,
};

/**
 * @brief 备忘录相邻两行之间的最终可见间距，单位为物理像素。
 */
constexpr int kScreensaverMemoLineSpacing = 5;

/**
 * @brief 三行备忘录裁切视口的最终可见高度，单位为物理像素。
 * @details 三行 28px 正文和两个 5px 行间距需要 94px，额外增加 2px 用于吸收字体缩放
 * 的向上取整误差，确保恰好三行时不会因为 1px 溢出而错误启动滚动。
 */
constexpr int kScreensaverMemoViewportHeight =
    kScreensaverSmallTextPixelSize * kScreensaverMemoVisibleLines
    + kScreensaverMemoLineSpacing * (kScreensaverMemoVisibleLines - 1) + 2;

/**
 * @brief 每条水平裁切带的高度，单位为物理像素。
 * @details 96px 总视口被等分为三个连续区域，每个区域高 32px。裁切带之间没有空隙，
 * 因此同一文本经过三个区域时仍保持连续，不会出现横向接缝或丢失扫描行。
 */
constexpr int kScreensaverMemoRowHeight =
    kScreensaverMemoViewportHeight / kScreensaverMemoVisibleLines;
static_assert(kScreensaverMemoViewportHeight % kScreensaverMemoVisibleLines == 0,
              "备忘录视口高度必须能够被三条裁切带整除");

/**
 * @brief 兼容超长状态文本向上滚动的目标速度，单位为物理像素每秒。
 * @details 结构化备忘录不会使用该值；兼容文本继续复用普通对话字幕速度。
 */
constexpr int kScreensaverMemoScrollPixelsPerSecond =
    kRoundSubtitleScrollPixelsPerSecond;

/**
 * @brief 超长备忘录开始向上滚动前停留在首行位置的时间，单位为毫秒。
 * @details 直接复用普通对话字幕的起始停留时间。
 */
constexpr int kScreensaverMemoScrollStartDelayMs =
    kRoundSubtitleScrollStartDelayMs;

/**
 * @brief 超长备忘录滚动到末尾后停留的时间，单位为毫秒。
 * @details 直接复用普通对话字幕的末尾停留时间，避免屏保滚动产生不同的顿挫感。
 */
constexpr int kScreensaverMemoScrollEndDelayMs =
    kRoundSubtitleScrollRepeatDelayMs;

/**
 * @brief 天气位置名称标签的最终可见宽度，单位为物理像素。
 * @details 220px 可以在圆屏顶部安全区内容纳常见完整省市区名称。
 */
constexpr int kScreensaverLocationWidth = 220;

/**
 * @brief 天气三列相邻内容之间的最终可见间距，单位为像素。
 * @details 该值描述的是缩放后的屏幕视觉间距。代码会根据天气字体的缩放比例反算 Flex
 * 容器所需的逻辑间距，因此当前温度、天气描述和温度范围之间约保持 15px 距离。
 */
constexpr int kScreensaverWeatherColumnGap = 15;

/**
 * @brief 日期三列相邻内容之间的最终可见间距，单位为像素。
 * @details 该值控制农历与公历、公历与星期之间的距离，并直接使用原生像素间距。
 */
constexpr int kScreensaverDateColumnGap = 10;

/**
 * @brief 备忘录区域相对屏幕中心的 Y 轴偏移，单位为物理像素。
 * @details LV_ALIGN_CENTER 以屏幕中心 Y=180 为基准，当前值 87 表示三行视口中心位于
 * Y=267，顶部约为 Y=219，恰好避开大时间；底部约为 Y=315，该位置的圆屏有效
 * 弦宽约 238px，228px 视口可在左右各保留约 5px。减小该值会靠近时间，增大则更接近电量图标。
 */
constexpr int kScreensaverBottomSectionOffsetY = 87;

/**
 * @brief 12 点 Wi-Fi 图标和 6 点电量图标距离圆屏上下边缘的偏移，单位为物理像素。
 * @details 0/60 和 30 对应的 Scale 主刻度会被透明区段隐藏。设置为 0px 后，Wi-Fi 标签
 * 顶边与屏幕顶边对齐，电量标签底边与屏幕底边对齐，让两个图标贴近圆屏边缘。
 */
constexpr int kScreensaverDialStatusInset = 0;

/**
 * @brief 自定义 MCP 清单根容器、固定标题和单项轮播区域尺寸。
 * @details 根容器位于刻度内；30px 标题固定在顶部，当前 MCP 在下方 190px 高圆角区域中
 * 垂直居中显示。
 */
constexpr int kCustomMcpListViewportWidth = 286;
constexpr int kCustomMcpListViewportHeight = 286;
constexpr int kCustomMcpListTitleWidth = 250;
constexpr int kCustomMcpListTitleTextPixelSize = 30;
constexpr int kCustomMcpListTitleTopOffset = 20;
constexpr int kCustomMcpListItemViewportWidth = 286;
constexpr int kCustomMcpListItemViewportHeight = 190;
constexpr int kCustomMcpListItemViewportOffsetY = 25;
constexpr int kCustomMcpListTextWidth = 250;
constexpr int kCustomMcpListViewportRadius = 32;
constexpr int kCustomMcpListItemSafeHeight = 174;

/**
 * @brief 自定义 MCP 单项内容的视觉字号、行距和切换周期。
 */
constexpr int kCustomMcpListTextPixelSize = 25;
constexpr int kCustomMcpListLineSpacing = 5;
constexpr int kCustomMcpListSwitchPeriodMs = 5000;

/**
 * @brief 屏保全屏根容器的背景颜色，格式为 0xRRGGBB。
 * @details 0x050607 是接近纯黑的冷色金属黑，用于覆盖普通页面并作为整个表盘最底层背景。
 * 保留少量 RGB 亮度可以避免纯黑背景与内层金属区域完全失去层次。
 */
constexpr uint32_t kScreensaverBackgroundColor = 0x050607;

/**
 * @brief 表盘外层金属圆盘的填充颜色，格式为 0xRRGGBB。
 * @details 0x15191D 是偏冷的枪灰色，比全屏背景更亮，用于表现表壳外圈与背景之间的层次。
 */
constexpr uint32_t kScreensaverOuterMetalColor = 0x15191D;

/**
 * @brief 表盘内层圆盘的填充颜色，格式为 0xRRGGBB。
 * @details 0x0A0D10 比外层金属色更暗，用于承载时间、天气和日期内容，同时保持白色文字
 * 和橙色强调元素具有足够对比度。
 */
constexpr uint32_t kScreensaverInnerMetalColor = 0x0A0D10;

/**
 * @brief 外层金属圆盘边框的高光颜色，格式为 0xRRGGBB。
 * @details 0x454B52 是中等亮度冷灰色，用于绘制 2px 金属边缘，模拟表壳受光后的细窄高光。
 */
constexpr uint32_t kScreensaverMetalBorderColor = 0x454B52;

/**
 * @brief 表盘次要信息文字的颜色，格式为 0xRRGGBB。
 * @details 0xAEB4BA 是低于纯白亮度的浅灰色，当前用于天气描述等辅助信息，使其不会与
 * 主时间、当前温度等主要数据争夺视觉焦点。
 */
constexpr uint32_t kScreensaverSecondaryTextColor = 0xAEB4BA;

/**
 * @brief 表盘普通分钟刻度的颜色，格式为 0xRRGGBB。
 * @details 0x8B9299 是中性冷灰色，用于 60 个常规刻度；当前秒对应的刻度会被橙色强调色覆盖。
 */
constexpr uint32_t kScreensaverTickColor = 0x8B9299;

/**
 * @brief 表盘重点状态的统一强调颜色，格式为 0xRRGGBB。
 * @details 0xF28A3A 是暖橙色，当前用于秒数、当前秒刻度、断网状态、充电状态和低电量。
 * 集中使用同一强调色可以形成统一视觉语言，同时避免表盘出现过多高饱和颜色。
 */
constexpr uint32_t kScreensaverAccentColor = 0xF28A3A;

/**
 * @brief 绑定页面标题、绑定码和说明文字的目标视觉字号，单位为物理像素。
 * @details 标题使用 Heavy 字体放大显示，说明文字使用资源分区中的 Heavy24 原生字号。
 */
constexpr int kBindingTitleTextPixelSize = 30;
constexpr int kBindingCodeTextPixelSize = 64;
constexpr int kBindingMessageTextPixelSize = 24;

/**
 * @brief 绑定码金属框和页面说明文字的圆屏安全尺寸，单位为物理像素。
 * @details 绑定码框位于圆心附近，可使用较宽区域；说明文字固定为两行并位于下半圆安全区。
 */
constexpr int kBindingCodePanelWidth = 324;
constexpr int kBindingCodePanelHeight = 108;
constexpr int kBindingMessageWidth = 300;

/**
 * @brief 绑定页面主要控件相对于圆屏边缘或圆心的垂直位置，单位为物理像素。
 */
constexpr int kBindingTitleOffsetY = 58;
constexpr int kBindingCodeOffsetY = -3;
constexpr int kBindingMessageBottomOffset = 50;

/**
 * @brief 1900-2100 年农历大小月和闰月编码表。
 * @details 每项低 4 位表示闰月月份；0x10000 表示闰月为 30 天；0x8000 至 0x10
 * 依次表示正月至腊月是否为 30 天。未置位的月份为 29 天。
 */
constexpr uint32_t kLunarYearInfo[] = {
    0x04bd8, 0x04ae0, 0x0a570, 0x054d5, 0x0d260, 0x0d950, 0x16554, 0x056a0, 0x09ad0, 0x055d2,
    0x04ae0, 0x0a5b6, 0x0a4d0, 0x0d250, 0x1d255, 0x0b540, 0x0d6a0, 0x0ada2, 0x095b0, 0x14977,
    0x04970, 0x0a4b0, 0x0b4b5, 0x06a50, 0x06d40, 0x1ab54, 0x02b60, 0x09570, 0x052f2, 0x04970,
    0x06566, 0x0d4a0, 0x0ea50, 0x06e95, 0x05ad0, 0x02b60, 0x186e3, 0x092e0, 0x1c8d7, 0x0c950,
    0x0d4a0, 0x1d8a6, 0x0b550, 0x056a0, 0x1a5b4, 0x025d0, 0x092d0, 0x0d2b2, 0x0a950, 0x0b557,
    0x06ca0, 0x0b550, 0x15355, 0x04da0, 0x0a5b0, 0x14573, 0x052b0, 0x0a9a8, 0x0e950, 0x06aa0,
    0x0aea6, 0x0ab50, 0x04b60, 0x0aae4, 0x0a570, 0x05260, 0x0f263, 0x0d950, 0x05b57, 0x056a0,
    0x096d0, 0x04dd5, 0x04ad0, 0x0a4d0, 0x0d4d4, 0x0d250, 0x0d558, 0x0b540, 0x0b5a0, 0x195a6,
    0x095b0, 0x049b0, 0x0a974, 0x0a4b0, 0x0b27a, 0x06a50, 0x06d40, 0x0af46, 0x0ab60, 0x09570,
    0x04af5, 0x04970, 0x064b0, 0x074a3, 0x0ea50, 0x06b58, 0x05ac0, 0x0ab60, 0x096d5, 0x092e0,
    0x0c960, 0x0d954, 0x0d4a0, 0x0da50, 0x07552, 0x056a0, 0x0abb7, 0x025d0, 0x092d0, 0x0cab5,
    0x0a950, 0x0b4a0, 0x0baa4, 0x0ad50, 0x055d9, 0x04ba0, 0x0a5b0, 0x15176, 0x052b0, 0x0a930,
    0x07954, 0x06aa0, 0x0ad50, 0x05b52, 0x04b60, 0x0a6e6, 0x0a4e0, 0x0d260, 0x0ea65, 0x0d530,
    0x05aa0, 0x076a3, 0x096d0, 0x04afb, 0x04ad0, 0x0a4d0, 0x1d0b6, 0x0d250, 0x0d520, 0x0dd45,
    0x0b5a0, 0x056d0, 0x055b2, 0x049b0, 0x0a577, 0x0a4b0, 0x0aa50, 0x1b255, 0x06d20, 0x0ada0,
    0x14b63, 0x09370, 0x049f8, 0x04970, 0x064b0, 0x168a6, 0x0ea50, 0x06b20, 0x1a6c4, 0x0aae0,
    0x0a2e0, 0x0d2e3, 0x0c960, 0x0d557, 0x0d4a0, 0x0da50, 0x05d55, 0x056a0, 0x0a6d0, 0x055d4,
    0x052d0, 0x0a9b8, 0x0a950, 0x0b4a0, 0x0b6a6, 0x0ad50, 0x055a0, 0x0aba4, 0x0a5b0, 0x052b0,
    0x0b273, 0x06930, 0x07337, 0x06aa0, 0x0ad50, 0x14b55, 0x04b60, 0x0a570, 0x054e4, 0x0d160,
    0x0e968, 0x0d520, 0x0daa0, 0x16aa6, 0x056d0, 0x04ae0, 0x0a9d4, 0x0a2d0, 0x0d150, 0x0f252,
    0x0d520,
};

constexpr int kLunarBaseYear = 1900;
constexpr int kLunarMaxYear = 2100;
static_assert(sizeof(kLunarYearInfo) / sizeof(kLunarYearInfo[0]) ==
              kLunarMaxYear - kLunarBaseYear + 1,
              "农历年份编码表长度必须覆盖 1900-2100 年");

/**
 * @brief 将公历日期转换为相对 1970-01-01 的连续天数。
 * @param year 公历年份。
 * @param month 公历月份，范围 1-12。
 * @param day 公历日，范围 1-31。
 * @return 指定日期对应的连续天数，可用于不受时区和夏令时影响的日期差计算。
 */
constexpr int64_t DaysFromCivil(int year, unsigned month, unsigned day) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned year_of_era = static_cast<unsigned>(year - era * 400);
    const int adjusted_month = static_cast<int>(month) + (month > 2 ? -3 : 9);
    const unsigned day_of_year =
        static_cast<unsigned>((153 * adjusted_month + 2) / 5) + day - 1;
    const unsigned day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(day_of_era) - 719468;
}

/**
 * @brief 获取指定农历年的闰月月份。
 * @param year 农历年份，支持 1900-2100。
 * @return 1-12 表示对应闰月，0 表示该年没有闰月或年份超出范围。
 */
inline int LunarLeapMonth(int year) {
    if (year < kLunarBaseYear || year > kLunarMaxYear) {
        return 0;
    }
    return static_cast<int>(kLunarYearInfo[year - kLunarBaseYear] & 0x0f);
}

/**
 * @brief 获取指定农历年的闰月天数。
 * @param year 农历年份，支持 1900-2100。
 * @return 有闰月时返回 29 或 30，没有闰月时返回 0。
 */
inline int LunarLeapMonthDays(int year) {
    if (LunarLeapMonth(year) == 0) {
        return 0;
    }
    return (kLunarYearInfo[year - kLunarBaseYear] & 0x10000) != 0 ? 30 : 29;
}

/**
 * @brief 获取指定农历普通月份的天数。
 * @param year 农历年份，支持 1900-2100。
 * @param month 农历月份，范围 1-12。
 * @return 参数有效时返回 29 或 30，否则返回 0。
 */
inline int LunarMonthDays(int year, int month) {
    if (year < kLunarBaseYear || year > kLunarMaxYear || month < 1 || month > 12) {
        return 0;
    }
    return (kLunarYearInfo[year - kLunarBaseYear] & (0x10000 >> month)) != 0 ? 30 : 29;
}

/**
 * @brief 计算指定农历年的总天数。
 * @param year 农历年份，支持 1900-2100。
 * @return 该农历年包含的总天数；年份无效时返回 0。
 */
inline int LunarYearDays(int year) {
    if (year < kLunarBaseYear || year > kLunarMaxYear) {
        return 0;
    }

    int days = 0;
    for (int month = 1; month <= 12; ++month) {
        days += LunarMonthDays(year, month);
    }
    return days + LunarLeapMonthDays(year);
}

/**
 * @brief 将公历日期换算为中文农历月日。
 * @param year 公历年份。
 * @param month 公历月份，范围 1-12。
 * @param day 公历日，范围 1-31。
 * @param output 接收 UTF-8 农历文本的缓冲区，例如“五月二十”或“闰二月初三”。
 * @param output_size output 缓冲区总字节数，必须包含字符串结束符空间。
 * @return 日期位于 1900-01-31 至 2100 年支持范围内且成功格式化时返回 true。
 */
inline bool FormatLunarDate(int year, unsigned month, unsigned day, char* output, size_t output_size) {
    if (output == nullptr || output_size == 0 || year < kLunarBaseYear || year > kLunarMaxYear) {
        return false;
    }

    const int64_t base_days = DaysFromCivil(1900, 1, 31);
    int64_t offset = DaysFromCivil(year, month, day) - base_days;
    if (offset < 0) {
        return false;
    }

    int lunar_year = kLunarBaseYear;
    while (lunar_year <= kLunarMaxYear) {
        const int year_days = LunarYearDays(lunar_year);
        if (offset < year_days) {
            break;
        }
        offset -= year_days;
        ++lunar_year;
    }
    if (lunar_year > kLunarMaxYear) {
        return false;
    }

    const int leap_month = LunarLeapMonth(lunar_year);
    int lunar_month = 1;
    bool is_leap_month = false;
    while (lunar_month <= 12) {
        int month_days = 0;
        if (leap_month > 0 && lunar_month == leap_month + 1 && !is_leap_month) {
            --lunar_month;
            is_leap_month = true;
            month_days = LunarLeapMonthDays(lunar_year);
        } else {
            month_days = LunarMonthDays(lunar_year, lunar_month);
        }

        if (offset < month_days) {
            break;
        }
        offset -= month_days;

        if (is_leap_month && lunar_month == leap_month) {
            is_leap_month = false;
        }
        ++lunar_month;
    }

    const int lunar_day = static_cast<int>(offset) + 1;
    if (lunar_month < 1 || lunar_month > 12 || lunar_day < 1 || lunar_day > 30) {
        return false;
    }

    static const char* month_names[] = {
        "正月", "二月", "三月", "四月", "五月", "六月",
        "七月", "八月", "九月", "十月", "冬月", "腊月",
    };
    static const char* day_names[] = {
        "", "初一", "初二", "初三", "初四", "初五", "初六", "初七", "初八", "初九", "初十",
        "十一", "十二", "十三", "十四", "十五", "十六", "十七", "十八", "十九", "二十",
        "廿一", "廿二", "廿三", "廿四", "廿五", "廿六", "廿七", "廿八", "廿九", "三十",
    };
    snprintf(output, output_size, "%s%s%s", is_leap_month ? "闰" : "",
             month_names[lunar_month - 1], day_names[lunar_day]);
    return true;
}

/**
 * @brief 将 30 px 字形的逻辑尺寸换算为约 25 px 缩放后的可见尺寸。
 * @param logical_size LVGL 按原始 30 px 字体计算的像素尺寸。
 * @return 按 256 表示 100% 的 LVGL 缩放规则向上取整后的可见尺寸。
 */
constexpr int ScaleSubtitleSize(int logical_size) {
    return (logical_size * kRoundSubtitleScale + 255) / 256;
}

/**
 * @brief 仅在内容变化时更新 LVGL 标签，减少每秒刷新表盘产生的无效重绘。
 * @param label 目标标签，可为空指针。
 * @param text 新的 UTF-8 文本，不得为空指针。
 */
inline void SetLabelTextIfChanged(lv_obj_t* label, const char* text) {
    if (label != nullptr && text != nullptr && strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

}  // namespace display::internal

#endif // LCD_DISPLAY_UTILS_H
