#ifndef EMOJI_COLLECTION_H
#define EMOJI_COLLECTION_H

#include "lvgl_image.h"

#include <lvgl.h>

#include <map>
#include <string>
#include <memory>


/**
 * @file emoji_collection.h
 * @brief 表情名称到 LVGL 图片的索引集合。
 */

/**
 * @brief 持有一组表情图片并负责析构释放。
 */
class EmojiCollection {
public:
    /**
     * @brief 添加或替换表情。
     * @param name 云端 emotion 字段名称。
     * @param image 转移给集合管理。
     */
    virtual void AddEmoji(const std::string& name, LvglImage* image);
    /**
     * @brief 查找表情。
     * @return 未命中时回退 neutral，仍未找到则返回 nullptr。
     */
    virtual const LvglImage* GetEmojiImage(const char* name);
    virtual ~EmojiCollection();

private:
    std::map<std::string, LvglImage*> emoji_collection_;
};

/**
 * @brief 注册固件内置的 32 像素 Twemoji 集合。
 */
class Twemoji32 : public EmojiCollection {
public:
    Twemoji32();
};

/**
 * @brief 注册适合本板 360x360 屏幕的 64 像素 Twemoji 集合。
 */
class Twemoji64 : public EmojiCollection {
public:
    Twemoji64();
};

#endif
