# ESP-IDF 要求先声明最低 CMake 版本，再包含 project.cmake；不要调整这两步的顺序。
cmake_minimum_required(VERSION 3.16)

# ESP-IDF 结构体经常使用指定成员初始化，未显式填写的字段会自动清零，因此关闭该告警。
add_compile_options(-Wno-missing-field-initializers)

# 板型完全由 main/boards/<board>/config.json 描述。默认值只用于保持无参数构建兼容；
# 新板型使用 -DBOARD_TYPE=<directory-name> 选择，不需要修改本文件。
if(NOT BOARD_TYPE)
    if(DEFINED ENV{TUNTUN_BOARD} AND NOT "$ENV{TUNTUN_BOARD}" STREQUAL "")
        set(BOARD_TYPE "$ENV{TUNTUN_BOARD}")
    else()
        set(BOARD_TYPE "movecall-moji2-esp32c5")
    endif()
endif()

if(DEFINED PYTHON AND EXISTS "${PYTHON}")
    set(BOARD_CONFIG_PYTHON "${PYTHON}")
else()
    find_program(BOARD_CONFIG_PYTHON NAMES python3 python)
    if(NOT BOARD_CONFIG_PYTHON)
        message(FATAL_ERROR "Python 3 is required to load board metadata")
    endif()
endif()

set(BOARD_CONFIG_CMAKE "${CMAKE_BINARY_DIR}/tuntun_board.cmake")
execute_process(
    COMMAND "${BOARD_CONFIG_PYTHON}"
            "${CMAKE_CURRENT_LIST_DIR}/scripts/board_config.py"
            emit-cmake "${BOARD_TYPE}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
    OUTPUT_FILE "${BOARD_CONFIG_CMAKE}"
    ERROR_VARIABLE BOARD_CONFIG_ERROR
    RESULT_VARIABLE BOARD_CONFIG_RESULT
)
if(NOT BOARD_CONFIG_RESULT EQUAL 0)
    string(STRIP "${BOARD_CONFIG_ERROR}" BOARD_CONFIG_ERROR)
    message(FATAL_ERROR "Invalid board '${BOARD_TYPE}': ${BOARD_CONFIG_ERROR}")
endif()
include("${BOARD_CONFIG_CMAKE}")

if(IDF_TARGET AND NOT IDF_TARGET STREQUAL TUNTUN_BOARD_TARGET)
    message(FATAL_ERROR
        "Board '${TUNTUN_BOARD_TYPE}' requires target '${TUNTUN_BOARD_TARGET}', "
        "but IDF_TARGET is '${IDF_TARGET}'. Use a separate build directory for each board.")
endif()
set(IDF_TARGET "${TUNTUN_BOARD_TARGET}")

# 只发现并构建当前板型组件。板级私有驱动及组件依赖均留在该目录。
list(APPEND EXTRA_COMPONENT_DIRS "${TUNTUN_BOARD_DIR}")
set(COMPONENTS main "${TUNTUN_BOARD_COMPONENT}")

# 每个构建目录保存独立 sdkconfig，避免 C5、S3 等目标互相污染项目根配置。
if(NOT SDKCONFIG)
    set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
endif()
if(NOT SDKCONFIG_DEFAULTS)
    set(SDKCONFIG_DEFAULTS
        "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults;${TUNTUN_BOARD_SDKCONFIG_DEFAULTS}")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Component Manager lock files contain the target and direct board dependencies,
# so each board owns its lock file alongside its manifest.
idf_build_set_property(
    DEPENDENCIES_LOCK "${TUNTUN_BOARD_DIR}/dependencies.lock")

# 固件版本会写入应用描述符，并用于 OTA 版本比较和发布包命名。
set(PROJECT_VER "1.0.0")
project(xiaozhi)
