/**
 * @file lcd_display.cc
 * @brief lcd_display.cc 中各类和辅助函数的具体实现。
 */
#include "lcd_display.h"
#include "fonts/ui_fonts.h"
#include "gif/lvgl_gif.h"
#include "settings.h"
#include "lvgl_theme.h"
#include "assets/lang_config.h"

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

namespace {

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
 *          确保容器的四角仍位于可见圆内。
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
 *          增大该值会让金属外圈更靠近屏幕边缘，过大会被圆形面板裁切；减小则会扩大
 *          表盘与屏幕边缘之间的黑色留白。
 */
constexpr int kScreensaverDialSize = 348;

/**
 * @brief 负责绘制 60 个分钟和秒钟刻度的 LVGL Scale 控件直径，单位为物理像素。
 * @details 该尺寸小于外层金属圆盘直径，使刻度完整落在金属边框内部。增大后刻度更靠近
 *          外圈，减小后刻度更靠近中心；修改时需要同时检查圆屏边缘是否发生裁切。
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
 *          半径换算和中心对齐计算。
 */
const std::array<SecondMarkerLayout, 60>& GetSecondMarkerLayouts() {
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
 *          下的 flush 次数；仍然使用单缓冲，避免双缓冲在当前硬件上的稳定性问题。
 */
constexpr uint32_t kSpiLcdDrawBufferLines = 24;

/**
 * @brief 时分秒组合容器的 LVGL 缩放比例。
 * @details LVGL 使用 256 表示原始尺寸的 100%，608/256 等于 237.5%。当前时间使用约 30px
 *          的内置字体，最终视觉高度约为 71.25px。增大该值会同时放大时分、秒数、描边和
 *          两者总宽度，过大可能超出圆屏左右安全区域。
 */
constexpr int kScreensaverTimeScale = 608;

/**
 * @brief 时分与秒数之间的最终可见间距，单位为物理像素。
 * @details 时间组合容器会按照 kScreensaverTimeScale 整体放大，因此创建 Flex 布局时会将
 *          该值反算为缩放前的逻辑间距，避免直接设置 10 导致实际间距被同步放大到 22.5px。
 */
constexpr int kScreensaverTimeColumnGap = 10;

/**
 * @brief 表盘普通辅助文字的目标视觉字号，单位为像素。
 * @details 当前主要用于时间下方的备忘录正文。完整主题字体通常为 30px，代码会根据字体
 *          实际行高自动换算 LVGL 缩放比例，使最终视觉字号接近 28px。大字号能够提升圆屏
 *          阅读距离，同时仍为三行短备忘录保留足够垂直空间。
 */
constexpr int kScreensaverSmallTextPixelSize = 28;

/**
 * @brief 时间下方三行备忘录区域的最大可见宽度，单位为物理像素。
 * @details 该宽度同时作为文本自动换行的最终视觉宽度。实际绘制时再由三条宽度逐渐收窄
 *          的水平裁切带限制每行两侧，使整体轮廓顺应圆屏下半部分的弧形。
 */
constexpr int kScreensaverMemoWidth = 228;

/**
 * @brief 备忘录固定显示的行数。
 * @details 结构化备忘录固定使用一行时间和两行正文，第三行末尾按需显示省略号；加载状态等
 *          兼容文本仍可使用原有纵向滚动，不启用左右滚动。
 */
constexpr int kScreensaverMemoVisibleLines = 3;

/**
 * @brief 三行备忘录从上到下各自允许显示的物理宽度。
 * @details 备忘录位于圆心下方，越靠近屏幕底部，可用圆弦宽度越小，因此依次使用
 *          228px、208px 和 180px。三条裁切带只限制文字可见区域，不绘制背景，也不会
 *          覆盖其外侧的表盘刻度。
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
 *          的向上取整误差，确保恰好三行时不会因为 1px 溢出而错误启动滚动。
 */
constexpr int kScreensaverMemoViewportHeight =
    kScreensaverSmallTextPixelSize * kScreensaverMemoVisibleLines
    + kScreensaverMemoLineSpacing * (kScreensaverMemoVisibleLines - 1) + 2;

/**
 * @brief 每条水平裁切带的高度，单位为物理像素。
 * @details 96px 总视口被等分为三个连续区域，每个区域高 32px。裁切带之间没有空隙，
 *          因此同一文本经过三个区域时仍保持连续，不会出现横向接缝或丢失扫描行。
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
 *          容器所需的逻辑间距，因此当前温度、天气描述和温度范围之间约保持 15px 距离。
 */
constexpr int kScreensaverWeatherColumnGap = 15;

/**
 * @brief 日期三列相邻内容之间的最终可见间距，单位为像素。
 * @details 该值控制农历与公历、公历与星期之间的距离，并直接使用原生像素间距。
 */
constexpr int kScreensaverDateColumnGap = 20;

/**
 * @brief 备忘录区域相对屏幕中心的 Y 轴偏移，单位为物理像素。
 * @details LV_ALIGN_CENTER 以屏幕中心 Y=180 为基准，当前值 87 表示三行视口中心位于
 *          Y=267，顶部约为 Y=219，恰好避开大时间；底部约为 Y=315，该位置的圆屏有效
 *          弦宽约 238px，228px 视口可在左右各保留约 5px。减小该值会靠近时间，增大则更接近电量图标。
 */
constexpr int kScreensaverBottomSectionOffsetY = 87;

/**
 * @brief 12 点 Wi-Fi 图标和 6 点电量图标距离圆屏上下边缘的偏移，单位为物理像素。
 * @details 0/60 和 30 对应的 Scale 主刻度会被透明区段隐藏。设置为 0px 后，Wi-Fi 标签
 *          顶边与屏幕顶边对齐，电量标签底边与屏幕底边对齐，让两个图标贴近圆屏边缘。
 */
constexpr int kScreensaverDialStatusInset = 0;

/**
 * @brief 自定义 MCP 清单根容器、固定标题和单项轮播区域尺寸。
 * @details 根容器位于刻度内；30px 标题固定在顶部，当前 MCP 在下方 190px 高圆角区域中
 *          垂直居中显示。
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
 *          保留少量 RGB 亮度可以避免纯黑背景与内层金属区域完全失去层次。
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
 *          和橙色强调元素具有足够对比度。
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
 *          主时间、当前温度等主要数据争夺视觉焦点。
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
 *          集中使用同一强调色可以形成统一视觉语言，同时避免表盘出现过多高饱和颜色。
 */
constexpr uint32_t kScreensaverAccentColor = 0xF28A3A;

/**
 * @brief 绑定页面标题、绑定码和说明文字的目标视觉字号，单位为物理像素。
 * @details 三种字号会根据当前主题字体的实际行高换算为 LVGL 缩放比例，使内置字体和
 *          下载后的完整中文字库保持一致的视觉层级。
 */
constexpr int kBindingTitleTextPixelSize = 26;
constexpr int kBindingCodeTextPixelSize = 64;
constexpr int kBindingMessageTextPixelSize = 22;

/**
 * @brief 绑定码金属框和页面说明文字的圆屏安全尺寸，单位为物理像素。
 * @details 绑定码框位于圆心附近，可使用较宽区域；说明文字靠近下半圆，因此使用更窄的
 *          宽度并允许自动换行，避免字符被圆形面板边缘裁切。
 */
constexpr int kBindingCodePanelWidth = 258;
constexpr int kBindingCodePanelHeight = 108;
constexpr int kBindingMessageWidth = 250;

/**
 * @brief 绑定页面主要控件相对于圆屏边缘或圆心的垂直位置，单位为物理像素。
 */
constexpr int kBindingTitleOffsetY = 58;
constexpr int kBindingCodeOffsetY = -3;
constexpr int kBindingMessageBottomOffset = 54;

/**
 * @brief 1900-2100 年农历大小月和闰月编码表。
 * @details 每项低 4 位表示闰月月份；0x10000 表示闰月为 30 天；0x8000 至 0x10
 *          依次表示正月至腊月是否为 30 天。未置位的月份为 29 天。
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
int LunarLeapMonth(int year) {
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
int LunarLeapMonthDays(int year) {
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
int LunarMonthDays(int year, int month) {
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
int LunarYearDays(int year) {
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
bool FormatLunarDate(int year, unsigned month, unsigned day, char* output, size_t output_size) {
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
void SetLabelTextIfChanged(lv_obj_t* label, const char* text) {
    if (label != nullptr && text != nullptr && strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

}  // namespace

/**
 * @brief 注册内置浅色/深色主题及资源主题。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void LcdDisplay::InitializeLcdThemes() {
    auto text_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_TEXT_FONT);
    auto icon_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_ICON_FONT);
    auto large_icon_font = std::make_shared<LvglBuiltInFont>(&font_awesome_30_4);

    // light theme
    auto light_theme = new LvglTheme("light");
    light_theme->set_background_color(lv_color_hex(0xFFFFFF));
    light_theme->set_text_color(lv_color_hex(0x000000));
    light_theme->set_chat_background_color(lv_color_hex(0xE0E0E0));
    light_theme->set_user_bubble_color(lv_color_hex(0x00FF00));
    light_theme->set_assistant_bubble_color(lv_color_hex(0xDDDDDD));
    light_theme->set_system_bubble_color(lv_color_hex(0xFFFFFF));
    light_theme->set_system_text_color(lv_color_hex(0x000000));
    light_theme->set_border_color(lv_color_hex(0x000000));
    light_theme->set_low_battery_color(lv_color_hex(0x000000));
    light_theme->set_text_font(text_font);
    light_theme->set_icon_font(icon_font);
    light_theme->set_large_icon_font(large_icon_font);

    // dark theme
    auto dark_theme = new LvglTheme("dark");
    dark_theme->set_background_color(lv_color_hex(0x000000));
    dark_theme->set_text_color(lv_color_hex(0xFFFFFF));
    dark_theme->set_chat_background_color(lv_color_hex(0x1F1F1F));
    dark_theme->set_user_bubble_color(lv_color_hex(0x00FF00));
    dark_theme->set_assistant_bubble_color(lv_color_hex(0x222222));
    dark_theme->set_system_bubble_color(lv_color_hex(0x000000));
    dark_theme->set_system_text_color(lv_color_hex(0xFFFFFF));
    dark_theme->set_border_color(lv_color_hex(0xFFFFFF));
    dark_theme->set_low_battery_color(lv_color_hex(0xFF0000));
    dark_theme->set_text_font(text_font);
    dark_theme->set_icon_font(icon_font);
    dark_theme->set_large_icon_font(large_icon_font);

    auto& theme_manager = LvglThemeManager::GetInstance();
    theme_manager.RegisterTheme("light", light_theme);
    theme_manager.RegisterTheme("dark", dark_theme);
}

/**
 * @brief 保存面板句柄和逻辑尺寸，供具体总线子类完成 LVGL 注册。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
LcdDisplay::LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width, int height)
    : panel_io_(panel_io), panel_(panel) {
    width_ = width;
    height_ = height;

    // Initialize LCD themes
    InitializeLcdThemes();

    // Load theme from settings
    Settings settings("display", false);
    std::string theme_name = settings.GetString("theme", "light");
    current_theme_ = LvglThemeManager::GetInstance().GetTheme(theme_name);

    // Create a timer to hide the preview image
    esp_timer_create_args_t preview_timer_args = {
        .callback = [](void* arg) {
            LcdDisplay* display = static_cast<LcdDisplay*>(arg);
            display->SetPreviewImage(nullptr);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "preview_timer",
        .skip_unhandled_events = false,
    };
    esp_timer_create(&preview_timer_args, &preview_timer_);
}

/**
 * @brief 构造 SpiLcdDisplay 对象并初始化该模块运行所需的成员和系统资源。
 * @details 构造阶段只建立本模块自身资源；需要异步运行的任务由后续 Start 或 Initialize 方法启动。
 */
SpiLcdDisplay::SpiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                           int width, int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {

    // draw white
    std::vector<uint16_t> buffer(width_, 0xFFFF);
    for (int y = 0; y < height_; y++) {
        esp_lcd_panel_draw_bitmap(panel_, 0, y, width_, y + 1, buffer.data());
    }

    // Set the display to on
    ESP_LOGI(TAG, "正在打开显示屏");
    {
        esp_err_t __err = esp_lcd_panel_disp_on_off(panel_, true);
        if (__err == ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGW(TAG, "屏幕面板不支持开关控制，按已打开处理");
        } else {
            ESP_ERROR_CHECK(__err);
        }
    }

    ESP_LOGI(TAG, "正在初始化 LVGL 图形库");
    lv_init();

#if CONFIG_SPIRAM
    // lv image cache, currently only PNG is supported
    size_t psram_size_mb = esp_psram_get_size() / 1024 / 1024;
    if (psram_size_mb >= 8) {
        lv_image_cache_resize(2 * 1024 * 1024, true);
        ESP_LOGI(TAG, "图像缓存使用 2MB PSRAM");
    } else if (psram_size_mb >= 2) {
        lv_image_cache_resize(512 * 1024, true);
        ESP_LOGI(TAG, "图像缓存使用 512KB PSRAM");
    }
#endif

    ESP_LOGI(TAG, "正在初始化 LVGL 端口");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = kResponsiveLvglTaskPriority;
#if CONFIG_SOC_CPU_CORES_NUM > 1
    port_cfg.task_affinity = 1;
#endif
    const esp_err_t lvgl_init_result = lvgl_port_init(&port_cfg);
    if (lvgl_init_result != ESP_OK) {
        ESP_LOGE(TAG, "LVGL 端口初始化失败，错误码=%s", esp_err_to_name(lvgl_init_result));
        return;
    }

    ESP_LOGI(TAG, "正在添加 LCD 显示设备");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * kSpiLcdDrawBufferLines),
        .double_buffer = false,
        .trans_size = 0,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = false,
        .rotation = {
            .swap_xy = swap_xy,
            .mirror_x = mirror_x,
            .mirror_y = mirror_y,
        },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = 1,
            .buff_spiram = 0,
            .sw_rotate = 0,
            .swap_bytes = 1,
            .full_refresh = 0,
            .direct_mode = 0,
        },
    };

    display_ = lvgl_port_add_disp(&display_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "添加 LCD 显示设备失败");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }
}


// RGB LCD implementation
/**
 * @brief 构造 RgbLcdDisplay 对象并初始化该模块运行所需的成员和系统资源。
 * @details 构造阶段只建立本模块自身资源；需要异步运行的任务由后续 Start 或 Initialize 方法启动。
 */
RgbLcdDisplay::RgbLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                           int width, int height, int offset_x, int offset_y,
                           bool mirror_x, bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {

    // draw white
    std::vector<uint16_t> buffer(width_, 0xFFFF);
    for (int y = 0; y < height_; y++) {
        esp_lcd_panel_draw_bitmap(panel_, 0, y, width_, y + 1, buffer.data());
    }

    ESP_LOGI(TAG, "正在初始化 LVGL 图形库");
    lv_init();

    ESP_LOGI(TAG, "正在初始化 LVGL 端口");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
    port_cfg.timer_period_ms = 50;
    const esp_err_t lvgl_init_result = lvgl_port_init(&port_cfg);
    if (lvgl_init_result != ESP_OK) {
        ESP_LOGE(TAG, "LVGL 端口初始化失败，错误码=%s", esp_err_to_name(lvgl_init_result));
        return;
    }

    ESP_LOGI(TAG, "正在添加 LCD 显示设备");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .buffer_size = static_cast<uint32_t>(width_ * 20),
        .double_buffer = true,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .rotation = {
            .swap_xy = swap_xy,
            .mirror_x = mirror_x,
            .mirror_y = mirror_y,
        },
        .flags = {
            .buff_dma = 1,
            .swap_bytes = 0,
            .full_refresh = 1,
            .direct_mode = 1,
        },
    };

    const lvgl_port_display_rgb_cfg_t rgb_cfg = {
        .flags = {
            .bb_mode = true,
            .avoid_tearing = true,
        }
    };
    
    display_ = lvgl_port_add_disp_rgb(&display_cfg, &rgb_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "添加 RGB 显示设备失败");
        return;
    }
    
    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }
}

/**
 * @brief 构造 MipiLcdDisplay 对象并初始化该模块运行所需的成员和系统资源。
 * @details 构造阶段只建立本模块自身资源；需要异步运行的任务由后续 Start 或 Initialize 方法启动。
 */
