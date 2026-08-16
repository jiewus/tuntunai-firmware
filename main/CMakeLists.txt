# TuntunLife 所有板型共享的产品组件构建入口。

# 项目自有源码清单；新增模块必须在此登记，否则不会参与编译和链接。
set(SOURCES
    "audio/audio_codec.cc"
    "audio/audio_service.cc"
    "audio/demuxer/ogg_demuxer.cc"
    "audio/processors/audio_debugger.cc"
    "audio/processors/no_audio_processor.cc"
    "audio/wake_words/esp_wake_word.cc"
    "display/display.cc"
    "xiaozhi/protocol/protocol.cc"
    "xiaozhi/protocol/protocol_message.cc"
    "xiaozhi/protocol/mqtt_protocol.cc"
    "xiaozhi/protocol/websocket_protocol.cc"
    "xiaozhi/client/xiaozhi_client.cc"
    "xiaozhi/provisioning/ota.cc"
    "mcp/mcp_server.cc"
    "mcp/tools/press_to_talk_mcp_tool.cc"
    "boards/common/board.cc"
    "boards/common/wifi_board.cc"
    "tuntun/backend/backend_service.cc"
    "tuntun/backend/backend_dynamic_mcp.cc"
    "tuntun/backend/backend_memo.cc"
    "tuntun/backend/backend_custom_reminder.cc"
    "tuntun/backend/backend_weather.cc"
    "tuntun/backend/backend_binding.cc"
    "tuntun/backend/backend_notification.cc"
    "system/system_info.cc"
    "system/settings.cc"
    "app/application.cc"
    "app/application_conversation.cc"
    "app/application_lifecycle.cc"
    "app/device_state_machine.cc"
    "assets.cc"
    "main.cc"
)

# 允许项目源码直接 include 的私有目录，第三方组件头文件由依赖系统自动注入。
set(INCLUDE_DIRS
    "."
    "audio"
    "audio/demuxer"
    "display"
    "boards/common"
)

# 中文语言 JSON 会生成 lang_config.h；对应 OGG 文件直接嵌入 main 组件二进制。
set(LANG_DIR "zh-CN")
set(LANG_JSON "${CMAKE_CURRENT_SOURCE_DIR}/assets/locales/${LANG_DIR}/language.json")
set(LANG_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/assets/lang_config.h")
file(GLOB LANG_SOUNDS ${CMAKE_CURRENT_SOURCE_DIR}/assets/locales/${LANG_DIR}/*.ogg)
file(GLOB COMMON_SOUNDS ${CMAKE_CURRENT_SOURCE_DIR}/assets/common/*.ogg)

# 注册共享 main 组件。具体板型作为独立 WHOLE_ARCHIVE 组件参与最终链接。
idf_component_register(
    SRCS ${SOURCES}
    EMBED_FILES
        ${LANG_SOUNDS}
        ${COMMON_SOUNDS}
    INCLUDE_DIRS ${INCLUDE_DIRS}
    # 板型组件会直接包含 Board、Display、AudioCodec 等共享头文件。这里公开这些头文件
    # 暴露的依赖，确保新板型只需声明自身 LCD、Codec、电池等硬件专属组件。
    REQUIRES
        78__esp-ml307
        78__xiaozhi-fonts
        espressif__esp_audio_codec
        espressif__esp_audio_effects
        espressif__esp-sr
        cjson
        nvs_flash
        esp_driver_gpio
        esp_driver_i2s
        esp_pm
        spi_flash
    PRIV_REQUIRES
        esp_psram
        esp_netif
        esp_driver_spi
        esp_driver_i2c
        esp_app_format
        app_update
        console
        efuse
        esp-wifi-connect
)

# 将板型标识作为编译期宏传给共享业务源码。
target_compile_definitions(${COMPONENT_LIB}
    PRIVATE
        BOARD_TYPE=\"${TUNTUN_BOARD_TYPE}\"
        BOARD_NAME=\"${TUNTUN_BOARD_NAME}\"
        BOARD_DEVICE_MODEL=${TUNTUN_BOARD_DEVICE_MODEL}
)

# language.json 或生成器变化时自动重新生成中文语言头文件。
add_custom_command(
    OUTPUT ${LANG_HEADER}
    COMMAND python ${PROJECT_DIR}/scripts/gen_lang.py
            --language "${LANG_DIR}"
            --output "${LANG_HEADER}"
    DEPENDS ${LANG_JSON} ${LANG_SOUNDS} ${COMMON_SOUNDS} ${PROJECT_DIR}/scripts/gen_lang.py
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
file(GLOB_RECURSE TUNTUN_BOARD_ASSET_FILES CONFIGURE_DEPENDS
    "${TUNTUN_BOARD_ASSETS_DIR}/*")

function(build_default_assets_bin)
    # 根据 sdkconfig 和当前板型资源目录生成可直接烧录的 assets.bin。
    set(GENERATED_ASSETS_BIN "${CMAKE_BINARY_DIR}/generated_assets.bin")
    add_custom_command(
        OUTPUT ${GENERATED_ASSETS_BIN}
        COMMAND python ${PROJECT_DIR}/scripts/build_default_assets.py
                --sdkconfig "${SDKCONFIG}"
                --output "${GENERATED_ASSETS_BIN}"
                --builtin_text_font "${TUNTUN_BOARD_BUILTIN_TEXT_FONT}"
                --emoji_collection "${TUNTUN_BOARD_EMOJI_COLLECTION}"
                --esp_sr_model_path "${ESP_SR_COMPONENT_PATH}/model"
                --xiaozhi_fonts_path "${XIAOZHI_FONTS_PATH}"
                --extra_files "${TUNTUN_BOARD_ASSETS_DIR}"
        DEPENDS ${SDKCONFIG} ${PROJECT_DIR}/scripts/build_default_assets.py
                ${TUNTUN_BOARD_ASSET_FILES}
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
        add_dependencies(flash generated_default_assets)
    elseif(CONFIG_FLASH_CUSTOM_ASSETS)
        get_assets_local_file("${CONFIG_CUSTOM_ASSETS_FILE}" ASSETS_LOCAL_FILE)
        esptool_py_flash_to_partition(flash "assets" "${ASSETS_LOCAL_FILE}")
    endif()
endif()
