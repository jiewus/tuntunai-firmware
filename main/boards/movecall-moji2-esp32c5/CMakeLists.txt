# This component owns all C5/Moji2 hardware bindings. New boards keep their
# sources and hardware-specific component requirements in their own directory.
idf_component_register(
    SRCS
        "movecall_moji2_esp32c5.cc"
        "audio/es8311_audio_codec.cc"
        "battery/adc_battery_monitor.cc"
        "input/button.cc"
        "led/single_led.cc"
        "power/power_save_timer.cc"
        "display/pwm_backlight.cc"
        "display/display_assets.cc"
        "display/lcd_display.cc"
        "display/lcd_screensaver.cc"
        "display/lcd_mcp_view.cc"
        "display/lcd_binding_view.cc"
        "display/lcd_chat_view.cc"
        "display/fonts/font_puhui_time_64.cc"
        "display/lvgl_display/lvgl_display.cc"
        "display/lvgl_display/emoji_collection.cc"
        "display/lvgl_display/lvgl_theme.cc"
        "display/lvgl_display/lvgl_font.cc"
        "display/lvgl_display/lvgl_image.cc"
        "display/lvgl_display/gif/lvgl_gif.cc"
        "display/lvgl_display/gif/gifdec.c"
        "display/lvgl_display/jpg/image_to_jpeg.cpp"
    EMBED_FILES
        "display/assets/screensaver_dial.rgb565"
    INCLUDE_DIRS
        "."
        "audio"
        "battery"
        "display"
        "display/lvgl_display"
        "display/lvgl_display/jpg"
        "input"
        "led"
        "power"
        "../common"
    REQUIRES
        main
        esp_driver_i2c
        esp_driver_spi
        esp_psram
        espressif__esp_lcd_st77916
        espressif__esp_codec_dev
        espressif__adc_battery_estimation
        espressif__button
        espressif__led_strip
        espressif__esp_lvgl_port
        espressif__esp_new_jpeg
        espressif__esp_image_effects
        lvgl__lvgl
    WHOLE_ARCHIVE
)

target_compile_definitions(${COMPONENT_LIB}
    PRIVATE
        BUILTIN_TEXT_FONT=${TUNTUN_BOARD_BUILTIN_TEXT_FONT}
        BUILTIN_ICON_FONT=${TUNTUN_BOARD_BUILTIN_ICON_FONT}
        LV_GIF_CACHE_DECODE_DATA=1
)

# LVGL 9.5 的解码器和缓存实现位于同一归档，直接加入板型组件以稳定链接顺序。
idf_build_get_property(build_components BUILD_COMPONENTS)
foreach(COMPONENT ${build_components})
    if(COMPONENT MATCHES "lvgl__lvgl")
        idf_component_get_property(LVGL_COMPONENT_PATH ${COMPONENT} COMPONENT_DIR)
        break()
    endif()
endforeach()
if(LVGL_COMPONENT_PATH)
    target_sources(${COMPONENT_LIB} PRIVATE
        "${LVGL_COMPONENT_PATH}/src/misc/cache/class/lv_cache_lru_rb.c"
        "${LVGL_COMPONENT_PATH}/src/misc/cache/instance/lv_image_cache.c"
        "${LVGL_COMPONENT_PATH}/src/misc/cache/instance/lv_image_header_cache.c"
    )
endif()