MipiLcdDisplay::MipiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                            int width, int height,  int offset_x, int offset_y,
                            bool mirror_x, bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {

    ESP_LOGI(TAG, "正在初始化 LVGL 图形库");
    lv_init();

    ESP_LOGI(TAG, "正在初始化 LVGL 端口");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    const esp_err_t lvgl_init_result = lvgl_port_init(&port_cfg);
    if (lvgl_init_result != ESP_OK) {
        ESP_LOGE(TAG, "LVGL 端口初始化失败，错误码=%s", esp_err_to_name(lvgl_init_result));
        return;
    }

    ESP_LOGI(TAG, "正在添加 LCD 显示设备");
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = panel_io,
        .panel_handle = panel,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * 50),
        .double_buffer = false,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = false,
        /* Rotation values must be same as used in esp_lcd for initial settings of the screen */
        .rotation = {
            .swap_xy = swap_xy,
            .mirror_x = mirror_x,
            .mirror_y = mirror_y,
        },
        .flags = {
            .buff_dma = true,
            .buff_spiram =false,
            .sw_rotate = true,
        },
    };

    const lvgl_port_display_dsi_cfg_t dpi_cfg = {
        .flags = {
            .avoid_tearing = false,
        }
    };
    display_ = lvgl_port_add_disp_dsi(&disp_cfg, &dpi_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "添加 LCD 显示设备失败");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }
}

/**
 * @brief 析构 LcdDisplay 对象并释放其持有的系统资源。
 * @details 释放顺序与创建顺序相反，先停止异步来源，再销毁句柄和动态内存，避免回调访问失效对象。
 */
LcdDisplay::~LcdDisplay() {
    SetPreviewImage(nullptr);
    
    // Clean up GIF controller
    if (gif_controller_) {
        gif_controller_->Stop();
        gif_controller_.reset();
    }
    
    if (preview_timer_ != nullptr) {
        esp_timer_stop(preview_timer_);
        esp_timer_delete(preview_timer_);
    }

    lv_anim_delete(this, ScreensaverMemoScrollAnimationCallback);
    if (screensaver_mcp_list_switch_timer_ != nullptr) {
        lv_timer_delete(screensaver_mcp_list_switch_timer_);
        screensaver_mcp_list_switch_timer_ = nullptr;
    }
    if (screensaver_memo_timer_ != nullptr) {
        lv_timer_delete(screensaver_memo_timer_);
        screensaver_memo_timer_ = nullptr;
    }

    if (screensaver_container_ != nullptr) {
        lv_obj_del(screensaver_container_);
        screensaver_container_ = nullptr;
    }
    if (binding_container_ != nullptr) {
        lv_obj_del(binding_container_);
        binding_container_ = nullptr;
        binding_title_label_ = nullptr;
        binding_code_panel_ = nullptr;
        binding_code_label_ = nullptr;
        binding_message_label_ = nullptr;
    }
    if (preview_image_ != nullptr) {
        lv_obj_del(preview_image_);
    }
    if (chat_message_label_ != nullptr) {
        lv_obj_del(chat_message_label_);
    }
    if (emoji_label_ != nullptr) {
        lv_obj_del(emoji_label_);
    }
    if (emoji_image_ != nullptr) {
        lv_obj_del(emoji_image_);
    }
    if (emoji_box_ != nullptr) {
        lv_obj_del(emoji_box_);
    }
    if (content_ != nullptr) {
        lv_obj_del(content_);
    }
    if (bottom_bar_ != nullptr) {
        lv_obj_del(bottom_bar_);
    }
    if (status_bar_ != nullptr) {
        lv_obj_del(status_bar_);
    }
    if (top_bar_ != nullptr) {
        lv_obj_del(top_bar_);
    }
    if (side_bar_ != nullptr) {
        lv_obj_del(side_bar_);
    }
    if (container_ != nullptr) {
        lv_obj_del(container_);
    }
    if (display_ != nullptr) {
        lv_display_delete(display_);
    }

    if (panel_ != nullptr) {
        esp_lcd_panel_del(panel_);
    }
    if (panel_io_ != nullptr) {
        esp_lcd_panel_io_del(panel_io_);
    }
}

/**
 * @brief 获取 LVGL 全局互斥锁。
 * @param timeout_ms 最大等待毫秒数。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
bool LcdDisplay::Lock(int timeout_ms) {
    return lvgl_port_lock(timeout_ms);
}

/**
 * @brief 释放 LVGL 全局互斥锁。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void LcdDisplay::Unlock() {
    lvgl_port_unlock();
}

/**
 * @brief 根据完整字幕高度配置两行窗口中的纵向滚动动画。
 * @details 方法先终止上一段字幕的动画并恢复到顶部。文本不超过两行时保持居中静止；
 *          超过两行时计算缩放后的溢出距离，以固定可见速度向上滚动。动画到达最后一行后
 *          停留一段时间，再回到第一行重新开始。调用本方法前必须持有 LVGL 锁。
 */
void LcdDisplay::UpdateSubtitleScroll() {
    if (chat_message_label_ == nullptr) {
        return;
    }

    /*
     * 新字幕必须从第一行开始显示。删除以标签为目标的旧动画，同时清除上一段字幕
     * 遗留的 Y 轴平移，避免新回答从中间位置开始。
     */
    lv_anim_delete(chat_message_label_, nullptr);
    lv_obj_set_style_translate_y(chat_message_label_, 0, 0);

    const char* text = lv_label_get_text(chat_message_label_);
    if (text == nullptr || text[0] == '\0') {
        return;
    }

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    const lv_font_t* text_font = lvgl_theme->text_font()->font();
    const int two_line_height = text_font->line_height * 2 + kRoundSubtitleLineSpacing;

    /*
     * 标签高度使用完整文本的自适应高度，而外层 bottom_bar_ 仍保持两行高度并负责裁切。
     * 这样所有文字都保留在标签中，只通过移动标签决定当前可见的两行内容。
     */
    lv_obj_set_height(chat_message_label_, LV_SIZE_CONTENT);
    lv_obj_update_layout(chat_message_label_);
    const int content_height = lv_obj_get_height(chat_message_label_);
    if (content_height <= two_line_height) {
        lv_obj_align(chat_message_label_, LV_ALIGN_CENTER, 0, 0);
        return;
    }

    lv_obj_align(chat_message_label_, LV_ALIGN_TOP_MID, 0, 0);
    const int scroll_distance = ScaleSubtitleSize(content_height - two_line_height);
    const int calculated_duration =
        scroll_distance * 1000 / kRoundSubtitleScrollPixelsPerSecond;
    const int scroll_duration = std::min(30000, std::max(1500, calculated_duration));

    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, chat_message_label_);
    lv_anim_set_exec_cb(&animation, [](void* target, int32_t value) {
        lv_obj_set_style_translate_y(static_cast<lv_obj_t*>(target), value, 0);
    });
    lv_anim_set_values(&animation, 0, -scroll_distance);
    lv_anim_set_duration(&animation, scroll_duration);
    lv_anim_set_delay(&animation, kRoundSubtitleScrollStartDelayMs);
    lv_anim_set_repeat_delay(&animation, kRoundSubtitleScrollRepeatDelayMs);
    lv_anim_set_repeat_count(&animation, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&animation, lv_anim_path_linear);
    lv_anim_start(&animation);
}

/**
 * @brief 将主题中的完整字体应用到表盘所有小号文字标签。
 * @param font 需要使用的 LVGL 字体对象；为空时不执行任何修改。
 * @details 默认资源分区提供 30px 完整中文字库，本方法根据字体实际行高计算缩放比例，
 *          将其视觉高度保持在约 28px，并同步反算标签宽度与行间距，使三行视口的物理
 *          尺寸不随主题字体变化。
 */
void LcdDisplay::ApplyScreensaverTextFont(const lv_font_t* font) {
    if (font == nullptr || font->line_height <= 0) {
        return;
    }

    const int text_scale = kScreensaverSmallTextPixelSize * 256 / font->line_height;
    /*
     * 标签自身会在绘制时缩放，但 LVGL 的自动换行依据缩放前的逻辑宽度计算。这里使用
     * 向下取整，确保缩放后的最终宽度不会因不足 1px 的取整误差越过 228px 裁切视口。
     */
    const int todo_logical_width = kScreensaverMemoWidth * 256 / text_scale;
    const int logical_line_spacing =
        (kScreensaverMemoLineSpacing * 256 + text_scale - 1) / text_scale;
    for (lv_obj_t* label : screensaver_memo_labels_) {
        if (label == nullptr) {
            continue;
        }
        lv_obj_set_style_text_font(label, font, 0);
        lv_obj_set_style_transform_scale(label, text_scale, 0);
        lv_obj_set_style_transform_pivot_x(label, LV_PCT(50), 0);
        lv_obj_set_style_transform_pivot_y(label, 0, 0);
        lv_obj_set_style_text_line_space(label, logical_line_spacing, 0);
    }
    for (lv_obj_t* label : screensaver_memo_date_labels_) {
        if (label == nullptr) {
            continue;
        }
        lv_obj_set_style_text_font(label, font, 0);
        lv_obj_set_style_transform_scale(label, text_scale, 0);
        lv_obj_set_style_transform_pivot_x(label, LV_PCT(50), 0);
        lv_obj_set_style_transform_pivot_y(label, 0, 0);
    }
    if (screensaver_memo_viewport_ != nullptr) {
        lv_obj_set_size(screensaver_memo_viewport_, kScreensaverMemoWidth,
                        kScreensaverMemoViewportHeight);
        lv_obj_align(screensaver_memo_viewport_, LV_ALIGN_CENTER, 0,
                     kScreensaverBottomSectionOffsetY);
    }
    for (int row = 0; row < kScreensaverMemoVisibleLines; ++row) {
        if (screensaver_memo_row_viewports_[row] != nullptr) {
            lv_obj_set_size(screensaver_memo_row_viewports_[row],
                            kScreensaverMemoRowWidths[row], kScreensaverMemoRowHeight);
            lv_obj_align(screensaver_memo_row_viewports_[row], LV_ALIGN_TOP_MID, 0,
                         row * kScreensaverMemoRowHeight);
        }
        if (screensaver_memo_labels_[row] != nullptr) {
            lv_obj_set_width(screensaver_memo_labels_[row], todo_logical_width);
            lv_obj_align(screensaver_memo_labels_[row], LV_ALIGN_TOP_MID, 0,
                         -row * kScreensaverMemoRowHeight);
        }
        if (screensaver_memo_date_labels_[row] != nullptr) {
            lv_obj_set_width(screensaver_memo_date_labels_[row], todo_logical_width);
            lv_obj_align(screensaver_memo_date_labels_[row], LV_ALIGN_TOP_MID, 0,
                         -row * kScreensaverMemoRowHeight);
        }
    }
}

/**
 * @brief 将主题字体应用到自定义 MCP 固定标题和单项内容标签。
 * @param font 当前主题的完整中文字库。
 * @details 标题保持 30px，单项内容保持 25px；两者都按最终物理宽度反算逻辑宽度。
 */
