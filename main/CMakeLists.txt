# Movecall Moji2 ESP32-C5 产品组件构建入口。

# 项目自有源码清单；新增模块必须在此登记，否则不会参与编译和链接。
set(SOURCES
    "audio/audio_codec.cc"
    "audio/audio_service.cc"
    "audio/demuxer/ogg_demuxer.cc"
    "audio/codecs/es8311_audio_codec.cc"
    "audio/processors/audio_debugger.cc"
    "audio/processors/no_audio_processor.cc"
    "audio/wake_words/esp_wake_word.cc"
    "led/single_led.cc"
    "display/display.cc"
    "display/lcd_display.cc"
    "display/lvgl_display/lvgl_display.cc"
    "display/lvgl_display/emoji_collection.cc"
    "display/lvgl_display/lvgl_theme.cc"
    "display/lvgl_display/lvgl_font.cc"
    "display/lvgl_display/lvgl_image.cc"
    "display/lvgl_display/gif/lvgl_gif.cc"
    "display/lvgl_display/gif/gifdec.c"
    "display/lvgl_display/jpg/image_to_jpeg.cpp"
    "protocols/protocol.cc"
    "protocols/mqtt_protocol.cc"
    "protocols/websocket_protocol.cc"
    "boards/common/board.cc"
    "boards/common/wifi_board.cc"
    "boards/common/adc_battery_monitor.cc"
    "boards/common/backlight.cc"
    "boards/common/button.cc"
    "boards/common/power_save_timer.cc"
    "boards/common/press_to_talk_mcp_tool.cc"
    "boards/movecall-moji2-esp32c5/movecall_moji2_esp32c5.cc"
    "backend/backend_service.cc"
    "mcp_server.cc"
    "system_info.cc"
    "application.cc"
    "ota.cc"
    "settings.cc"
    "device_state_machine.cc"
    "assets.cc"
    "main.cc"
)

# 允许项目源码直接 include 的私有目录，第三方组件头文件由依赖系统自动注入。
set(INCLUDE_DIRS
    "."
    "audio"
    "audio/demuxer"
    "display"
    "display/lvgl_display"
    "display/lvgl_display/jpg"
    "protocols"
    "boards/common"
    "backend"
)

# 唯一板型、内置字体和表情资源选择，同时传给 C++ 宏和资源构建脚本。
# 360x360 圆屏对话字幕使用 30 px 中文字形源，界面中缩放为约 25 px 显示；
# 状态栏在 LCD 实现中单独使用 20 px 字体。
set(BOARD_TYPE "movecall-moji2-esp32c5")
set(BOARD_NAME "movecall-moji2-esp32c5")
set(BUILTIN_TEXT_FONT font_puhui_basic_30_4)
set(BUILTIN_ICON_FONT font_awesome_20_4)
set(DEFAULT_EMOJI_COLLECTION twemoji_64)

# 保留带 CONFIG 条件的板型赋值，发布脚本和 menuconfig 都依赖该声明识别板型。
if(CONFIG_BOARD_TYPE_MOVECALL_MOJI2_ESP32C5)
    set(BOARD_TYPE "movecall-moji2-esp32c5")
endif()

