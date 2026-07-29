#ifndef GIFDEC_H
#define GIFDEC_H

#ifdef __cplusplus
extern "C" {
#endif

#include <lvgl.h>

#include <stdint.h>

/**
 * @file gifdec.h
 * @brief gifdec 解码器的 C 接口。该文件来自上游算法，仅补充接口级中文说明。
 */

/**
 * @brief GIF 调色板，最多保存 256 个 RGB888 颜色。
 */
typedef struct _gd_Palette {
    int size;
    uint8_t colors[0x100 * 3];
} gd_Palette;

/**
 * @brief 当前帧 Graphic Control Extension：延时、透明色和处置方式。
 */
typedef struct _gd_GCE {
    uint16_t delay;
    uint8_t tindex;
    uint8_t disposal;
    int input;
    int transparency;
} gd_GCE;



/**
 * @brief 一个已打开 GIF 的完整解析与解码上下文。
 */
typedef struct _gd_GIF {
    lv_fs_file_t fd;
    const char * data;
    uint8_t is_file;
    uint32_t f_rw_p;
    int32_t anim_start;
    uint16_t width, height;
    uint16_t depth;
    int32_t loop_count;
    gd_GCE gce;
    gd_Palette * palette;
    gd_Palette lct, gct;
    void (*plain_text)(
        struct _gd_GIF * gif, uint16_t tx, uint16_t ty,
        uint16_t tw, uint16_t th, uint8_t cw, uint8_t ch,
        uint8_t fg, uint8_t bg
    );
    void (*comment)(struct _gd_GIF * gif);
    void (*application)(struct _gd_GIF * gif, char id[8], char auth[3]);
    uint16_t fx, fy, fw, fh;
    uint8_t bgindex;
    uint8_t * canvas, * frame;
    /** true 表示整个解码上下文由 PSRAM 分配，关闭时必须使用 LVGL 释放函数。 */
    uint8_t allocated_from_psram;
#if LV_GIF_CACHE_DECODE_DATA
    uint8_t *lzw_cache;
#endif
} gd_GIF;

/**
 * @brief 从 LVGL 文件系统路径打开 GIF。
 * @return 失败时返回 NULL。
 */
gd_GIF * gd_open_gif_file(const char * fname);

/**
 * @brief 从内存中的完整 GIF 文件打开。
 * @param data 文件首地址。
 */
gd_GIF * gd_open_gif_data(const void * data);

/**
 * @brief 将当前帧与画布合成为 RGB888 输出。
 * @param buffer 至少 width*height*3 字节。
 */
void gd_render_frame(gd_GIF * gif, uint8_t * buffer);

/**
 * @brief 解码下一帧。
 * @return 1 有新帧，0 到达结尾，负值表示错误。
 */
int gd_get_frame(gd_GIF * gif);
/**
 * @brief 回绕到第一帧并恢复初始画布状态。
 */
void gd_rewind(gd_GIF * gif);
/**
 * @brief 关闭文件并释放解码上下文全部内存。
 */
void gd_close_gif(gd_GIF * gif);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* GIFDEC_H */