void LcdDisplay::ApplyCustomMcpListFont(const lv_font_t* font) {
    if (font == nullptr || font->line_height <= 0
        || screensaver_mcp_list_title_label_ == nullptr
        || screensaver_mcp_list_label_ == nullptr) {
        return;
    }

    const int title_scale =
        kCustomMcpListTitleTextPixelSize * 256 / font->line_height;
    const int title_logical_width =
        kCustomMcpListTitleWidth * 256 / title_scale;
    lv_obj_set_width(screensaver_mcp_list_title_label_, title_logical_width);
    lv_obj_set_style_text_font(screensaver_mcp_list_title_label_, font, 0);
    lv_obj_set_style_transform_scale(
        screensaver_mcp_list_title_label_, title_scale, 0);
    lv_obj_set_style_transform_pivot_x(
        screensaver_mcp_list_title_label_, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(
        screensaver_mcp_list_title_label_, 0, 0);

    const int text_scale =
        kCustomMcpListTextPixelSize * 256 / font->line_height;
    const int logical_width =
        kCustomMcpListTextWidth * 256 / text_scale;
    const int logical_line_spacing =
        (kCustomMcpListLineSpacing * 256 + text_scale - 1) / text_scale;
    lv_obj_set_width(screensaver_mcp_list_label_, logical_width);
    lv_obj_set_style_text_font(screensaver_mcp_list_label_, font, 0);
    lv_obj_set_style_text_line_space(
        screensaver_mcp_list_label_, logical_line_spacing, 0);
    lv_obj_set_style_transform_scale(screensaver_mcp_list_label_, text_scale, 0);
    lv_obj_set_style_transform_pivot_x(
        screensaver_mcp_list_label_, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(screensaver_mcp_list_label_, 0, 0);
}

/**
 * @brief 在普通表盘内容和自定义 MCP 清单之间切换。
 * @param visible true 显示普通表盘内容；false 只显示外圈、刻度和 MCP 清单。
 */
void LcdDisplay::SetStandardScreensaverContentVisible(bool visible) {
    lv_obj_t* standard_objects[] = {
        screensaver_weather_location_label_,
        screensaver_weather_group_,
        screensaver_date_group_,
        screensaver_time_group_,
        screensaver_memo_viewport_,
        screensaver_network_label_,
        screensaver_battery_label_,
    };
    for (lv_obj_t* object : standard_objects) {
        if (object == nullptr) {
            continue;
        }
        if (visible) {
            lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (screensaver_mcp_list_viewport_ != nullptr) {
        if (visible) {
            lv_obj_add_flag(screensaver_mcp_list_viewport_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(screensaver_mcp_list_viewport_, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/**
 * @brief 将完整主题字体应用到天气位置名称标签。
 * @param font 需要使用的 LVGL 字体对象；为空时不执行任何修改。
 * @details 位置名称固定使用 220px 可见宽度和单行裁切模式，直接使用主题字体的原生字号。
 *          完整省市区名称仍能保持视觉居中。
 */
void LcdDisplay::ApplyScreensaverLocationFont(const lv_font_t* font) {
    if (font == nullptr || font->line_height <= 0 || screensaver_weather_location_label_ == nullptr) {
        return;
    }

    lv_obj_set_width(screensaver_weather_location_label_, kScreensaverLocationWidth);
    lv_obj_set_style_text_font(screensaver_weather_location_label_, font, 0);
    lv_obj_align(screensaver_weather_location_label_, LV_ALIGN_CENTER, 0, -130);
}

/**
 * @brief 将完整主题字体应用到天气三列，并统一计算视觉字号和列间距。
 * @param font 需要使用的 LVGL 字体对象；为空时不执行任何修改。
 * @details 天气三列继续使用主题字体的原生字号；本方法只设置字体和列间距，不改变字形大小。
 */
void LcdDisplay::ApplyScreensaverWeatherFont(const lv_font_t* font) {
    if (font == nullptr || font->line_height <= 0 || screensaver_weather_group_ == nullptr) {
        return;
    }

    lv_obj_t* labels[] = {
        screensaver_weather_temperature_label_,
        screensaver_weather_description_label_,
        screensaver_weather_range_label_,
    };

    for (lv_obj_t* label : labels) {
        if (label != nullptr) {
            lv_obj_set_style_text_font(label, font, 0);
        }
    }

    lv_obj_set_style_pad_column(screensaver_weather_group_, kScreensaverWeatherColumnGap, 0);
    lv_obj_update_layout(screensaver_weather_group_);
    lv_obj_align(screensaver_weather_group_, LV_ALIGN_CENTER, 0, -96);
}

/**
 * @brief 将完整主题字体应用到农历、公历和星期三列。
 * @param font 需要使用的 LVGL 字体对象；为空时不执行任何修改。
 * @details 日期标签直接使用主题字体的原生字号，三个标签使用自动内容宽度和裁切模式，
 *          不会自动换行。
 */
void LcdDisplay::ApplyScreensaverDateFont(const lv_font_t* font) {
    if (font == nullptr || font->line_height <= 0 || screensaver_date_group_ == nullptr) {
        return;
    }

    lv_obj_t* labels[] = {
        screensaver_lunar_date_label_,
        screensaver_solar_date_label_,
        screensaver_weekday_label_,
    };

    for (lv_obj_t* label : labels) {
        if (label != nullptr) {
            lv_obj_set_style_text_font(label, font, 0);
        }
    }

    lv_obj_set_style_pad_column(screensaver_date_group_, kScreensaverDateColumnGap, 0);
    lv_obj_update_layout(screensaver_date_group_);
    lv_obj_align(screensaver_date_group_, LV_ALIGN_CENTER, 0, -56);
}

/**
 * @brief 将 Font Awesome 图标字体应用到表盘 12 点网络和 6 点电量标签。
 * @param font 普通页面状态栏正在使用的图标字体；为空时不执行任何修改。
 * @details 字体变化后重新对齐两个标签，使标签背景继续完整覆盖对应的主刻度。
 */
void LcdDisplay::ApplyScreensaverStatusIconFont(const lv_font_t* font) {
    if (font == nullptr) {
        return;
    }
    if (screensaver_network_label_ != nullptr) {
        lv_obj_set_style_text_font(screensaver_network_label_, font, 0);
    }
    if (screensaver_battery_label_ != nullptr) {
        lv_obj_set_style_text_font(screensaver_battery_label_, font, 0);
    }
    if (screensaver_network_label_ != nullptr) {
        lv_obj_update_layout(screensaver_network_label_);
        lv_obj_align(screensaver_network_label_, LV_ALIGN_TOP_MID, 0,
                     kScreensaverDialStatusInset);
    }
    if (screensaver_battery_label_ != nullptr) {
        lv_obj_update_layout(screensaver_battery_label_);
        lv_obj_align(screensaver_battery_label_, LV_ALIGN_BOTTOM_MID, 0,
                     -kScreensaverDialStatusInset);
    }
}

/**
 * @brief 创建参考运动手表风格的金属黑圆形屏保表盘。
 * @details 表盘使用两个深色圆形对象表现枪灰金属层次，使用 LVGL Scale 绘制 60 个刻度，
 *          不申请全屏 Canvas。农历由设备本地换算，天气由 BackendService 按屏保生命周期
 *          同步；待办区域在备忘录接口接入前显示占位内容。
 *          本方法只负责创建控件，调用者必须已经持有 LVGL 锁。
 */
void LcdDisplay::CreateScreensaverUI() {
    if (screensaver_container_ != nullptr) {
        return;
    }

    auto screen = lv_screen_active();
    const lv_font_t* text_font = &font_puhui_basic_20_4;
    const lv_font_t* icon_font = &BUILTIN_ICON_FONT;
    if (current_theme_ != nullptr) {
        auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
        if (lvgl_theme->text_font() != nullptr && lvgl_theme->text_font()->font() != nullptr) {
            text_font = lvgl_theme->text_font()->font();
        }
        if (lvgl_theme->icon_font() != nullptr && lvgl_theme->icon_font()->font() != nullptr) {
            icon_font = lvgl_theme->icon_font()->font();
        }
    }

    /*
     * 全屏根容器始终保持不透明，确保屏保不受普通主题背景、字幕和表情影响。
     * 物理屏幕为圆形，因此 360x360 方形容器的四角不会出现在可见区域。
     */
    screensaver_container_ = lv_obj_create(screen);
    lv_obj_set_size(screensaver_container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(screensaver_container_, 0, 0);
    lv_obj_set_style_bg_color(screensaver_container_, lv_color_hex(kScreensaverBackgroundColor), 0);
    lv_obj_set_style_bg_opa(screensaver_container_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(screensaver_container_, 0, 0);
    lv_obj_set_style_pad_all(screensaver_container_, 0, 0);
    lv_obj_remove_flag(screensaver_container_, LV_OBJ_FLAG_SCROLLABLE);

    /* 金属圆盘、内层和固定刻度在构建期预渲染，运行时只需复制 RGB565 像素。 */
    screensaver_dial_image_ = lv_image_create(screensaver_container_);
    lv_image_set_src(screensaver_dial_image_, &kScreensaverDialImage);
    lv_obj_center(screensaver_dial_image_);
    lv_obj_remove_flag(screensaver_dial_image_, LV_OBJ_FLAG_SCROLLABLE);

    /*
     * 秒刻度使用独立小对象。移动对象只会使旧、新位置失效，避免通过 Scale Section
     * 每秒重绘 330x330 的完整刻度盘。
     */
    screensaver_second_marker_ = lv_obj_create(screensaver_container_);
    lv_obj_set_size(screensaver_second_marker_, 3, 9);
    lv_obj_set_style_radius(screensaver_second_marker_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(
        screensaver_second_marker_, lv_color_hex(kScreensaverAccentColor), 0);
    lv_obj_set_style_bg_opa(screensaver_second_marker_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(screensaver_second_marker_, 0, 0);
    lv_obj_set_style_pad_all(screensaver_second_marker_, 0, 0);
    lv_obj_remove_flag(screensaver_second_marker_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(screensaver_second_marker_, LV_OBJ_FLAG_HIDDEN);

    /* 自定义 MCP 清单根容器只负责组合固定标题和单项轮播区域。 */
    screensaver_mcp_list_viewport_ = lv_obj_create(screensaver_container_);
    lv_obj_set_size(screensaver_mcp_list_viewport_, kCustomMcpListViewportWidth,
                    kCustomMcpListViewportHeight);
    lv_obj_set_style_bg_opa(screensaver_mcp_list_viewport_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(screensaver_mcp_list_viewport_, 0, 0);
    lv_obj_set_style_border_width(screensaver_mcp_list_viewport_, 0, 0);
    lv_obj_set_style_pad_all(screensaver_mcp_list_viewport_, 0, 0);
    lv_obj_set_scrollbar_mode(screensaver_mcp_list_viewport_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(screensaver_mcp_list_viewport_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(screensaver_mcp_list_viewport_, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_center(screensaver_mcp_list_viewport_);

    screensaver_mcp_list_title_label_ = lv_label_create(screensaver_mcp_list_viewport_);
    lv_obj_set_width(screensaver_mcp_list_title_label_, kCustomMcpListTitleWidth);
    lv_obj_set_height(screensaver_mcp_list_title_label_, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(screensaver_mcp_list_title_label_, text_font, 0);
    lv_obj_set_style_text_color(
        screensaver_mcp_list_title_label_, lv_color_white(), 0);
    lv_obj_set_style_text_align(
        screensaver_mcp_list_title_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(screensaver_mcp_list_title_label_, LV_LABEL_LONG_CLIP);
    lv_label_set_text(screensaver_mcp_list_title_label_, "自定义 MCP");
    lv_obj_align(
        screensaver_mcp_list_title_label_, LV_ALIGN_TOP_MID, 0,
        kCustomMcpListTitleTopOffset);

    screensaver_mcp_list_item_viewport_ = lv_obj_create(screensaver_mcp_list_viewport_);
    lv_obj_set_size(
        screensaver_mcp_list_item_viewport_,
        kCustomMcpListItemViewportWidth,
        kCustomMcpListItemViewportHeight);
    lv_obj_set_style_bg_opa(screensaver_mcp_list_item_viewport_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(
        screensaver_mcp_list_item_viewport_, kCustomMcpListViewportRadius, 0);
    lv_obj_set_style_border_width(screensaver_mcp_list_item_viewport_, 0, 0);
    lv_obj_set_style_pad_all(screensaver_mcp_list_item_viewport_, 0, 0);
    lv_obj_set_scrollbar_mode(
        screensaver_mcp_list_item_viewport_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(
        screensaver_mcp_list_item_viewport_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(
        screensaver_mcp_list_item_viewport_, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_align(
        screensaver_mcp_list_item_viewport_, LV_ALIGN_CENTER, 0,
        kCustomMcpListItemViewportOffsetY);

    screensaver_mcp_list_label_ = lv_label_create(screensaver_mcp_list_item_viewport_);
    lv_obj_set_width(screensaver_mcp_list_label_, kCustomMcpListTextWidth);
    lv_obj_set_height(screensaver_mcp_list_label_, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(screensaver_mcp_list_label_, text_font, 0);
    lv_obj_set_style_text_color(
        screensaver_mcp_list_label_, lv_color_hex(0xE8ECEF), 0);
    lv_obj_set_style_text_align(
        screensaver_mcp_list_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(
        screensaver_mcp_list_label_, kCustomMcpListLineSpacing, 0);
    lv_label_set_long_mode(screensaver_mcp_list_label_, LV_LABEL_LONG_WRAP);
    lv_label_set_text(screensaver_mcp_list_label_, "暂无自定义 MCP");
    lv_obj_center(screensaver_mcp_list_label_);

    screensaver_mcp_list_switch_timer_ = lv_timer_create(
        CustomMcpListSwitchTimerCallback,
        kCustomMcpListSwitchPeriodMs,
        this);
    lv_timer_pause(screensaver_mcp_list_switch_timer_);
    lv_obj_add_flag(screensaver_mcp_list_viewport_, LV_OBJ_FLAG_HIDDEN);

    /* 当前天气位置独占一行并位于天气三列上方，固定宽度保证文本始终以圆屏中心对齐。 */
    screensaver_weather_location_label_ = lv_label_create(screensaver_container_);
    lv_obj_set_width(screensaver_weather_location_label_, kScreensaverLocationWidth);
    lv_obj_set_style_text_font(screensaver_weather_location_label_, text_font, 0);
    lv_obj_set_style_text_color(screensaver_weather_location_label_,
                                lv_color_hex(kScreensaverSecondaryTextColor), 0);
    lv_obj_set_style_text_opa(screensaver_weather_location_label_, LV_OPA_80, 0);
    lv_obj_set_style_text_align(screensaver_weather_location_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(screensaver_weather_location_label_, LV_LABEL_LONG_CLIP);
    lv_label_set_text(screensaver_weather_location_label_, "天气位置待同步");
    lv_obj_align(screensaver_weather_location_label_, LV_ALIGN_CENTER, 0, -126);

    /*
     * 天气信息占据顶部同一行，并按照当前温度、天气描述、最低/最高温三列排列。
     * 三列放在自动宽度的 Flex 容器中，保持 12px 可见间距并作为一个整体水平居中。
     */
    screensaver_weather_group_ = lv_obj_create(screensaver_container_);
    lv_obj_set_size(screensaver_weather_group_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(screensaver_weather_group_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(screensaver_weather_group_, 0, 0);
    lv_obj_set_style_pad_all(screensaver_weather_group_, 0, 0);
    lv_obj_set_flex_flow(screensaver_weather_group_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(screensaver_weather_group_, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(screensaver_weather_group_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(screensaver_weather_group_, LV_ALIGN_CENTER, 0, -96);

    screensaver_weather_temperature_label_ = lv_label_create(screensaver_weather_group_);
    lv_obj_set_size(screensaver_weather_temperature_label_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(screensaver_weather_temperature_label_, text_font, 0);
    lv_obj_set_style_text_color(screensaver_weather_temperature_label_, lv_color_white(), 0);
    lv_obj_set_style_text_align(screensaver_weather_temperature_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(screensaver_weather_temperature_label_, "--℃");

    screensaver_weather_description_label_ = lv_label_create(screensaver_weather_group_);
    lv_obj_set_size(screensaver_weather_description_label_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(screensaver_weather_description_label_, text_font, 0);
    lv_obj_set_style_text_color(screensaver_weather_description_label_,
                                lv_color_hex(kScreensaverSecondaryTextColor), 0);
    lv_obj_set_style_text_align(screensaver_weather_description_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(screensaver_weather_description_label_, "待同步");

    screensaver_weather_range_label_ = lv_label_create(screensaver_weather_group_);
    lv_obj_set_size(screensaver_weather_range_label_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(screensaver_weather_range_label_, text_font, 0);
    lv_obj_set_style_text_color(screensaver_weather_range_label_, lv_color_white(), 0);
    lv_obj_set_style_text_align(screensaver_weather_range_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(screensaver_weather_range_label_, "--/--");

    /*
     * 日期三列使用一个自动宽度的横向组，最终可见间距为 16px，并由整个组统一居中。
     * 标签采用自动内容宽度和裁切模式，确保农历、公历、星期始终保持在同一行。
     */
    screensaver_date_group_ = lv_obj_create(screensaver_container_);
    lv_obj_set_size(screensaver_date_group_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(screensaver_date_group_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(screensaver_date_group_, 0, 0);
    lv_obj_set_style_pad_all(screensaver_date_group_, 0, 0);
    lv_obj_set_flex_flow(screensaver_date_group_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(screensaver_date_group_, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(screensaver_date_group_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(screensaver_date_group_, LV_ALIGN_CENTER, 0, -56);

    screensaver_lunar_date_label_ = lv_label_create(screensaver_date_group_);
    lv_obj_set_size(screensaver_lunar_date_label_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(screensaver_lunar_date_label_, text_font, 0);
    lv_obj_set_style_text_color(screensaver_lunar_date_label_, lv_color_white(), 0);
    lv_obj_set_style_text_opa(screensaver_lunar_date_label_, LV_OPA_80, 0);
    lv_obj_set_style_text_align(screensaver_lunar_date_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(screensaver_lunar_date_label_, LV_LABEL_LONG_CLIP);
    lv_label_set_text(screensaver_lunar_date_label_, "农历待同步");

    screensaver_solar_date_label_ = lv_label_create(screensaver_date_group_);
    lv_obj_set_size(screensaver_solar_date_label_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(screensaver_solar_date_label_, text_font, 0);
    lv_obj_set_style_text_color(screensaver_solar_date_label_, lv_color_white(), 0);
    lv_obj_set_style_text_opa(screensaver_solar_date_label_, LV_OPA_80, 0);
    lv_obj_set_style_text_align(screensaver_solar_date_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(screensaver_solar_date_label_, LV_LABEL_LONG_CLIP);
    lv_label_set_text(screensaver_solar_date_label_, "-- --");

    screensaver_weekday_label_ = lv_label_create(screensaver_date_group_);
    lv_obj_set_size(screensaver_weekday_label_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(screensaver_weekday_label_, text_font, 0);
    lv_obj_set_style_text_color(screensaver_weekday_label_, lv_color_white(), 0);
    lv_obj_set_style_text_opa(screensaver_weekday_label_, LV_OPA_80, 0);
    lv_obj_set_style_text_align(screensaver_weekday_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(screensaver_weekday_label_, LV_LABEL_LONG_CLIP);
    lv_label_set_text(screensaver_weekday_label_, "星期--");

    /*
     * 时分和秒放入同一个自动宽度的横向容器。容器自身负责整体居中，并在两个标签之间
     * 保留约 10px 的最终可见间距。整个容器统一放大，避免分别缩放和定位产生偏移。
     */
    screensaver_time_group_ = lv_obj_create(screensaver_container_);
    lv_obj_set_size(screensaver_time_group_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(screensaver_time_group_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(screensaver_time_group_, 0, 0);
    lv_obj_set_style_pad_all(screensaver_time_group_, 0, 0);
    lv_obj_set_style_pad_column(screensaver_time_group_, kScreensaverTimeColumnGap, 0);
    lv_obj_set_flex_flow(screensaver_time_group_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(screensaver_time_group_, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(screensaver_time_group_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(screensaver_time_group_, LV_ALIGN_CENTER, 0, 4);

    screensaver_time_label_ = lv_label_create(screensaver_time_group_);
    lv_obj_set_size(screensaver_time_label_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(screensaver_time_label_, &font_puhui_time_64, 0);
    lv_obj_set_style_text_color(screensaver_time_label_, lv_color_white(), 0);
    lv_obj_set_style_text_align(screensaver_time_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(screensaver_time_label_, "--:--");

    screensaver_seconds_label_ = lv_label_create(screensaver_time_group_);
    lv_obj_set_size(screensaver_seconds_label_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(screensaver_seconds_label_, &font_puhui_time_64, 0);
    lv_obj_set_style_text_color(screensaver_seconds_label_, lv_color_hex(kScreensaverAccentColor), 0);
    lv_obj_set_style_text_align(screensaver_seconds_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(screensaver_seconds_label_, "--");

    /*
     * 时间下方使用固定三行高度的透明根视口。根视口内部再建立三条由宽到窄的水平裁切带，
     * 用分段圆弦逼近圆屏下半部分的左右弧形，避免备忘录覆盖外圈刻度。每条裁切带持有一份
     * 相同文本，三个标签同步纵向移动后，视觉上仍是一段连续滚动的完整备忘录。
     */
    screensaver_memo_viewport_ = lv_obj_create(screensaver_container_);
    lv_obj_set_size(screensaver_memo_viewport_, kScreensaverMemoWidth,
                    kScreensaverMemoViewportHeight);
    lv_obj_set_style_bg_opa(screensaver_memo_viewport_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(screensaver_memo_viewport_, 0, 0);
    lv_obj_set_style_pad_all(screensaver_memo_viewport_, 0, 0);
    lv_obj_set_scrollbar_mode(screensaver_memo_viewport_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(screensaver_memo_viewport_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(screensaver_memo_viewport_, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_align(screensaver_memo_viewport_, LV_ALIGN_CENTER, 0,
                 kScreensaverBottomSectionOffsetY);

    for (int row = 0; row < kScreensaverMemoVisibleLines; ++row) {
        lv_obj_t* row_viewport = lv_obj_create(screensaver_memo_viewport_);
        screensaver_memo_row_viewports_[row] = row_viewport;
        lv_obj_set_size(row_viewport, kScreensaverMemoRowWidths[row],
                        kScreensaverMemoRowHeight);
        lv_obj_set_style_bg_opa(row_viewport, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row_viewport, 0, 0);
        lv_obj_set_style_pad_all(row_viewport, 0, 0);
        lv_obj_set_scrollbar_mode(row_viewport, LV_SCROLLBAR_MODE_OFF);
        lv_obj_remove_flag(row_viewport, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(row_viewport, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
        lv_obj_align(row_viewport, LV_ALIGN_TOP_MID, 0,
                     row * kScreensaverMemoRowHeight);

        lv_obj_t* memo_label = lv_label_create(row_viewport);
        screensaver_memo_labels_[row] = memo_label;
        lv_obj_set_width(memo_label, kScreensaverMemoWidth);
        lv_obj_set_style_text_font(memo_label, text_font, 0);
        lv_obj_set_style_text_color(memo_label, lv_color_hex(0xDDE1E4), 0);
        lv_obj_set_style_text_align(memo_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_line_space(memo_label, kScreensaverMemoLineSpacing, 0);
        lv_label_set_long_mode(memo_label, LV_LABEL_LONG_WRAP);
        lv_label_set_text(memo_label, "暂无待办");
        lv_obj_align(memo_label, LV_ALIGN_TOP_MID, 0,
                     -row * kScreensaverMemoRowHeight);

        lv_obj_t* date_label = lv_label_create(row_viewport);
        screensaver_memo_date_labels_[row] = date_label;
        lv_obj_set_width(date_label, kScreensaverMemoWidth);
        lv_obj_set_height(date_label, LV_SIZE_CONTENT);
        lv_obj_set_style_text_font(date_label, text_font, 0);
        lv_obj_set_style_text_color(date_label, lv_color_hex(0xDDE1E4), 0);
        lv_obj_set_style_text_align(date_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(date_label, LV_LABEL_LONG_CLIP);
        lv_label_set_text(date_label, "");
        lv_obj_align(date_label, LV_ALIGN_TOP_MID, 0,
                     -row * kScreensaverMemoRowHeight);
    }
    screensaver_memo_timer_ = lv_timer_create(
        ScreensaverMemoTimerCallback, 8000, this);
    lv_timer_pause(screensaver_memo_timer_);

    /*
     * Wi-Fi 图标放在已经隐藏主刻度的 12 点位置。网络连接、弱信号和断网状态继续使用
     * Board 返回的不同 Font Awesome 图标表达，透明背景保持金属外圈连续可见。
     */
    screensaver_network_label_ = lv_label_create(screensaver_container_);
    lv_obj_set_size(screensaver_network_label_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(screensaver_network_label_, icon_font, 0);
    lv_obj_set_style_text_color(screensaver_network_label_, lv_color_hex(0xDDE1E4), 0);
    lv_obj_set_style_text_align(screensaver_network_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_opa(screensaver_network_label_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(screensaver_network_label_, 0, 0);
    lv_label_set_text(screensaver_network_label_, FONT_AWESOME_WIFI_SLASH);
    lv_obj_align(screensaver_network_label_, LV_ALIGN_TOP_MID, 0,
                 kScreensaverDialStatusInset);

    /*
     * 电量图标位于已经隐藏主刻度的 6 点位置，使用空、四分之一、半格、四分之三、满格和
     * 充电闪电图标直接表达当前电池状态，不再占用时间下方的备忘录空间。
     */
    screensaver_battery_label_ = lv_label_create(screensaver_container_);
    lv_obj_set_size(screensaver_battery_label_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(screensaver_battery_label_, icon_font, 0);
    lv_obj_set_style_text_color(screensaver_battery_label_, lv_color_hex(0xDDE1E4), 0);
    lv_obj_set_style_text_align(screensaver_battery_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_opa(screensaver_battery_label_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(screensaver_battery_label_, 0, 0);
    lv_label_set_text(screensaver_battery_label_, FONT_AWESOME_BATTERY_EMPTY);
    lv_obj_align(screensaver_battery_label_, LV_ALIGN_BOTTOM_MID, 0,
                 -kScreensaverDialStatusInset);

    /* 使用当前主题字体，并把不同源字号换算为各信息层级对应的表盘视觉字号。 */
    ApplyScreensaverTextFont(text_font);
    ApplyCustomMcpListFont(text_font);
    ApplyScreensaverLocationFont(text_font);
    ApplyScreensaverWeatherFont(text_font);
    ApplyScreensaverDateFont(text_font);
    ApplyScreensaverStatusIconFont(icon_font);
    UpdateScreensaverMemo();

    lv_obj_add_flag(screensaver_container_, LV_OBJ_FLAG_HIDDEN);
}

/**
 * @brief 将当前主题字体应用到设备绑定页面，并保持固定的最终视觉尺寸。
 * @param font 主题中文字库；为空或行高无效时不改变页面。
 */
void LcdDisplay::ApplyDeviceBindingFont(const lv_font_t* font) {
    if (font == nullptr || font->line_height <= 0) {
        return;
    }

    const int title_scale = kBindingTitleTextPixelSize * 256 / font->line_height;
    const int message_scale = kBindingMessageTextPixelSize * 256 / font->line_height;

    if (binding_title_label_ != nullptr) {
        lv_obj_set_style_text_font(binding_title_label_, font, 0);
        lv_obj_set_width(binding_title_label_,
                         kBindingMessageWidth * 256 / title_scale);
        lv_obj_set_style_transform_scale(binding_title_label_, title_scale, 0);
        lv_obj_set_style_transform_pivot_x(binding_title_label_, LV_PCT(50), 0);
        lv_obj_set_style_transform_pivot_y(binding_title_label_, LV_PCT(50), 0);
        lv_obj_align(binding_title_label_, LV_ALIGN_TOP_MID, 0,
                     kBindingTitleOffsetY);
    }
    if (binding_code_label_ != nullptr) {
        lv_obj_set_style_text_font(binding_code_label_, &font_puhui_time_64, 0);
        lv_obj_set_style_transform_scale(binding_code_label_, 256, 0);
        lv_obj_center(binding_code_label_);
    }
    if (binding_message_label_ != nullptr) {
        lv_obj_set_style_text_font(binding_message_label_, font, 0);
        lv_obj_set_width(binding_message_label_,
                         kBindingMessageWidth * 256 / message_scale);
        lv_obj_set_style_transform_scale(binding_message_label_, message_scale, 0);
        lv_obj_set_style_transform_pivot_x(binding_message_label_, LV_PCT(50), 0);
        lv_obj_set_style_transform_pivot_y(binding_message_label_, LV_PCT(50), 0);
        if (binding_code_panel_ != nullptr
            && !lv_obj_has_flag(binding_code_panel_, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_align(binding_message_label_, LV_ALIGN_BOTTOM_MID, 0,
                         -kBindingMessageBottomOffset);
        } else {
            lv_obj_align(binding_message_label_, LV_ALIGN_CENTER, 0, 16);
        }
    }
}

/**
 * @brief 创建金属黑风格的圆屏设备绑定码页面。
 * @details 绑定页是屏幕根对象的独立子对象，并在创建后位于屏保对象之后。显示时再次移动到
 *          最前方，可确保语音会话结束触发屏保后，绑定码仍持续可见。
 */
void LcdDisplay::CreateDeviceBindingUI() {
    if (binding_container_ != nullptr) {
        return;
    }

    auto screen = lv_screen_active();
    const lv_font_t* text_font = &font_puhui_basic_20_4;
    if (current_theme_ != nullptr) {
        auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
        if (lvgl_theme->text_font() != nullptr
            && lvgl_theme->text_font()->font() != nullptr) {
            text_font = lvgl_theme->text_font()->font();
        }
    }

    binding_container_ = lv_obj_create(screen);
    lv_obj_set_size(binding_container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(binding_container_, 0, 0);
    lv_obj_set_style_bg_color(binding_container_,
                              lv_color_hex(kScreensaverBackgroundColor), 0);
    lv_obj_set_style_bg_opa(binding_container_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(binding_container_, 0, 0);
    lv_obj_set_style_pad_all(binding_container_, 0, 0);
    lv_obj_remove_flag(binding_container_, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* outer_metal = lv_obj_create(binding_container_);
    lv_obj_set_size(outer_metal, kScreensaverDialSize, kScreensaverDialSize);
    lv_obj_set_style_radius(outer_metal, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(outer_metal,
                              lv_color_hex(kScreensaverOuterMetalColor), 0);
    lv_obj_set_style_bg_opa(outer_metal, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(outer_metal,
                                  lv_color_hex(kScreensaverMetalBorderColor), 0);
    lv_obj_set_style_border_width(outer_metal, 2, 0);
    lv_obj_set_style_pad_all(outer_metal, 0, 0);
    lv_obj_remove_flag(outer_metal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(outer_metal);

    lv_obj_t* inner_metal = lv_obj_create(outer_metal);
    lv_obj_set_size(inner_metal, kScreensaverScaleSize, kScreensaverScaleSize);
    lv_obj_set_style_radius(inner_metal, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(inner_metal,
                              lv_color_hex(kScreensaverInnerMetalColor), 0);
    lv_obj_set_style_bg_opa(inner_metal, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(inner_metal, 0, 0);
    lv_obj_set_style_pad_all(inner_metal, 0, 0);
    lv_obj_remove_flag(inner_metal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(inner_metal);

    binding_title_label_ = lv_label_create(binding_container_);
    lv_label_set_text(binding_title_label_, "设备绑定");
    lv_obj_set_style_text_color(binding_title_label_, lv_color_hex(0xDDE1E4), 0);
    lv_obj_set_style_text_align(binding_title_label_, LV_TEXT_ALIGN_CENTER, 0);

    binding_code_panel_ = lv_obj_create(binding_container_);
    lv_obj_set_size(binding_code_panel_, kBindingCodePanelWidth,
                    kBindingCodePanelHeight);
    lv_obj_set_style_radius(binding_code_panel_, 8, 0);
    lv_obj_set_style_bg_color(binding_code_panel_,
                              lv_color_hex(kScreensaverBackgroundColor), 0);
    lv_obj_set_style_bg_opa(binding_code_panel_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(binding_code_panel_,
                                  lv_color_hex(kScreensaverMetalBorderColor), 0);
    lv_obj_set_style_border_width(binding_code_panel_, 2, 0);
    lv_obj_set_style_pad_all(binding_code_panel_, 0, 0);
    lv_obj_remove_flag(binding_code_panel_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(binding_code_panel_, LV_ALIGN_CENTER, 0, kBindingCodeOffsetY);

    binding_code_label_ = lv_label_create(binding_code_panel_);
    lv_label_set_text(binding_code_label_, "------");
    lv_obj_set_size(binding_code_label_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_text_color(binding_code_label_,
                                lv_color_hex(kScreensaverAccentColor), 0);
    lv_obj_set_style_text_align(binding_code_label_, LV_TEXT_ALIGN_CENTER, 0);

    binding_message_label_ = lv_label_create(binding_container_);
    lv_label_set_text(binding_message_label_, "正在获取绑定码...");
    lv_label_set_long_mode(binding_message_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(binding_message_label_,
                                lv_color_hex(kScreensaverSecondaryTextColor), 0);
    lv_obj_set_style_text_align(binding_message_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(binding_message_label_, 5, 0);

    ApplyDeviceBindingFont(text_font);
    lv_obj_add_flag(binding_container_, LV_OBJ_FLAG_HIDDEN);
}

/**
 * @brief 显示设备绑定页，并根据绑定码是否存在切换页面布局。
 * @param binding_code 用户输入网页端的短绑定码；空字符串表示加载、成功或失败状态。
 * @param message 页面说明文字。
 */
void LcdDisplay::ShowDeviceBinding(const std::string& binding_code,
                                   const std::string& message) {
    DisplayLockGuard lock(this);
    if (binding_container_ == nullptr || binding_code_panel_ == nullptr
        || binding_code_label_ == nullptr || binding_message_label_ == nullptr) {
        ESP_LOGW(TAG, "设备绑定界面尚未初始化");
        return;
    }

    binding_active_ = true;
    SetLabelTextIfChanged(binding_code_label_, binding_code.c_str());
    SetLabelTextIfChanged(binding_message_label_, message.c_str());
    if (binding_code.empty()) {
        lv_obj_add_flag(binding_code_panel_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(binding_message_label_, LV_ALIGN_CENTER, 0, 16);
    } else {
        lv_obj_remove_flag(binding_code_panel_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(binding_message_label_, LV_ALIGN_BOTTOM_MID, 0,
                     -kBindingMessageBottomOffset);
    }
    lv_obj_remove_flag(binding_container_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(binding_container_);
    lv_obj_invalidate(binding_container_);
}

/**
 * @brief 隐藏设备绑定页，保留页面下方原有界面的状态。
 */
void LcdDisplay::HideDeviceBinding() {
    DisplayLockGuard lock(this);
    binding_active_ = false;
    if (binding_container_ != nullptr) {
        lv_obj_add_flag(binding_container_, LV_OBJ_FLAG_HIDDEN);
    }
}

/**
 * @brief 刷新屏保表盘中的时间、农历、公历、星期、当前秒刻度、网络和电量。
 * @details 系统时间尚未由网络校准时显示占位文本。农历由本地年份表换算，天气和待办内容
 *          由 BackendService 独立更新。本方法仅在标签内容变化时重绘，降低每秒刷新负担。
 */
void LcdDisplay::UpdateScreensaverContent() {
    if (!screensaver_active_ || screensaver_container_ == nullptr
        || custom_mcp_list_active_.load()) {
        return;
    }

    char time_text[8] = "--:--";
    char seconds_text[4] = "--";
    char lunar_date_text[32] = "农历--";
    /*
     * tm_mon 和 tm_mday 的类型都是 int。虽然正常值分别只有 1-12 和 1-31，
     * 编译器进行格式截断检查时仍会按照两个完整 int 的最坏长度计算，因此预留
     * 足以容纳两个 32 位十进制整数、分隔符和字符串结束符的缓冲区。
     */
    char solar_date_text[32] = "-- --";
    char weekday_text[16] = "星期--";
    int current_second = 0;

    const time_t now = time(nullptr);
    struct tm local_time = {};
    if (localtime_r(&now, &local_time) != nullptr && local_time.tm_year >= 2025 - 1900) {
        static const char* weekdays[] = {
            "星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"
        };
        snprintf(time_text, sizeof(time_text), "%02d:%02d", local_time.tm_hour, local_time.tm_min);
        snprintf(seconds_text, sizeof(seconds_text), "%02d", local_time.tm_sec);
        snprintf(solar_date_text, sizeof(solar_date_text), "%d-%d",
                 local_time.tm_mon + 1, local_time.tm_mday);
        snprintf(weekday_text, sizeof(weekday_text), "%s", weekdays[local_time.tm_wday]);
        FormatLunarDate(local_time.tm_year + 1900,
                        static_cast<unsigned>(local_time.tm_mon + 1),
                        static_cast<unsigned>(local_time.tm_mday),
                        lunar_date_text, sizeof(lunar_date_text));
        current_second = local_time.tm_sec;
    }

    SetLabelTextIfChanged(screensaver_time_label_, time_text);
    SetLabelTextIfChanged(screensaver_seconds_label_, seconds_text);
    const int date_key = local_time.tm_year >= 2025 - 1900
        ? (local_time.tm_year + 1900) * 10000
            + (local_time.tm_mon + 1) * 100
            + local_time.tm_mday
        : 0;
    if (date_key != screensaver_last_date_key_) {
        SetLabelTextIfChanged(screensaver_lunar_date_label_, lunar_date_text);
        SetLabelTextIfChanged(screensaver_solar_date_label_, solar_date_text);
        SetLabelTextIfChanged(screensaver_weekday_label_, weekday_text);
        screensaver_last_date_key_ = date_key;
    }

    if (screensaver_second_marker_ != nullptr) {
        const auto& marker_layout = GetSecondMarkerLayouts()[current_second];
        const bool marker_visible =
            local_time.tm_year >= 2025 - 1900 && marker_layout.visible;
        const int rendered_second = local_time.tm_year >= 2025 - 1900
            ? current_second : -1;
        if (rendered_second != screensaver_last_rendered_second_) {
            if (!marker_visible) {
                lv_obj_add_flag(screensaver_second_marker_, LV_OBJ_FLAG_HIDDEN);
            } else {
                const int marker_style = marker_layout.major ? 1 : 0;
                if (screensaver_second_marker_style_ != marker_style) {
                    lv_obj_set_size(
                        screensaver_second_marker_, marker_layout.width, marker_layout.height);
                    lv_obj_set_style_transform_pivot_x(
                        screensaver_second_marker_, marker_layout.width / 2, 0);
                    lv_obj_set_style_transform_pivot_y(
                        screensaver_second_marker_, marker_layout.height / 2, 0);
                    screensaver_second_marker_style_ = marker_style;
                }
                lv_obj_set_style_transform_rotation(
                    screensaver_second_marker_, marker_layout.rotation, 0);
                lv_obj_set_pos(
                    screensaver_second_marker_, marker_layout.x, marker_layout.y);
                lv_obj_remove_flag(screensaver_second_marker_, LV_OBJ_FLAG_HIDDEN);
            }
            screensaver_last_rendered_second_ = rendered_second;
        }
    }

    int battery_level = 0;
    bool charging = false;
    bool discharging = false;
    const char* battery_icon = FONT_AWESOME_BATTERY_EMPTY;
    const bool battery_available =
        Board::GetInstance().GetBatteryLevel(battery_level, charging, discharging);
    if (battery_available) {
        (void)discharging;
        if (charging) {
            battery_icon = FONT_AWESOME_BATTERY_BOLT;
        } else if (battery_level >= 80) {
            battery_icon = FONT_AWESOME_BATTERY_FULL;
        } else if (battery_level >= 60) {
            battery_icon = FONT_AWESOME_BATTERY_THREE_QUARTERS;
        } else if (battery_level >= 40) {
            battery_icon = FONT_AWESOME_BATTERY_HALF;
        } else if (battery_level >= 20) {
            battery_icon = FONT_AWESOME_BATTERY_QUARTER;
        }
    }
    SetLabelTextIfChanged(screensaver_battery_label_, battery_icon);
    if (screensaver_battery_label_ != nullptr) {
        const bool battery_needs_attention =
            battery_available && (charging || battery_level < 20);
        if (!screensaver_battery_attention_valid_
            || screensaver_battery_attention_ != battery_needs_attention) {
            lv_obj_set_style_text_color(
                screensaver_battery_label_,
                lv_color_hex(battery_needs_attention ? kScreensaverAccentColor : 0xDDE1E4), 0);
            screensaver_battery_attention_ = battery_needs_attention;
            screensaver_battery_attention_valid_ = true;
        }
    }

    /*
     * Board 返回的网络图标与普通页面状态栏使用相同来源，已经综合当前连接类型、
     * 连接状态和信号强度。返回空指针时使用断网图标作为稳定回退。
     */
    const char* network_icon = Board::GetInstance().GetNetworkStateIcon();
    const char* visible_network_icon =
        network_icon != nullptr ? network_icon : FONT_AWESOME_WIFI_SLASH;
    SetLabelTextIfChanged(screensaver_network_label_, visible_network_icon);
    if (screensaver_network_label_ != nullptr) {
        const bool network_disconnected =
            strcmp(visible_network_icon, FONT_AWESOME_WIFI_SLASH) == 0;
        if (!screensaver_network_disconnected_valid_
            || screensaver_network_disconnected_ != network_disconnected) {
            lv_obj_set_style_text_color(
                screensaver_network_label_,
                lv_color_hex(network_disconnected ? kScreensaverAccentColor : 0xDDE1E4), 0);
            screensaver_network_disconnected_ = network_disconnected;
            screensaver_network_disconnected_valid_ = true;
        }
    }
}

/**
 * @brief 刷新普通状态栏以及当前可见的屏保表盘。
 * @param update_all true 强制刷新网络等全部状态；false 使用普通周期策略。
 */
void LcdDisplay::UpdateStatusBar(bool update_all) {
    LvglDisplay::UpdateStatusBar(update_all);

    DisplayLockGuard lock(this);
    UpdateScreensaverContent();
}

/**
 * @brief 显示或隐藏金属黑表盘屏保。
 * @param enabled true 显示表盘；false 恢复进入屏保前的正常界面。
 * @details 进入屏保时停止 GIF 和字幕滚动以降低后台刷新；退出时恢复仍然有效的表情动画和字幕滚动。
 */
void LcdDisplay::SetScreensaverMode(bool enabled) {
    DisplayLockGuard lock(this);
    if (screensaver_container_ == nullptr || screensaver_active_ == enabled) {
        return;
    }

    screensaver_active_ = enabled;
    if (enabled) {
        screensaver_last_rendered_second_ = -2;
        SetStandardScreensaverContentVisible(true);
        if (gif_controller_) {
            gif_controller_->Stop();
        }
        if (chat_message_label_ != nullptr) {
            lv_anim_delete(chat_message_label_, nullptr);
            lv_obj_set_style_translate_y(chat_message_label_, 0, 0);
        }
        UpdateScreensaverContent();
        /*
         * 结构化备忘录在数据写入时已经完成文本和三行布局，屏保隐藏不会销毁这些状态。
         * 只有没有日期行的兼容长文本需要在动画被停止后恢复滚动布局。
         */
        if (screensaver_memo_labels_[0] != nullptr) {
            const char* memo_text = lv_label_get_text(screensaver_memo_labels_[0]);
            if (memo_text != nullptr && std::strchr(memo_text, '\n') == nullptr) {
                UpdateScreensaverMemoScroll();
            }
        }
        if (screensaver_memo_timer_ != nullptr && screensaver_memos_.size() > 1) {
            lv_timer_resume(screensaver_memo_timer_);
            lv_timer_reset(screensaver_memo_timer_);
        }
        lv_obj_remove_flag(screensaver_container_, LV_OBJ_FLAG_HIDDEN);
        if (binding_active_ && binding_container_ != nullptr) {
            lv_obj_move_foreground(binding_container_);
        }
    } else {
        custom_mcp_list_active_.store(false);
        SetStandardScreensaverContentVisible(true);
        if (screensaver_mcp_list_switch_timer_ != nullptr) {
            lv_timer_pause(screensaver_mcp_list_switch_timer_);
        }
        if (screensaver_memo_timer_ != nullptr) {
            lv_timer_pause(screensaver_memo_timer_);
        }
        lv_anim_delete(this, ScreensaverMemoScrollAnimationCallback);
        for (lv_obj_t* memo_label : screensaver_memo_labels_) {
            if (memo_label != nullptr) {
                lv_obj_set_style_translate_y(memo_label, 0, 0);
            }
        }
        lv_obj_add_flag(screensaver_container_, LV_OBJ_FLAG_HIDDEN);
        if (gif_controller_) {
            gif_controller_->Start();
        }
        UpdateSubtitleScroll();
    }
}

/**
 * @brief 显示仅保留表盘外圈的自定义 MCP 清单页面。
 * @param title 固定显示在顶部的清单标题。
 * @param items 按 5 秒间隔轮播的 MCP 中文名称和工具代码数组。
 */
void LcdDisplay::ShowCustomMcpList(
    const std::string& title,
    const std::vector<std::string>& items) {
    DisplayLockGuard lock(this);
    if (screensaver_container_ == nullptr
        || screensaver_mcp_list_viewport_ == nullptr
        || screensaver_mcp_list_title_label_ == nullptr
        || screensaver_mcp_list_label_ == nullptr) {
        return;
    }

    custom_mcp_list_active_.store(true);
    screensaver_active_ = true;
    if (gif_controller_) {
        gif_controller_->Stop();
    }
    if (chat_message_label_ != nullptr) {
        lv_anim_delete(chat_message_label_, nullptr);
        lv_obj_set_style_translate_y(chat_message_label_, 0, 0);
    }
    if (screensaver_memo_timer_ != nullptr) {
        lv_timer_pause(screensaver_memo_timer_);
    }
    lv_anim_delete(this, ScreensaverMemoScrollAnimationCallback);
    SetStandardScreensaverContentVisible(false);

    SetLabelTextIfChanged(
        screensaver_mcp_list_title_label_,
        title.empty() ? "自定义 MCP" : title.c_str());
    custom_mcp_list_items_ = items;
    if (custom_mcp_list_items_.empty()) {
        custom_mcp_list_items_.push_back("暂无自定义 MCP");
    }
    custom_mcp_list_index_ = 0;
    if (screensaver_second_marker_ != nullptr) {
        lv_obj_add_flag(screensaver_second_marker_, LV_OBJ_FLAG_HIDDEN);
    }
    UpdateCustomMcpListItem();
    if (screensaver_mcp_list_switch_timer_ != nullptr) {
        if (custom_mcp_list_items_.size() > 1) {
            lv_timer_resume(screensaver_mcp_list_switch_timer_);
            lv_timer_reset(screensaver_mcp_list_switch_timer_);
        } else {
            lv_timer_pause(screensaver_mcp_list_switch_timer_);
        }
    }
    lv_obj_remove_flag(screensaver_container_, LV_OBJ_FLAG_HIDDEN);
    if (binding_active_ && binding_container_ != nullptr) {
        lv_obj_move_foreground(binding_container_);
    }
    lv_obj_invalidate(screensaver_container_);
}

/**
 * @brief 退出自定义 MCP 清单页面。
 * @details 复用屏保退出路径，统一停止列表动画、恢复普通内容并重新启用对话界面动画。
 */
void LcdDisplay::HideCustomMcpList() {
    if (!custom_mcp_list_active_.load()) {
        return;
    }
    SetScreensaverMode(false);
}

/**
 * @brief 更新当前 MCP 单项并在内容视口中垂直居中。
 * @details 超出单项安全高度时使用省略号截断，不启动滚动，确保语音播放期间屏幕保持低负载。
 */
void LcdDisplay::UpdateCustomMcpListItem() {
    if (screensaver_mcp_list_label_ == nullptr
        || screensaver_mcp_list_item_viewport_ == nullptr
        || custom_mcp_list_items_.empty()) {
        return;
    }

    if (custom_mcp_list_index_ >= custom_mcp_list_items_.size()) {
        custom_mcp_list_index_ = 0;
    }
    lv_label_set_long_mode(screensaver_mcp_list_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(screensaver_mcp_list_label_, LV_SIZE_CONTENT);
    lv_label_set_text(
        screensaver_mcp_list_label_,
        custom_mcp_list_items_[custom_mcp_list_index_].c_str());
    lv_obj_update_layout(screensaver_mcp_list_label_);

    const int text_scale =
        lv_obj_get_style_transform_scale_y_safe(
            screensaver_mcp_list_label_, LV_PART_MAIN);
    const int logical_content_height = lv_obj_get_height(screensaver_mcp_list_label_);
    const int visible_content_height =
        (logical_content_height * text_scale + 255) / 256;
    int visible_height = visible_content_height;
    if (visible_content_height > kCustomMcpListItemSafeHeight) {
        const int logical_safe_height =
            (kCustomMcpListItemSafeHeight * 256 + text_scale - 1) / text_scale;
        lv_label_set_long_mode(screensaver_mcp_list_label_, LV_LABEL_LONG_DOT);
        lv_obj_set_height(screensaver_mcp_list_label_, logical_safe_height);
        visible_height = kCustomMcpListItemSafeHeight;
    }
    lv_obj_align(
        screensaver_mcp_list_label_,
        LV_ALIGN_TOP_MID,
        0,
        (kCustomMcpListItemViewportHeight - visible_height) / 2);
    lv_obj_invalidate(screensaver_mcp_list_item_viewport_);
}

/**
 * @brief 每 5 秒切换到下一项 MCP。
 * @param timer user_data 指向当前 LcdDisplay 实例。
 */
void LcdDisplay::CustomMcpListSwitchTimerCallback(lv_timer_t* timer) {
    auto* display = static_cast<LcdDisplay*>(lv_timer_get_user_data(timer));
    if (display == nullptr || !display->screensaver_active_
        || !display->custom_mcp_list_active_.load()
        || display->custom_mcp_list_items_.size() <= 1) {
        lv_timer_pause(timer);
        return;
    }
    display->custom_mcp_list_index_ =
        (display->custom_mcp_list_index_ + 1)
        % display->custom_mcp_list_items_.size();
    display->UpdateCustomMcpListItem();
}

/**
 * @brief 更新表盘天气位置、当前温度、天气描述和最低/最高温三列。
 * @param location 当前天气位置名称，例如“上海市松江区”。
 * @param temperature 当前摄氏温度。
 * @param weather 中文天气描述。
 * @param low_temperature 当天最低摄氏温度。
 * @param high_temperature 当天最高摄氏温度。
 * @details 方法内部获取 LVGL 锁并复制所有文本，调用结束后不再引用传入字符串。
 */
void LcdDisplay::SetScreensaverWeather(
    const std::string& location,
    int temperature,
    const std::string& weather,
    int low_temperature,
    int high_temperature) {
    char temperature_text[32];
    char range_text[64];
    snprintf(temperature_text, sizeof(temperature_text), "%d℃", temperature);
    snprintf(range_text, sizeof(range_text), "%d/%d", low_temperature, high_temperature);
    SetScreensaverWeatherText(location, temperature_text, weather, range_text);
}

/**
 * @brief 使用已经格式化的文本更新表盘天气位置和三列内容。
 * @param location 天气位置或当前天气服务状态说明。
 * @param temperature 当前温度文本，例如“21℃”或“--℃”。
 * @param weather 中文天气描述或占位文本。
 * @param temperature_range 最低和最高温度文本，例如“18/26”或“--/--”。
 * @details 方法内部获取 LVGL 锁并复制全部文本，适用于后端尚未接入、请求加载中或
 *          返回值无效等无法构造整数温度的界面状态。
 */
void LcdDisplay::SetScreensaverWeatherText(
    const std::string& location,
    const std::string& temperature,
    const std::string& weather,
    const std::string& temperature_range) {
    DisplayLockGuard lock(this);
    SetLabelTextIfChanged(screensaver_weather_location_label_, location.c_str());
    SetLabelTextIfChanged(screensaver_weather_temperature_label_, temperature.c_str());
    SetLabelTextIfChanged(screensaver_weather_description_label_, weather.c_str());
    SetLabelTextIfChanged(screensaver_weather_range_label_, temperature_range.c_str());
}

/**
 * @brief 替换屏保备忘录数组并立即从第一条重新展示。
 * @param memos 后端按提醒时间排序的备忘录正文，最多采用前 5 条。
 */
void LcdDisplay::SetScreensaverMemos(const std::vector<std::string>& memos) {
    DisplayLockGuard lock(this);
    const size_t count = std::min<size_t>(memos.size(), 5);
    if (screensaver_memos_.size() == count
        && std::equal(screensaver_memos_.begin(), screensaver_memos_.end(), memos.begin())) {
        return;
    }
    screensaver_memos_.assign(memos.begin(), memos.begin() + count);
    screensaver_memo_index_ = 0;
    UpdateScreensaverMemo();
    if (screensaver_memo_timer_ != nullptr) {
        if (screensaver_active_ && screensaver_memos_.size() > 1) {
            lv_timer_resume(screensaver_memo_timer_);
            lv_timer_reset(screensaver_memo_timer_);
        } else {
            lv_timer_pause(screensaver_memo_timer_);
        }
    }
}

/**
 * @brief 刷新当前备忘录文本并重新计算三行视口布局。
 * @details 结构化屏保文本使用“时间换行正文”格式，正文最多显示两行，日期时间显示在正文
 *          下方并以第三行作为独立显示行。没有时间首行的加载状态和兼容文本继续使用原有高度判断。
 */
void LcdDisplay::UpdateScreensaverMemo() {
    if (screensaver_memo_labels_[0] == nullptr) {
        return;
    }
    const char* visible_text = "暂无待办";
    if (screensaver_memos_.empty()) {
        screensaver_memo_index_ = 0;
    } else {
        if (screensaver_memo_index_ >= screensaver_memos_.size()) {
            screensaver_memo_index_ = 0;
        }
        visible_text = screensaver_memos_[screensaver_memo_index_].c_str();
    }

    const char* content_text = visible_text;
    const char* line_break = std::strchr(visible_text, '\n');
    if (line_break != nullptr) {
        content_text = line_break + 1;
        screensaver_memo_has_reminder_line_ = true;
    } else {
        screensaver_memo_has_reminder_line_ = false;
    }

    for (lv_obj_t* memo_label : screensaver_memo_labels_) {
        if (memo_label != nullptr) {
            lv_label_set_long_mode(memo_label, LV_LABEL_LONG_WRAP);
            lv_label_set_text(memo_label, content_text);
        }
    }
    const std::string date_text = line_break == nullptr
        ? std::string()
        : std::string(visible_text, static_cast<size_t>(line_break - visible_text));
    for (lv_obj_t* date_label : screensaver_memo_date_labels_) {
        if (date_label != nullptr) {
            lv_label_set_text(date_label, date_text.c_str());
        }
    }
    UpdateScreensaverMemoScroll();
}

/**
 * @brief 根据当前备忘录文本类型配置固定三行或兼容滚动布局。
 * @details 包含显式换行的结构化备忘录正文最多占用两行，日期时间紧接正文显示在下一行；
 *          内容不足两行时，正文和日期整体垂直居中。没有显式时间行的状态文本仍按实际高度居中，
 *          兼容的超长文本继续使用原有纵向滚动逻辑。
 */
void LcdDisplay::UpdateScreensaverMemoScroll() {
    lv_obj_t* measurement_label = screensaver_memo_labels_[0];
    if (screensaver_memo_viewport_ == nullptr || measurement_label == nullptr) {
        return;
    }

    lv_anim_delete(this, ScreensaverMemoScrollAnimationCallback);
    for (lv_obj_t* memo_label : screensaver_memo_labels_) {
        if (memo_label == nullptr) {
            continue;
        }
        lv_obj_set_style_translate_y(memo_label, 0, 0);
        lv_label_set_long_mode(memo_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_height(memo_label, LV_SIZE_CONTENT);
        lv_obj_update_layout(memo_label);
    }

    const int text_scale =
        lv_obj_get_style_transform_scale_y_safe(measurement_label, LV_PART_MAIN);
    const int logical_content_height = lv_obj_get_height(measurement_label);
    const int visible_content_height =
        (logical_content_height * text_scale + 255) / 256;
    const bool has_reminder_line = screensaver_memo_has_reminder_line_;
    if (has_reminder_line) {
        const bool content_fits =
            visible_content_height <= kScreensaverMemoRowHeight * 2;
        const int content_rows = content_fits
            ? std::max(1, std::min(2,
                (visible_content_height + kScreensaverMemoRowHeight - 1)
                    / kScreensaverMemoRowHeight))
            : 2;
        const int content_block_height =
            (content_rows + 1) * kScreensaverMemoRowHeight;
        const int top_offset =
            std::max(0, (kScreensaverMemoViewportHeight - content_block_height) / 2);
        const int logical_viewport_height =
            (kScreensaverMemoRowHeight * 2 * 256 + text_scale - 1) / text_scale;
        for (int row = 0; row < kScreensaverMemoVisibleLines; ++row) {
            lv_obj_t* memo_label = screensaver_memo_labels_[row];
            if (memo_label == nullptr) {
                continue;
            }
            lv_label_set_long_mode(
                memo_label,
                content_fits ? LV_LABEL_LONG_WRAP : LV_LABEL_LONG_DOT);
            lv_obj_set_height(
                memo_label,
                content_fits ? logical_content_height : logical_viewport_height);
            lv_obj_align(memo_label, LV_ALIGN_TOP_MID, 0,
                         top_offset - row * kScreensaverMemoRowHeight);
            lv_obj_update_layout(memo_label);
            lv_obj_t* date_label = screensaver_memo_date_labels_[row];
            if (date_label != nullptr) {
                lv_obj_align(date_label, LV_ALIGN_TOP_MID, 0,
                             top_offset + content_rows * kScreensaverMemoRowHeight
                                 - row * kScreensaverMemoRowHeight);
                lv_obj_update_layout(date_label);
            }
        }
        if (screensaver_memo_timer_ != nullptr) {
            lv_timer_set_period(screensaver_memo_timer_, 8000);
        }
        return;
    }

    if (visible_content_height <= kScreensaverMemoViewportHeight) {
        const int top_offset =
            (kScreensaverMemoViewportHeight - visible_content_height) / 2;
        for (int row = 0; row < kScreensaverMemoVisibleLines; ++row) {
            if (screensaver_memo_labels_[row] != nullptr) {
                lv_obj_align(screensaver_memo_labels_[row], LV_ALIGN_TOP_MID, 0,
                             top_offset - row * kScreensaverMemoRowHeight);
            }
        }
        if (screensaver_memo_timer_ != nullptr) {
            lv_timer_set_period(screensaver_memo_timer_, 8000);
        }
        return;
    }

    for (int row = 0; row < kScreensaverMemoVisibleLines; ++row) {
        if (screensaver_memo_labels_[row] != nullptr) {
            lv_obj_align(screensaver_memo_labels_[row], LV_ALIGN_TOP_MID, 0,
                         -row * kScreensaverMemoRowHeight);
        }
    }
    const int scroll_distance = visible_content_height - kScreensaverMemoViewportHeight;
    const int calculated_duration =
        scroll_distance * 1000 / kScreensaverMemoScrollPixelsPerSecond;
    const int scroll_duration = std::min(30000, std::max(1500, calculated_duration));
    const uint32_t display_period = static_cast<uint32_t>(
        kScreensaverMemoScrollStartDelayMs + scroll_duration + kScreensaverMemoScrollEndDelayMs);
    if (screensaver_memo_timer_ != nullptr) {
        lv_timer_set_period(screensaver_memo_timer_, display_period);
    }

    if (!screensaver_active_) {
        return;
    }

    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, this);
    lv_anim_set_exec_cb(&animation, ScreensaverMemoScrollAnimationCallback);
    lv_anim_set_values(&animation, 0, -scroll_distance);
    lv_anim_set_duration(&animation, scroll_duration);
    lv_anim_set_delay(&animation, kScreensaverMemoScrollStartDelayMs);
    lv_anim_set_repeat_delay(&animation, kScreensaverMemoScrollEndDelayMs);
    lv_anim_set_repeat_count(
        &animation,
        screensaver_memos_.size() > 1 ? 0 : LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&animation, lv_anim_path_linear);
    lv_anim_start(&animation);
}

/**
 * @brief 在同一动画帧内同步移动三条裁切带中的备忘录镜像标签。
 * @param target 指向当前 LcdDisplay 实例。
 * @param value 相对各标签初始位置的 Y 轴物理像素位移。
 * @details 单个 LVGL 动画只计算一次时间和像素位置，本方法再把结果写入三个标签。这样
 *          三条裁切带交界处看到的是同一份文本的同一条扫描行，滚动连续性与单标签聊天
 *          字幕一致，同时仍保留圆屏需要的分段弧形裁切。
 */
void LcdDisplay::ScreensaverMemoScrollAnimationCallback(void* target, int32_t value) {
    auto* display = static_cast<LcdDisplay*>(target);
    if (display == nullptr) {
        return;
    }

    for (lv_obj_t* memo_label : display->screensaver_memo_labels_) {
        if (memo_label != nullptr) {
            lv_obj_set_style_translate_y(memo_label, value, 0);
        }
    }
}

/**
 * @brief 在 LVGL 定时器上下文中切换到下一条备忘录。
 * @param timer user_data 必须是仍然有效的 LcdDisplay 指针。
 */
void LcdDisplay::ScreensaverMemoTimerCallback(lv_timer_t* timer) {
    auto* display = static_cast<LcdDisplay*>(lv_timer_get_user_data(timer));
    if (display == nullptr || !display->screensaver_active_
        || display->screensaver_memos_.empty()) {
        return;
    }
    display->screensaver_memo_index_ =
        (display->screensaver_memo_index_ + 1) % display->screensaver_memos_.size();
    display->UpdateScreensaverMemo();
    lv_timer_reset(timer);
}

#if CONFIG_USE_WECHAT_MESSAGE_STYLE
/**
 * @brief 创建微信气泡消息风格的 LVGL 界面。
 * @details 保留可纵向滚动的多条消息气泡，用于显式选择微信消息样式时的兼容布局。
 */
void LcdDisplay::SetupUI() {
    // Prevent duplicate calls - if already called, return early
    if (setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() 被重复调用，已跳过重复初始化");
        return;
    }
    
    Display::SetupUI();  // Mark SetupUI as called
    DisplayLockGuard lock(this);

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);
    lv_obj_set_style_bg_color(screen, lvgl_theme->background_color(), 0);

    /* Container */
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(container_, 0, 0);
    lv_obj_set_flex_flow(container_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_pad_row(container_, 0, 0);
    lv_obj_set_style_bg_color(container_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_color(container_, lvgl_theme->border_color(), 0);

    /* Layer 1: Top bar - for status icons */
    top_bar_ = lv_obj_create(container_);
    lv_obj_set_size(top_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(top_bar_, 0, 0);
    lv_obj_set_style_bg_opa(top_bar_, LV_OPA_50, 0);  // 50% opacity background
    lv_obj_set_style_bg_color(top_bar_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_width(top_bar_, 0, 0);
    lv_obj_set_style_pad_all(top_bar_, 0, 0);
    lv_obj_set_style_pad_top(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_left(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_pad_right(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_flex_flow(top_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(top_bar_, LV_SCROLLBAR_MODE_OFF);

    // Left icon
    network_label_ = lv_label_create(top_bar_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, icon_font, 0);
    lv_obj_set_style_text_color(network_label_, lvgl_theme->text_color(), 0);

    // Right icons container
    lv_obj_t* right_icons = lv_obj_create(top_bar_);
    lv_obj_set_size(right_icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right_icons, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_icons, 0, 0);
    lv_obj_set_style_pad_all(right_icons, 0, 0);
    lv_obj_set_flex_flow(right_icons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right_icons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    mute_label_ = lv_label_create(right_icons);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, icon_font, 0);
    lv_obj_set_style_text_color(mute_label_, lvgl_theme->text_color(), 0);

    battery_label_ = lv_label_create(right_icons);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, icon_font, 0);
    lv_obj_set_style_text_color(battery_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_margin_left(battery_label_, lvgl_theme->spacing(2), 0);

    /* Layer 2: Status bar - for center text labels */
    status_bar_ = lv_obj_create(screen);
    lv_obj_set_size(status_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(status_bar_, 0, 0);
    lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);  // Transparent background
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_style_pad_top(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_scrollbar_mode(status_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_layout(status_bar_, LV_LAYOUT_NONE, 0);  // Use absolute positioning
    lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, 0);  // Overlap with top_bar_

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(notification_label_, LV_HOR_RES * 0.8);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(notification_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_align(notification_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(status_label_, LV_HOR_RES * 0.8);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);
    lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 0);
    
    /* Content - Chat area */
    content_ = lv_obj_create(container_);
    lv_obj_set_style_radius(content_, 0, 0);
    lv_obj_set_width(content_, LV_HOR_RES);
    lv_obj_set_flex_grow(content_, 1);
    lv_obj_set_style_pad_all(content_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_border_width(content_, 0, 0);
    lv_obj_set_style_bg_color(content_, lvgl_theme->chat_background_color(), 0); // Background for chat area

    // Enable scrolling for chat content
    lv_obj_set_scrollbar_mode(content_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(content_, LV_DIR_VER);
    
    // Create a flex container for chat messages
    lv_obj_set_flex_flow(content_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(content_, lvgl_theme->spacing(4), 0); // Space between messages

    // We'll create chat messages dynamically in SetChatMessage
    chat_message_label_ = nullptr;

    low_battery_popup_ = lv_obj_create(screen);
    lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(low_battery_popup_, LV_HOR_RES * 0.9, text_font->line_height * 2);
    lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, -lvgl_theme->spacing(4));
    lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);
    lv_obj_set_style_radius(low_battery_popup_, lvgl_theme->spacing(4), 0);
    low_battery_label_ = lv_label_create(low_battery_popup_);
    lv_label_set_text(low_battery_label_, Lang::Strings::BATTERY_NEED_CHARGE);
    lv_obj_set_style_text_color(low_battery_label_, lv_color_white(), 0);
    lv_obj_center(low_battery_label_);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);

    emoji_image_ = lv_img_create(screen);
    lv_obj_align(emoji_image_, LV_ALIGN_TOP_MID, 0, text_font->line_height + lvgl_theme->spacing(8));

    // Display AI logo while booting
    emoji_label_ = lv_label_create(screen);
    lv_obj_center(emoji_label_);
    lv_obj_set_style_text_font(emoji_label_, large_icon_font, 0);
    lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(emoji_label_, FONT_AWESOME_MICROCHIP_AI);

    CreateScreensaverUI();
    CreateDeviceBindingUI();
}
#if CONFIG_IDF_TARGET_ESP32P4
#define  MAX_MESSAGES 40
#else
#define  MAX_MESSAGES 20
#endif
/**
 * @brief 更新对应配置并同步到底层资源。
 * @details 实现会维护 LcdDisplay 的内部一致性；发生错误时记录日志，并避免向后续流程传播无效资源。
 */
void LcdDisplay::SetChatMessage(const char* role, const char* content) {
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() 尚未完成，聊天消息将无法显示，角色=%s，内容=%s", role, content);
    }
    DisplayLockGuard lock(this);
    if (content_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(TAG, "聊天消息显示失败，内容容器为空，角色=%s，内容=%s", role, content);
        }
        return;
    }
    
    // Check if message count exceeds limit
    uint32_t child_count = lv_obj_get_child_cnt(content_);
    if (child_count >= MAX_MESSAGES) {
        // Delete the oldest message (first child object)
        lv_obj_t* first_child = lv_obj_get_child(content_, 0);
        if (first_child != nullptr) {
            lv_obj_del(first_child);
            // Refresh child count after deletion
            child_count = lv_obj_get_child_cnt(content_);
        }
        // Scroll to the last message immediately (get last_child after deletion)
        if (child_count > 0) {
            lv_obj_t* last_child = lv_obj_get_child(content_, child_count - 1);
            if (last_child != nullptr && lv_obj_is_valid(last_child)) {
                lv_obj_scroll_to_view_recursive(last_child, LV_ANIM_OFF);
            }
        }
    }
    
    // Collapse system messages (if it's a system message, check if the last message is also a system message)
    if (strcmp(role, "system") == 0) {
        // Refresh child count to get accurate count after potential deletion above
        child_count = lv_obj_get_child_cnt(content_);
        if (child_count > 0) {
            // Get the last message container
            lv_obj_t* last_container = lv_obj_get_child(content_, child_count - 1);
            if (last_container != nullptr && lv_obj_is_valid(last_container) && lv_obj_get_child_cnt(last_container) > 0) {
                // Get the bubble inside the container
                lv_obj_t* last_bubble = lv_obj_get_child(last_container, 0);
                if (last_bubble != nullptr && lv_obj_is_valid(last_bubble)) {
                    // Check if bubble type is system message
                    void* bubble_type_ptr = lv_obj_get_user_data(last_bubble);
                    if (bubble_type_ptr != nullptr && strcmp((const char*)bubble_type_ptr, "system") == 0) {
                        // If the last message is also a system message, delete it
                        lv_obj_del(last_container);
                    }
                }
            }
        }
    } else {
        // Hide the centered AI logo
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }

    // Avoid empty message boxes
    if(strlen(content) == 0) {
        return;
    }

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);

    // Create a message bubble
    lv_obj_t* msg_bubble = lv_obj_create(content_);
    lv_obj_set_style_radius(msg_bubble, 8, 0);
    lv_obj_set_scrollbar_mode(msg_bubble, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_border_width(msg_bubble, 0, 0);
    lv_obj_set_style_pad_all(msg_bubble, lvgl_theme->spacing(4), 0);

    // Create the message text
    lv_obj_t* msg_text = lv_label_create(msg_bubble);
    lv_label_set_text(msg_text, content);
    
    // Calculate bubble width constraints
    lv_coord_t max_width = LV_HOR_RES * 85 / 100 - 16;  // 85% of screen width
    lv_coord_t min_width = 20;  
    
    // Let LVGL calculate the natural text width first
    lv_obj_set_width(msg_text, LV_SIZE_CONTENT);
    lv_obj_update_layout(msg_text);
    lv_coord_t text_width = lv_obj_get_width(msg_text);
    
    // Ensure text width is not less than minimum width
    if (text_width < min_width) {
        text_width = min_width;
    }

    // Constrain to max width
    lv_coord_t bubble_width = (text_width < max_width) ? text_width : max_width;
    
    // Set message text width
    lv_obj_set_width(msg_text, bubble_width);
    lv_label_set_long_mode(msg_text, LV_LABEL_LONG_WRAP);

    // Set bubble width
    lv_obj_set_width(msg_bubble, bubble_width);
    lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);

    // Set alignment and style based on message role
    if (strcmp(role, "user") == 0) {
        // User messages are right-aligned with green background
        lv_obj_set_style_bg_color(msg_bubble, lvgl_theme->user_bubble_color(), 0);
        lv_obj_set_style_bg_opa(msg_bubble, LV_OPA_70, 0);
        // Set text color for contrast
        lv_obj_set_style_text_color(msg_text, lvgl_theme->text_color(), 0);
        
        // Set custom attribute to mark bubble type
        lv_obj_set_user_data(msg_bubble, (void*)"user");
        
        // Set appropriate width for content
        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);
        
        // Don't grow
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    } else if (strcmp(role, "assistant") == 0) {
        // Assistant messages are left-aligned with white background
        lv_obj_set_style_bg_color(msg_bubble, lvgl_theme->assistant_bubble_color(), 0);
        lv_obj_set_style_bg_opa(msg_bubble, LV_OPA_70, 0);
        // Set text color for contrast
        lv_obj_set_style_text_color(msg_text, lvgl_theme->text_color(), 0);
        
        // Set custom attribute to mark bubble type
        lv_obj_set_user_data(msg_bubble, (void*)"assistant");
        
        // Set appropriate width for content
        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);
        
        // Don't grow
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    } else if (strcmp(role, "system") == 0) {
        // System messages are center-aligned with light gray background
        lv_obj_set_style_bg_color(msg_bubble, lvgl_theme->system_bubble_color(), 0);
        lv_obj_set_style_bg_opa(msg_bubble, LV_OPA_70, 0);
        // Set text color for contrast
        lv_obj_set_style_text_color(msg_text, lvgl_theme->system_text_color(), 0);
        
        // Set custom attribute to mark bubble type
        lv_obj_set_user_data(msg_bubble, (void*)"system");
        
        // Set appropriate width for content
        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);
        
        // Don't grow
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    }
    
    // Create a full-width container for user messages to ensure right alignment
    if (strcmp(role, "user") == 0) {
        // Create a full-width container
        lv_obj_t* container = lv_obj_create(content_);
        lv_obj_set_width(container, LV_HOR_RES);
        lv_obj_set_height(container, LV_SIZE_CONTENT);
        
        // Make container transparent and borderless
        lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(container, 0, 0);
        lv_obj_set_style_pad_all(container, 0, 0);
        
        // Move the message bubble into this container
        lv_obj_set_parent(msg_bubble, container);
        
        // Right align the bubble in the container
        lv_obj_align(msg_bubble, LV_ALIGN_RIGHT_MID, -25, 0);
        
        // Auto-scroll to this container
        lv_obj_scroll_to_view_recursive(container, LV_ANIM_ON);
    } else if (strcmp(role, "system") == 0) {
        // Create full-width container for system messages to ensure center alignment
        lv_obj_t* container = lv_obj_create(content_);
        lv_obj_set_width(container, LV_HOR_RES);
        lv_obj_set_height(container, LV_SIZE_CONTENT);
        
        lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(container, 0, 0);
        lv_obj_set_style_pad_all(container, 0, 0);
        
        lv_obj_set_parent(msg_bubble, container);
        lv_obj_align(msg_bubble, LV_ALIGN_CENTER, 0, 0);
        lv_obj_scroll_to_view_recursive(container, LV_ANIM_ON);
    } else {
        // For assistant messages
        // Left align assistant messages
        lv_obj_align(msg_bubble, LV_ALIGN_LEFT_MID, 0, 0);

        // Auto-scroll to the message bubble
        lv_obj_scroll_to_view_recursive(msg_bubble, LV_ANIM_ON);
    }
    
    // Store reference to the latest message label
    chat_message_label_ = msg_text;
}

/**
 * @brief 临时显示预览图并在 PREVIEW_IMAGE_DURATION_MS 后恢复表情。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void LcdDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    DisplayLockGuard lock(this);
    if (content_ == nullptr) {
        return;
    }

    if (image == nullptr) {
        return;
    }
    
    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    // Create a message bubble for image preview
    lv_obj_t* img_bubble = lv_obj_create(content_);
    lv_obj_set_style_radius(img_bubble, 8, 0);
    lv_obj_set_scrollbar_mode(img_bubble, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_border_width(img_bubble, 0, 0);
    lv_obj_set_style_pad_all(img_bubble, lvgl_theme->spacing(4), 0);
    
    // Set image bubble background color (similar to system message)
    lv_obj_set_style_bg_color(img_bubble, lvgl_theme->assistant_bubble_color(), 0);
    lv_obj_set_style_bg_opa(img_bubble, LV_OPA_70, 0);
    
    // Set custom attribute to mark bubble type
    lv_obj_set_user_data(img_bubble, (void*)"image");

    // Create the image object inside the bubble
    lv_obj_t* preview_image = lv_image_create(img_bubble);
    
    // Calculate appropriate size for the image
    lv_coord_t max_width = LV_HOR_RES * 70 / 100;  // 70% of screen width
    lv_coord_t max_height = LV_VER_RES * 50 / 100; // 50% of screen height
    
    // Calculate zoom factor to fit within maximum dimensions
    auto img_dsc = image->image_dsc();
    lv_coord_t img_width = img_dsc->header.w;
    lv_coord_t img_height = img_dsc->header.h;
    if (img_width == 0 || img_height == 0) {
        img_width = max_width;
        img_height = max_height;
        ESP_LOGW(TAG, "图片尺寸无效：%ld x %ld，改用默认尺寸：%ld x %ld", img_width, img_height, max_width, max_height);
    }
    
    lv_coord_t zoom_w = (max_width * 256) / img_width;
    lv_coord_t zoom_h = (max_height * 256) / img_height;
    lv_coord_t zoom = (zoom_w < zoom_h) ? zoom_w : zoom_h;
    
    // Ensure zoom doesn't exceed 256 (100%)
    if (zoom > 256) zoom = 256;
    
    // Set image properties
    lv_image_set_src(preview_image, img_dsc);
    lv_image_set_scale(preview_image, zoom);
    
    // Add event handler to clean up LvglImage when image is deleted
    // We need to transfer ownership of the unique_ptr to the event callback
    LvglImage* raw_image = image.release(); // Release ownership of smart pointer
    lv_obj_add_event_cb(preview_image, [](lv_event_t* e) {
        LvglImage* img = (LvglImage*)lv_event_get_user_data(e);
        if (img != nullptr) {
            delete img; // Properly release memory by deleting LvglImage object
        }
    }, LV_EVENT_DELETE, (void*)raw_image);
    
    // Calculate actual scaled image dimensions
    lv_coord_t scaled_width = (img_width * zoom) / 256;
    lv_coord_t scaled_height = (img_height * zoom) / 256;
    
    // Set bubble size to be 16 pixels larger than the image (8 pixels on each side)
    lv_obj_set_width(img_bubble, scaled_width + 16);
    lv_obj_set_height(img_bubble, scaled_height + 16);
    
    // Don't grow in flex layout
    lv_obj_set_style_flex_grow(img_bubble, 0, 0);
    
    // Center the image within the bubble
    lv_obj_center(preview_image);
    
    // Left align the image bubble like assistant messages
    lv_obj_align(img_bubble, LV_ALIGN_LEFT_MID, 0, 0);

    // Auto-scroll to the image bubble
    lv_obj_scroll_to_view_recursive(img_bubble, LV_ANIM_ON);
}

/**
 * @brief 执行 ClearChatMessages 对应的模块内部流程。
 * @details 实现会维护 LcdDisplay 的内部一致性；发生错误时记录日志，并避免向后续流程传播无效资源。
 */
void LcdDisplay::ClearChatMessages() {
    DisplayLockGuard lock(this);
    if (content_ == nullptr) {
        return;
    }
    
    // Use lv_obj_clean to delete all children of content_ (chat message bubbles)
    lv_obj_clean(content_);
    
    // Reset chat_message_label_ as it has been deleted
    chat_message_label_ = nullptr;
    
    // Show the centered AI logo (emoji_label_) again
    if (emoji_label_ != nullptr) {
        lv_obj_remove_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }
    
    ESP_LOGI(TAG, "聊天消息已清除");
}
#else
/**
 * @brief 创建适配 360x360 圆屏的默认 LVGL 界面。
 * @details 顶部使用收窄的图标栏和状态栏，中央保留表情及预览图区域，
 *          底部使用固定两行的大字字幕容器。所有关键控件均位于圆形可见安全区内。
 */
void LcdDisplay::SetupUI() {
    // Prevent duplicate calls - if already called, return early
    if (setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() 被重复调用，已跳过重复初始化");
        return;
    }
    
    Display::SetupUI();  // Mark SetupUI as called
    DisplayLockGuard lock(this);
    LvglTheme* lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    const lv_font_t* status_font = &font_puhui_basic_20_4;
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);
    lv_obj_set_style_bg_color(screen, lvgl_theme->background_color(), 0);

    /* Container - used as background */
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(container_, 0, 0);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_bg_color(container_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_color(container_, lvgl_theme->border_color(), 0);

    /* Bottom layer: emoji_box_ - centered display */
    emoji_box_ = lv_obj_create(screen);
    lv_obj_set_size(emoji_box_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(emoji_box_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(emoji_box_, 0, 0);
    lv_obj_set_style_border_width(emoji_box_, 0, 0);
    lv_obj_align(emoji_box_, LV_ALIGN_CENTER, 0, kRoundEmojiOffsetY);

    emoji_label_ = lv_label_create(emoji_box_);
    lv_obj_set_style_text_font(emoji_label_, large_icon_font, 0);
    lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(emoji_label_, FONT_AWESOME_MICROCHIP_AI);

    emoji_image_ = lv_img_create(emoji_box_);
    lv_obj_center(emoji_image_);
    lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);

    /* Middle layer: preview_image_ - centered display */
    preview_image_ = lv_image_create(screen);
    lv_obj_set_size(preview_image_, width_ / 2, height_ / 2);
    lv_obj_align(preview_image_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);

    /*
     * 第一层顶部状态栏固定为三栏：左侧网络、中间时间、右侧电量。
     */
    top_bar_ = lv_obj_create(screen);
    lv_obj_set_size(top_bar_, kRoundTopBarWidth, kRoundTopBarHeight);
    lv_obj_set_style_radius(top_bar_, kRoundTopBarHeight / 2, 0);
    lv_obj_set_style_bg_opa(top_bar_, LV_OPA_50, 0);
    lv_obj_set_style_bg_color(top_bar_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_width(top_bar_, 0, 0);
    lv_obj_set_style_pad_all(top_bar_, 0, 0);
    lv_obj_set_style_pad_top(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_left(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_pad_right(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_pad_column(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_flex_flow(top_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(top_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(top_bar_, LV_ALIGN_TOP_MID, 0, kRoundTopBarOffsetY);

    /*
     * 左侧固定显示网络状态图标。标签使用与右侧电量标签相同的固定宽度，
     * 从而使中间时间标签的几何中心始终与屏幕中线重合。
     */
    network_label_ = lv_label_create(top_bar_);
    lv_obj_set_width(network_label_, kRoundTopBarSideWidth);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, icon_font, 0);
    lv_obj_set_style_text_align(network_label_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(network_label_, lvgl_theme->text_color(), 0);

    /*
     * 中间时间标签占用顶部栏的全部剩余宽度，并始终保持居中。
     * 时间与设备状态使用不同的标签，避免语音状态或通知内容覆盖时钟。
     */
    time_label_ = lv_label_create(top_bar_);
    lv_obj_set_width(time_label_, 1);
    lv_obj_set_flex_grow(time_label_, 1);
    lv_obj_set_style_text_font(time_label_, status_font, 0);
    lv_obj_set_style_text_align(time_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(time_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(time_label_, "--:--");

    /*
     * 右侧只显示电量图标。固定宽度与左侧网络标签完全一致，文字右对齐，
     * 既维持“网络、时间、电量”的三栏顺序，也避免电量图标挤压时钟位置。
     */
    battery_label_ = lv_label_create(top_bar_);
    lv_obj_set_width(battery_label_, kRoundTopBarSideWidth);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, icon_font, 0);
    lv_obj_set_style_text_align(battery_label_, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(battery_label_, lvgl_theme->text_color(), 0);

    /*
     * 设备运行状态和临时通知保留在顶部栏下方的独立区域中。
     * 该区域不属于顶部三栏，因此 SetStatus() 和 ShowNotification() 不会影响时间标签。
     */
    status_bar_ = lv_obj_create(screen);
    lv_obj_set_size(status_bar_, kRoundTopBarWidth, kRoundTopBarHeight);
    lv_obj_set_style_radius(status_bar_, 0, 0);
    lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_scrollbar_mode(status_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_layout(status_bar_, LV_LAYOUT_NONE, 0);
    lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0,
                 kRoundTopBarOffsetY + kRoundTopBarHeight);

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(notification_label_, LV_PCT(100));
    lv_obj_set_style_text_font(notification_label_, status_font, 0);
    lv_label_set_long_mode(notification_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(notification_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_align(notification_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(status_label_, LV_PCT(100));
    lv_obj_set_style_text_font(status_label_, status_font, 0);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);
    lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 0);

    /*
     * 圆屏字幕区使用固定两行高度。不让容器随长文本无限向上扩展，
     * 否则会遮挡中央表情并进入圆屏下方的不可见区。
     */
    bottom_bar_ = lv_obj_create(screen);
    const int subtitle_text_height = text_font->line_height * 2 + kRoundSubtitleLineSpacing;
    lv_obj_set_size(bottom_bar_, kRoundSubtitleWidth,
                    ScaleSubtitleSize(subtitle_text_height) + lvgl_theme->spacing(8));
    lv_obj_set_style_radius(bottom_bar_, lvgl_theme->spacing(10), 0);
    lv_obj_set_style_bg_color(bottom_bar_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_80, 0);
    lv_obj_set_style_text_color(bottom_bar_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_pad_all(bottom_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_border_width(bottom_bar_, 0, 0);
    lv_obj_set_scrollbar_mode(bottom_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, -kRoundSubtitleBottomOffset);

    /*
     * 字幕容器始终只显示两行；标签自身保存完整文本，超过两行时由
     * UpdateSubtitleScroll() 在容器裁切区域内执行纵向滚动。
     */
    chat_message_label_ = lv_label_create(bottom_bar_);
    lv_label_set_text(chat_message_label_, "");
    lv_obj_set_size(chat_message_label_, kRoundSubtitleLogicalTextWidth, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(chat_message_label_, text_font, 0);
    lv_obj_set_style_text_line_space(chat_message_label_, kRoundSubtitleLineSpacing, 0);
    lv_obj_set_style_transform_scale(chat_message_label_, kRoundSubtitleScale, 0);
    lv_obj_set_style_transform_pivot_x(chat_message_label_, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(chat_message_label_, 0, 0);
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(chat_message_label_, lvgl_theme->text_color(), 0);
    lv_obj_align(chat_message_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);

    low_battery_popup_ = lv_obj_create(screen);
    lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(low_battery_popup_, kRoundPopupWidth, status_font->line_height * 2);
    lv_obj_align(low_battery_popup_, LV_ALIGN_CENTER, 0, 72);
    lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);
    lv_obj_set_style_radius(low_battery_popup_, lvgl_theme->spacing(4), 0);
    
    low_battery_label_ = lv_label_create(low_battery_popup_);
    lv_label_set_text(low_battery_label_, Lang::Strings::BATTERY_NEED_CHARGE);
    lv_obj_set_style_text_font(low_battery_label_, status_font, 0);
    lv_obj_set_style_text_color(low_battery_label_, lv_color_white(), 0);
    lv_obj_center(low_battery_label_);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);

    CreateScreensaverUI();
    CreateDeviceBindingUI();
}

/**
 * @brief 临时显示预览图并在 PREVIEW_IMAGE_DURATION_MS 后恢复表情。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void LcdDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    DisplayLockGuard lock(this);
    if (preview_image_ == nullptr) {
        ESP_LOGE(TAG, "预览图片尚未初始化");
        return;
    }

    if (image == nullptr) {
        esp_timer_stop(preview_timer_);
        lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
        preview_image_cached_.reset();
        if (gif_controller_) {
            gif_controller_->Start();
        }
        return;
    }

    preview_image_cached_ = std::move(image);
    auto img_dsc = preview_image_cached_->image_dsc();
    lv_image_set_src(preview_image_, img_dsc);
    if (img_dsc->header.w > 0 && img_dsc->header.h > 0) {
        // zoom factor 0.5
        lv_image_set_scale(preview_image_, 128 * width_ / img_dsc->header.w);
    }

    // Hide emoji_box_
    if (gif_controller_) {
        gif_controller_->Stop();
    }
    lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
    esp_timer_stop(preview_timer_);
    ESP_ERROR_CHECK(esp_timer_start_once(preview_timer_, PREVIEW_IMAGE_DURATION_MS * 1000));
}

/**
 * @brief 更新圆屏底部的对话字幕。
 * @param role 消息角色，用于日志诊断；默认圆屏布局不按角色改变字幕位置。
 * @param content UTF-8 字幕文本；nullptr 或空字符串会隐藏字幕区。
 * @details 方法完整保留云端下发的 UTF-8 文本，使用 30 px 字形源换行并缩放到目标字号。
 *          两行以内的内容静止居中；超过两行时在固定窗口内纵向滚动，不再截断回答尾部。
 */
void LcdDisplay::SetChatMessage(const char* role, const char* content) {
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() 尚未完成，聊天消息将无法显示，角色=%s，内容=%s", role, content);
    }
    DisplayLockGuard lock(this);
    if (chat_message_label_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(TAG, "聊天消息显示失败，消息标签为空，角色=%s，内容=%s", role, content);
        }
        return;
    }

    /*
     * 新字幕到达时先同步终止上一段字幕动画并恢复初始位置，再替换标签内容。
     * 这一步放在 lv_label_set_text() 之前，确保旧动画不会在文本替换过程中继续写入
     * translate_y，从而让新字幕能够立即从第一行显示。
     */
    lv_anim_delete(chat_message_label_, nullptr);
    lv_obj_set_style_translate_y(chat_message_label_, 0, 0);
    lv_obj_align(chat_message_label_, LV_ALIGN_CENTER, 0, 0);

    const char* complete_text = content != nullptr ? content : "";
    lv_label_set_text(chat_message_label_, complete_text);
    UpdateSubtitleScroll();
    lv_obj_invalidate(chat_message_label_);

    // Show bottom_bar_ only when there is content (and subtitle is not globally hidden)
    if (bottom_bar_ != nullptr) {
        if (complete_text[0] == '\0') {
            lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        } else if (!hide_subtitle_) {
            lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, -kRoundSubtitleBottomOffset);
        lv_obj_invalidate(bottom_bar_);
    }
}

/**
 * @brief 执行 ClearChatMessages 对应的模块内部流程。
 * @details 实现会维护 LcdDisplay 的内部一致性；发生错误时记录日志，并避免向后续流程传播无效资源。
 */
void LcdDisplay::ClearChatMessages() {
    DisplayLockGuard lock(this);
    // In non-wechat mode, just clear the chat message label and hide the bar
    if (chat_message_label_ != nullptr) {
        lv_label_set_text(chat_message_label_, "");
        UpdateSubtitleScroll();
    }
    if (bottom_bar_ != nullptr) {
        lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
    }
}
#endif

/**
 * @brief 在静态 PNG 和 GIF 控制器之间切换表情。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void LcdDisplay::SetEmotion(const char* emotion) {
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() 尚未完成，表情无法显示，表情=%s", emotion);
    }
    if (emoji_image_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(TAG, "表情显示失败，表情图片为空，表情=%s", emotion);
        }
        return;
    }

    auto emoji_collection = static_cast<LvglTheme*>(current_theme_)->emoji_collection();
    auto image = emoji_collection != nullptr ? emoji_collection->GetEmojiImage(emotion) : nullptr;
    if (image == nullptr) {
        const char* utf8 = font_awesome_get_utf8(emotion);
        if (utf8 != nullptr && emoji_label_ != nullptr) {
            DisplayLockGuard lock(this);
            if (gif_controller_) {
                gif_controller_->Stop();
                gif_controller_.reset();
            }
            lv_label_set_text(emoji_label_, utf8);
            lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    DisplayLockGuard lock(this);
    // Stop any running GIF animation in the same lock scope as setting new image
    // to prevent LVGL from accessing freed image data between operations
    if (gif_controller_) {
        gif_controller_->Stop();
        gif_controller_.reset();
    }
    if (image->IsGif()) {
        // Create new GIF controller
        gif_controller_ = std::make_unique<LvglGif>(image->image_dsc());
        
        if (gif_controller_->IsLoaded()) {
            // Set up frame update callback
            gif_controller_->SetFrameCallback([this]() {
                lv_image_set_src(emoji_image_, gif_controller_->image_dsc());
            });
            
            // Set initial frame and start animation
            lv_image_set_src(emoji_image_, gif_controller_->image_dsc());
            gif_controller_->Start();
            
            // Show GIF, hide others
            lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        } else {
            ESP_LOGE(TAG, "表情 GIF 加载失败：%s", emotion);
            gif_controller_.reset();
        }
    } else {
        lv_image_set_src(emoji_image_, image->image_dsc());
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
    }

#if CONFIG_USE_WECHAT_MESSAGE_STYLE
    // In WeChat message style, if emotion is neutral, don't display it
    uint32_t child_count = lv_obj_get_child_cnt(content_);
    if (strcmp(emotion, "neutral") == 0 && child_count > 0) {
        // Stop GIF animation if running
        if (gif_controller_) {
            gif_controller_->Stop();
            gif_controller_.reset();
        }
        
        lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }
#endif
}

/**
 * @brief 将主题颜色、字体、背景和表情集合应用到已创建控件。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void LcdDisplay::SetTheme(Theme* theme) {
    DisplayLockGuard lock(this);
    
    auto lvgl_theme = static_cast<LvglTheme*>(theme);
    
    // Get the active screen
    lv_obj_t* screen = lv_screen_active();

    // Set font
    auto text_font = lvgl_theme->text_font()->font();
    const lv_font_t* status_font = &font_puhui_basic_20_4;
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    if (text_font->line_height >= 40) {
        if (mute_label_ != nullptr) {
            lv_obj_set_style_text_font(mute_label_, large_icon_font, 0);
        }
        if (battery_label_ != nullptr) {
            lv_obj_set_style_text_font(battery_label_, large_icon_font, 0);
        }
        if (network_label_ != nullptr) {
            lv_obj_set_style_text_font(network_label_, large_icon_font, 0);
        }
    } else {
        if (mute_label_ != nullptr) {
            lv_obj_set_style_text_font(mute_label_, icon_font, 0);
        }
        if (battery_label_ != nullptr) {
            lv_obj_set_style_text_font(battery_label_, icon_font, 0);
        }
        if (network_label_ != nullptr) {
            lv_obj_set_style_text_font(network_label_, icon_font, 0);
        }
    }

    // Set parent text color
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);

    /*
     * 默认资源加载完成后，主题中的 text_font 会从精简内置字体替换为完整中文字库。
     * 表盘标签设置了局部字体，不会自动继承屏幕字体，因此需要在此显式同步。
     */
    ApplyScreensaverTextFont(text_font);
    ApplyCustomMcpListFont(text_font);
    ApplyScreensaverLocationFont(text_font);
    ApplyScreensaverWeatherFont(text_font);
    ApplyScreensaverDateFont(text_font);
    ApplyScreensaverStatusIconFont(icon_font);
    ApplyDeviceBindingFont(text_font);
    UpdateScreensaverMemo();
    if (custom_mcp_list_active_.load()) {
        UpdateCustomMcpListItem();
    }

    // Set background image
    if (lvgl_theme->background_image() != nullptr) {
        lv_obj_set_style_bg_image_src(container_, lvgl_theme->background_image()->image_dsc(), 0);
    } else {
        lv_obj_set_style_bg_image_src(container_, nullptr, 0);
        lv_obj_set_style_bg_color(container_, lvgl_theme->background_color(), 0);
    }
    
    // Update top bar background color with 50% opacity
    if (top_bar_ != nullptr) {
        lv_obj_set_style_bg_opa(top_bar_, LV_OPA_50, 0);
        lv_obj_set_style_bg_color(top_bar_, lvgl_theme->background_color(), 0);
    }
    
    // Update status bar elements
    if (network_label_ != nullptr) {
        lv_obj_set_style_text_color(network_label_, lvgl_theme->text_color(), 0);
    }
    if (time_label_ != nullptr) {
        lv_obj_set_style_text_font(time_label_, status_font, 0);
        lv_obj_set_style_text_color(time_label_, lvgl_theme->text_color(), 0);
    }
    if (status_label_ != nullptr) {
        lv_obj_set_style_text_font(status_label_, status_font, 0);
        lv_obj_set_style_text_color(status_label_, lvgl_theme->text_color(), 0);
    }
    if (notification_label_ != nullptr) {
        lv_obj_set_style_text_font(notification_label_, status_font, 0);
        lv_obj_set_style_text_color(notification_label_, lvgl_theme->text_color(), 0);
    }
    if (mute_label_ != nullptr) {
        lv_obj_set_style_text_color(mute_label_, lvgl_theme->text_color(), 0);
    }
    if (battery_label_ != nullptr) {
        lv_obj_set_style_text_color(battery_label_, lvgl_theme->text_color(), 0);
    }
    lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);

    // If we have the chat message style, update all message bubbles
#if CONFIG_USE_WECHAT_MESSAGE_STYLE
    // Set content background opacity
    lv_obj_set_style_bg_opa(content_, LV_OPA_TRANSP, 0);

    // Iterate through all children of content (message containers or bubbles)
    uint32_t child_count = lv_obj_get_child_cnt(content_);
    for (uint32_t i = 0; i < child_count; i++) {
        lv_obj_t* obj = lv_obj_get_child(content_, i);
        if (obj == nullptr) continue;
        
        lv_obj_t* bubble = nullptr;
        
        // Check if this object is a container or bubble
        // If it's a container (user or system message), get its child as bubble
        // If it's a bubble (assistant message), use it directly
        if (lv_obj_get_child_cnt(obj) > 0) {
            // Might be a container, check if it's a user or system message container
            // User and system message containers are transparent
            lv_opa_t bg_opa = lv_obj_get_style_bg_opa(obj, LV_PART_MAIN);
            if (bg_opa == LV_OPA_TRANSP) {
                // This is a user or system message container
                bubble = lv_obj_get_child(obj, 0);
            } else {
                // This might be an assistant message bubble itself
                bubble = obj;
            }
        } else {
            // No child elements, might be other UI elements, skip
            continue;
        }
        
        if (bubble == nullptr) continue;
        
        // Use saved user data to identify bubble type
        void* bubble_type_ptr = lv_obj_get_user_data(bubble);
        if (bubble_type_ptr != nullptr) {
            const char* bubble_type = static_cast<const char*>(bubble_type_ptr);
            
            // Apply correct color based on bubble type
            if (strcmp(bubble_type, "user") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->user_bubble_color(), 0);
            } else if (strcmp(bubble_type, "assistant") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->assistant_bubble_color(), 0); 
            } else if (strcmp(bubble_type, "system") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->system_bubble_color(), 0);
            } else if (strcmp(bubble_type, "image") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->system_bubble_color(), 0);
            }
            
            // Update border color
            lv_obj_set_style_border_color(bubble, lvgl_theme->border_color(), 0);
            
            // Update text color for the message
            if (lv_obj_get_child_cnt(bubble) > 0) {
                lv_obj_t* text = lv_obj_get_child(bubble, 0);
                if (text != nullptr) {
                    // Set text color based on bubble type
                    if (strcmp(bubble_type, "system") == 0) {
                        lv_obj_set_style_text_color(text, lvgl_theme->system_text_color(), 0);
                    } else {
                        lv_obj_set_style_text_color(text, lvgl_theme->text_color(), 0);
                    }
                }
            }
        } else {
            ESP_LOGW(TAG, "未找到子消息气泡类型，下标=%lu", i);
        }
    }
#else
    // Simple UI mode - just update the main chat message
    if (chat_message_label_ != nullptr) {
        const int subtitle_text_height = text_font->line_height * 2 + kRoundSubtitleLineSpacing;
        lv_obj_set_size(chat_message_label_, kRoundSubtitleLogicalTextWidth, LV_SIZE_CONTENT);
        lv_obj_set_style_text_font(chat_message_label_, text_font, 0);
        lv_obj_set_style_text_line_space(chat_message_label_, kRoundSubtitleLineSpacing, 0);
        lv_obj_set_style_transform_scale(chat_message_label_, kRoundSubtitleScale, 0);
        lv_obj_set_style_transform_pivot_x(chat_message_label_, LV_PCT(50), 0);
        lv_obj_set_style_transform_pivot_y(chat_message_label_, 0, 0);
        lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(chat_message_label_, lvgl_theme->text_color(), 0);
        if (bottom_bar_ != nullptr) {
            lv_obj_set_size(bottom_bar_, kRoundSubtitleWidth,
                            ScaleSubtitleSize(subtitle_text_height) + lvgl_theme->spacing(8));
            lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, -kRoundSubtitleBottomOffset);
        }
        UpdateSubtitleScroll();
    }
    
    if (emoji_label_ != nullptr) {
        lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);
    }
    
    // 字幕背景保持 80% 不透明度，避免表情图干扰大字字幕的可读性。
    if (bottom_bar_ != nullptr) {
        lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_80, 0);
        lv_obj_set_style_bg_color(bottom_bar_, lvgl_theme->background_color(), 0);
    }
#endif
    
    // Update low battery popup
    lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);
    lv_obj_set_style_text_font(low_battery_label_, status_font, 0);

    // No errors occurred. Save theme to settings
    Display::SetTheme(lvgl_theme);
}

/**
 * @brief 设置是否隐藏字幕。
 * @param hide true 隐藏并清空当前字幕。
 * @details 本实现完成实际资源操作和状态同步；失败路径会保留可恢复状态并输出诊断日志。
 */
void LcdDisplay::SetHideSubtitle(bool hide) {
    DisplayLockGuard lock(this);
    hide_subtitle_ = hide;
    
    // Immediately update UI visibility based on the setting
    if (bottom_bar_ != nullptr) {
        if (hide) {
            if (chat_message_label_ != nullptr) {
                lv_anim_delete(chat_message_label_, nullptr);
                lv_obj_set_style_translate_y(chat_message_label_, 0, 0);
            }
            lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        } else {
            // Only show if there is actual content to display
            const char* text = (chat_message_label_ != nullptr) ? lv_label_get_text(chat_message_label_) : nullptr;
            if (text != nullptr && text[0] != '\0') {
                UpdateSubtitleScroll();
                lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}
