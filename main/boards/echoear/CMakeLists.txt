set(MOJI2_BOARD_DIR "${CMAKE_CURRENT_LIST_DIR}/../movecall-moji2-esp32c5")
set(MOJI2_DISPLAY_DIR "${MOJI2_BOARD_DIR}/display")

idf_component_register(
    SRCS
        "echoear.cc"
        "echoear_audio_codec.cc"
        "${MOJI2_BOARD_DIR}/input/button.cc"
        "${MOJI2_BOARD_DIR}/power/power_save_timer.cc"
        "${MOJI2_DISPLAY_DIR}/pwm_backlight.cc"
        "${MOJI2_DISPLAY_DIR}/display_assets.cc"
        "${MOJI2_DISPLAY_DIR}/lcd_display.cc"
        "${MOJI2_DISPLAY_DIR}/lcd_screensaver.cc"
        "${MOJI2_DISPLAY_DIR}/lcd_mcp_view.cc"
        "${MOJI2_DISPLAY_DIR}/lcd_binding_view.cc"
        "${MOJI2_DISPLAY_DIR}/lcd_chat_view.cc"
        "${MOJI2_DISPLAY_DIR}/fonts/font_puhui_time_64.cc"
        "${MOJI2_DISPLAY_DIR}/lvgl_display/lvgl_display.cc"
        "${MOJI2_DISPLAY_DIR}/lvgl_display/emoji_collection.cc"
        "${MOJI2_DISPLAY_DIR}/lvgl_display/lvgl_theme.cc"
        "${MOJI2_DISPLAY_DIR}/lvgl_display/lvgl_font.cc"
        "${MOJI2_DISPLAY_DIR}/lvgl_display/lvgl_image.cc"
        "${MOJI2_DISPLAY_DIR}/lvgl_display/gif/lvgl_gif.cc"
        "${MOJI2_DISPLAY_DIR}/lvgl_display/gif/gifdec.c"
    EMBED_FILES
        "${MOJI2_DISPLAY_DIR}/assets/screensaver_dial.rgb565"
    INCLUDE_DIRS
        "."
        "${MOJI2_BOARD_DIR}/input"
        "${MOJI2_DISPLAY_DIR}"
        "${MOJI2_DISPLAY_DIR}/lvgl_display"
        "${MOJI2_DISPLAY_DIR}/lvgl_display/jpg"
        "${MOJI2_BOARD_DIR}/power"
        "../common"
    REQUIRES
        main
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_i2s
        esp_driver_spi
        esp_driver_ledc
        esp_psram
        espressif__esp_lcd_st77916
        espressif__esp_codec_dev
        espressif__button
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
        ECHOEAR_DISABLE_JPEG=1
)

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
