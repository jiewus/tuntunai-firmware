# ESP-IDF 要求先声明最低 CMake 版本，再包含 project.cmake；不要调整这两步的顺序。
cmake_minimum_required(VERSION 3.16)

# ESP-IDF 结构体经常使用指定成员初始化，未显式填写的字段会自动清零，因此关闭该告警。
add_compile_options(-Wno-missing-field-initializers)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# 只构建 main 及其依赖，避免把 ESP-IDF 中未使用的组件链接进固件。
idf_build_set_property(MINIMAL_BUILD ON)

# 固件版本会写入应用描述符，并用于 OTA 版本比较和发布包命名。
set(PROJECT_VER "1.0.0")
project(xiaozhi)
