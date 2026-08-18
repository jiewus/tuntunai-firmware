# 板型移植契约

`main/boards/` 下每个子目录都是一个自包含的硬件适配层。新增板型时，**不需要**修改项目根、公共
`main` 源码或构建/发布脚本。

## 必需文件

    main/boards/<board>/
    ├── CMakeLists.txt
    ├── config.json
    ├── dependencies.lock       首次 configure 后生成
    ├── idf_component.yml
    ├── sdkconfig.defaults
    ├── config.h
    ├── assets/                  加入默认资源分区的文件
    └── <board 实现>.cc

目录名使用小写字母、数字和连字符，同时也是传给 `BOARD_TYPE` 的值。

`config.json` 是 CMake 与 Python 工具链使用的唯一元数据来源：

    {
      "target": "esp32s3",
      "name": "Example ESP32-S3 Board",
      "component": "example-esp32s3",
      "device_model": 2,
      "builtin_text_font": "font_puhui_basic_30_4",
      "builtin_icon_font": "font_awesome_20_4",
      "emoji_collection": "twemoji_64",
      "partition_table": "partitions/v2/16m.csv"
    }

- `target`：ESP-IDF 芯片目标。
- `name`：对用户可见的板型/SKU 名称。
- `component`：必须等于板型目录名。
- `device_model`：正整数形式的后端设备型号编号。
- `builtin_text_font`、`builtin_icon_font`、`emoji_collection`：该板型选用的资源。
- `partition_table`：可选的、相对项目根的分区表路径，由板型元数据校验器检查。

`CMakeLists.txt` 注册该板型的所有实现源码。LCD、Codec、电池、触摸、摄像头等硬件相关源码应放在
这里，而不是公共的 `main/CMakeLists.txt`。组件注册时使用 `WHOLE_ARCHIVE`，以确保板型的
`DECLARE_BOARD` 工厂被保留。放在 `assets/` 下的文件会进入该板型生成的默认 `assets.bin`；
显示素材和编译后的字体应保留在板型目录内。

`idf_component.yml` 只负责该板型专用的依赖。需要更换 LCD 或音频 Codec 的板型因此可以直接
替换这些依赖，而无需修改公共组件清单。`main` 导出的公共接口提供网络、显示、音频、背光和 LED
契约；具体的 LCD/LVGL、Codec、按键、电池、背光和 LED 依赖归属板型清单。
首次 configure 成功后，Component Manager 会在清单旁生成 `dependencies.lock`。请提交该文件，
让每个目标都保留各自解析完成的依赖图；它无需在首次 configure 前存在。

`sdkconfig.defaults` 负责芯片、Flash、PSRAM、分区、唤醒词和板级资源限制。公共的协议/UI 默认值
仍保留在项目级 `sdkconfig.defaults`。

板型实现必须继承自 `Board` 或 `WifiBoard`，并使用 `DECLARE_BOARD` 注册**恰好一个**具体类型。

## 常用命令

列出已发现的板型：

    python scripts/build.py --list-boards

在隔离目录中构建某个板型：

    python scripts/build.py <board> build

也支持直接使用 ESP-IDF 命令：

    idf.py -B build/<board>/debug -DBOARD_TYPE=<board> build

目标芯片从 `config.json` 读取；使用这些按板型隔离的构建目录时，**不要**执行 `idf.py set-target`。

## 新增板型

1. 复制一个外设相近的现有板型目录。
2. 替换 `config.json`、GPIO 定义、组件依赖与 `sdkconfig.defaults`。
3. 在新目录内实现板型工厂与硬件初始化。
4. 如果是独立商用型号，在后端注册新的 `device_model`。这是服务端的型号注册，不是固件构建系统改动。
5. 运行 `python scripts/board_config.py list`，然后构建新板型。
