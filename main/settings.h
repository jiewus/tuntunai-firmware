#ifndef SETTINGS_H
#define SETTINGS_H

#include <string>
#include <nvs_flash.h>

/**
 * @file settings.h
 * @brief ESP-IDF NVS 键值配置的 C++ 资源管理封装。
 */

/**
 * @brief 在指定 NVS 命名空间中读写字符串、整数和布尔值。
 *
 * 只读实例不会提交修改；可写实例在析构时统一 commit，减少 Flash 擦写次数。
 */
class Settings {
public:
    /**
     * @param ns NVS 命名空间，最长 15 字符。
     * @param read_write true 表示以读写方式打开。
     */
    Settings(const std::string& ns, bool read_write = false);
    /**
     * @brief 提交尚未写入 Flash 的修改并关闭 NVS 句柄。
     */
    ~Settings();

    /**
     * @brief 读取字符串。
     * @param key 键名。
     * @param default_value 不存在或读取失败时的返回值。
     */
    std::string GetString(const std::string& key, const std::string& default_value = "");
    /**
     * @brief 写入字符串。
     * @note 只读实例调用不会生效。
     */
    void SetString(const std::string& key, const std::string& value);
    /**
     * @brief 读取 32 位整数，不存在时返回 default_value。
     */
    int32_t GetInt(const std::string& key, int32_t default_value = 0);
    /**
     * @brief 写入 32 位整数。
     */
    void SetInt(const std::string& key, int32_t value);
    /**
     * @brief 读取布尔值；底层以整数形式存储。
     */
    bool GetBool(const std::string& key, bool default_value = false);
    /**
     * @brief 写入布尔值。
     */
    void SetBool(const std::string& key, bool value);
    /**
     * @brief 删除指定键。
     * @param key 要删除的键名。
     */
    void EraseKey(const std::string& key);
    /**
     * @brief 清空当前命名空间内的所有键。
     */
    void EraseAll();

private:
    std::string ns_;
    nvs_handle_t nvs_handle_ = 0;
    bool read_write_ = false;
    bool dirty_ = false;
};

#endif