# 中文语言 JSON 会生成 lang_config.h；对应 OGG 文件直接嵌入 main 组件二进制。
set(LANG_DIR "zh-CN")
set(LANG_JSON "${CMAKE_CURRENT_SOURCE_DIR}/assets/locales/${LANG_DIR}/language.json")
set(LANG_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/assets/lang_config.h")
file(GLOB LANG_SOUNDS ${CMAKE_CURRENT_SOURCE_DIR}/assets/locales/${LANG_DIR}/*.ogg)
file(GLOB COMMON_SOUNDS ${CMAKE_CURRENT_SOURCE_DIR}/assets/common/*.ogg)

# 注册 main 组件。WHOLE_ARCHIVE 确保 DECLARE_BOARD 创建函数不会被链接器裁剪。
idf_component_register(
    SRCS ${SOURCES}
    EMBED_FILES ${LANG_SOUNDS} ${COMMON_SOUNDS}
    INCLUDE_DIRS ${INCLUDE_DIRS}
    WHOLE_ARCHIVE
    PRIV_REQUIRES
        esp_pm
        esp_psram
        esp_netif
        esp_driver_gpio
        esp_driver_spi
        esp_driver_i2c
        esp_driver_i2s
        esp_app_format
        app_update
        spi_flash
        console
        efuse
)

# 将板型和字体符号作为编译期宏传给业务源码。
target_compile_definitions(${COMPONENT_LIB}
    PRIVATE BOARD_TYPE=\"${BOARD_TYPE}\" BOARD_NAME=\"${BOARD_NAME}\"
    PRIVATE BUILTIN_TEXT_FONT=${BUILTIN_TEXT_FONT} BUILTIN_ICON_FONT=${BUILTIN_ICON_FONT}
)

# language.json 或生成器变化时自动重新生成中文语言头文件。
add_custom_command(
    OUTPUT ${LANG_HEADER}
    COMMAND python ${PROJECT_DIR}/scripts/gen_lang.py
            --language "${LANG_DIR}"
            --output "${LANG_HEADER}"
    DEPENDS ${LANG_JSON} ${PROJECT_DIR}/scripts/gen_lang.py
    COMMENT "Generating ${LANG_DIR} language config"
)
add_custom_target(lang_header ALL DEPENDS ${LANG_HEADER})

# 组件管理器会改变实际下载目录名，因此按组件名模式查询 ESP-SR 和字体路径。
idf_build_get_property(build_components BUILD_COMPONENTS)
function(find_component_by_pattern PATTERN COMPONENT_PATH_VAR)
    foreach(COMPONENT ${build_components})
        if(COMPONENT MATCHES "${PATTERN}")
            idf_component_get_property(COMPONENT_PATH ${COMPONENT} COMPONENT_DIR)
            set(${COMPONENT_PATH_VAR} "${COMPONENT_PATH}" PARENT_SCOPE)
            break()
        endif()
    endforeach()
endfunction()

find_component_by_pattern("espressif__esp-sr" ESP_SR_COMPONENT_PATH)
find_component_by_pattern("xiaozhi-fonts" XIAOZHI_FONTS_PATH)
find_component_by_pattern("lvgl__lvgl" LVGL_COMPONENT_PATH)

# LVGL 9.5 的图像解码器和缓存实现位于同一个静态归档中。干净链接时，链接器会先提取
# 解码器对象，但不会再次扫描归档中靠后的缓存对象，导致 lv_image_cache_* 未定义引用。
# 将两个原始缓存实现直接加入 main 组件，可稳定解决归档顺序问题，且不会修改第三方源码。
if(LVGL_COMPONENT_PATH)
    target_sources(${COMPONENT_LIB} PRIVATE
        "${LVGL_COMPONENT_PATH}/src/misc/cache/class/lv_cache_lru_rb.c"
        "${LVGL_COMPONENT_PATH}/src/misc/cache/instance/lv_image_cache.c"
        "${LVGL_COMPONENT_PATH}/src/misc/cache/instance/lv_image_header_cache.c"
    )
endif()

function(build_default_assets_bin)
    # 根据 sdkconfig 中的唤醒模型、字体和表情生成可直接烧录的 assets.bin。
    set(GENERATED_ASSETS_BIN "${CMAKE_BINARY_DIR}/generated_assets.bin")
    add_custom_command(
        OUTPUT ${GENERATED_ASSETS_BIN}
        COMMAND python ${PROJECT_DIR}/scripts/build_default_assets.py
                --sdkconfig "${SDKCONFIG}"
                --output "${GENERATED_ASSETS_BIN}"
                --builtin_text_font "${BUILTIN_TEXT_FONT}"
                --emoji_collection "${DEFAULT_EMOJI_COLLECTION}"
                --esp_sr_model_path "${ESP_SR_COMPONENT_PATH}/model"
                --xiaozhi_fonts_path "${XIAOZHI_FONTS_PATH}"
        DEPENDS ${SDKCONFIG} ${PROJECT_DIR}/scripts/build_default_assets.py
        COMMENT "Building default assets.bin"
        VERBATIM
    )
    add_custom_target(generated_default_assets ALL DEPENDS ${GENERATED_ASSETS_BIN})
    set(GENERATED_ASSETS_LOCAL_FILE ${GENERATED_ASSETS_BIN} PARENT_SCOPE)
endfunction()

function(get_assets_local_file assets_source assets_local_file_var)
    # 统一解析 URL、绝对路径和相对 main/ 的自定义资源文件，并校验文件存在。
    if(assets_source MATCHES "^https?://")
        get_filename_component(ASSETS_FILENAME "${assets_source}" NAME)
        set(ASSETS_LOCAL_FILE "${CMAKE_BINARY_DIR}/${ASSETS_FILENAME}")
        if(NOT EXISTS ${ASSETS_LOCAL_FILE})
            file(DOWNLOAD ${assets_source} ${ASSETS_LOCAL_FILE} STATUS DOWNLOAD_STATUS)
            list(GET DOWNLOAD_STATUS 0 STATUS_CODE)
            if(NOT STATUS_CODE EQUAL 0)
                message(FATAL_ERROR "Failed to download ${assets_source}")
            endif()
        endif()
    elseif(IS_ABSOLUTE "${assets_source}")
        set(ASSETS_LOCAL_FILE "${assets_source}")
    else()
        set(ASSETS_LOCAL_FILE "${CMAKE_CURRENT_SOURCE_DIR}/${assets_source}")
    endif()
    if(NOT EXISTS ${ASSETS_LOCAL_FILE})
        message(FATAL_ERROR "Assets file not found: ${ASSETS_LOCAL_FILE}")
    endif()
    set(${assets_local_file_var} ${ASSETS_LOCAL_FILE} PARENT_SCOPE)
endfunction()

# 只有分区表确实包含 assets 分区时，才把默认或自定义资源挂到 flash 目标。
partition_table_get_partition_info(size "--partition-name assets" "size")
partition_table_get_partition_info(offset "--partition-name assets" "offset")
if("${size}" AND "${offset}")
    if(CONFIG_FLASH_DEFAULT_ASSETS)
        build_default_assets_bin()
        esptool_py_flash_to_partition(flash "assets" "${GENERATED_ASSETS_LOCAL_FILE}")
    elseif(CONFIG_FLASH_CUSTOM_ASSETS)
        get_assets_local_file("${CONFIG_CUSTOM_ASSETS_FILE}" ASSETS_LOCAL_FILE)
        esptool_py_flash_to_partition(flash "assets" "${ASSETS_LOCAL_FILE}")
    endif()
endif()
