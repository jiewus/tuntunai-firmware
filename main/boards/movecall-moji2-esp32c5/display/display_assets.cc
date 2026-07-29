#include "lcd_display.h"

#include "assets.h"
#include "lvgl_display/emoji_collection.h"
#include "lvgl_display/lvgl_font.h"
#include "lvgl_display/lvgl_image.h"
#include "lvgl_display/lvgl_theme.h"

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>

#define TAG "LcdDisplay"

bool LcdDisplay::SetThemeByName(const std::string& theme_name) {
    auto* theme = LvglThemeManager::GetInstance().GetTheme(theme_name);
    if (theme == nullptr) {
        return false;
    }
    SetTheme(theme);
    return true;
}

bool LcdDisplay::ApplyAssets(Assets& assets, cJSON* index, bool refresh_theme) {
    void* ptr = nullptr;
    size_t size = 0;
    auto& theme_manager = LvglThemeManager::GetInstance();
    auto* light_theme = theme_manager.GetTheme("light");
    auto* dark_theme = theme_manager.GetTheme("dark");

    cJSON* font = cJSON_GetObjectItem(index, "text_font");
    if (cJSON_IsString(font)) {
        const std::string font_file = font->valuestring;
        if (assets.GetAssetData(font_file, ptr, size)) {
            auto text_font = std::make_shared<LvglCBinFont>(ptr);
            if (text_font->font() == nullptr) {
                ESP_LOGE(TAG, "Failed to load fonts.bin");
                return false;
            }
            if (light_theme != nullptr) {
                light_theme->set_text_font(text_font);
            }
            if (dark_theme != nullptr) {
                dark_theme->set_text_font(text_font);
            }
        } else {
            ESP_LOGE(TAG, "The font file %s is not found", font_file.c_str());
        }
    }

    cJSON* emoji_collection = cJSON_GetObjectItem(index, "emoji_collection");
    if (cJSON_IsArray(emoji_collection)) {
        auto custom_emojis = std::make_shared<EmojiCollection>();
        const int emoji_count = cJSON_GetArraySize(emoji_collection);
        for (int i = 0; i < emoji_count; ++i) {
            cJSON* emoji = cJSON_GetArrayItem(emoji_collection, i);
            cJSON* name = cJSON_GetObjectItem(emoji, "name");
            cJSON* file = cJSON_GetObjectItem(emoji, "file");
            cJSON* eaf = cJSON_GetObjectItem(emoji, "eaf");
            if (!cJSON_IsObject(emoji) || !cJSON_IsString(name) || !cJSON_IsString(file) || eaf != nullptr) {
                continue;
            }
            if (!assets.GetAssetData(file->valuestring, ptr, size)) {
                ESP_LOGE(TAG, "Emoji %s image file %s is not found", name->valuestring, file->valuestring);
                continue;
            }
            custom_emojis->AddEmoji(name->valuestring, new LvglRawImage(ptr, size));
        }
        if (light_theme != nullptr) {
            light_theme->set_emoji_collection(custom_emojis);
        }
        if (dark_theme != nullptr) {
            dark_theme->set_emoji_collection(custom_emojis);
        }
    }

    auto apply_skin = [&assets, &ptr, &size](cJSON* skin, LvglTheme* theme) -> bool {
        if (!cJSON_IsObject(skin) || theme == nullptr) {
            return true;
        }
        cJSON* text_color = cJSON_GetObjectItem(skin, "text_color");
        cJSON* background_color = cJSON_GetObjectItem(skin, "background_color");
        cJSON* background_image = cJSON_GetObjectItem(skin, "background_image");
        if (cJSON_IsString(text_color)) {
            theme->set_text_color(LvglTheme::ParseColor(text_color->valuestring));
        }
        if (cJSON_IsString(background_color)) {
            const auto color = LvglTheme::ParseColor(background_color->valuestring);
            theme->set_background_color(color);
            theme->set_chat_background_color(color);
        }
        if (cJSON_IsString(background_image)) {
            if (!assets.GetAssetData(background_image->valuestring, ptr, size)) {
                ESP_LOGE(TAG, "The background image file %s is not found", background_image->valuestring);
                return false;
            }
            theme->set_background_image(std::make_shared<LvglCBinImage>(ptr));
        }
        return true;
    };

    cJSON* skin = cJSON_GetObjectItem(index, "skin");
    if (cJSON_IsObject(skin)) {
        if (!apply_skin(cJSON_GetObjectItem(skin, "light"), light_theme)
            || !apply_skin(cJSON_GetObjectItem(skin, "dark"), dark_theme)) {
            return false;
        }
    }

    if (refresh_theme) {
        ESP_LOGI(TAG, "Refreshing display theme...");
        if (current_theme_ != nullptr) {
            SetTheme(current_theme_);
        }
        cJSON* hide_subtitle = cJSON_GetObjectItem(index, "hide_subtitle");
        if (cJSON_IsBool(hide_subtitle)) {
            const bool hide = cJSON_IsTrue(hide_subtitle);
            SetHideSubtitle(hide);
            ESP_LOGI(TAG, "Set hide_subtitle to %s", hide ? "true" : "false");
        }
    }
    return true;
}

bool LcdDisplay::SetPreviewImageData(void* data, size_t size) {
    try {
        SetPreviewImage(std::make_unique<LvglAllocatedImage>(data, size));
        return true;
    } catch (...) {
        heap_caps_free(data);
        return false;
    }
}
