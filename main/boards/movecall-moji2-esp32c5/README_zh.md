# Tuntun Moji2 ESP32-C5 板型

本目录完整描述 `movecall-moji2-esp32c5` 的 ESP32-C5 目标、GPIO、LCD、
ES8311 Codec、电池检测、组件依赖和 sdkconfig 默认值。硬件设计参考：
[立创开源硬件平台](https://oshwhub.com/movecall/moji2)。
圆屏界面、表盘资源、专用字体和 ES8311 实现也只保存在本目录，不进入公共源码。

从固件根目录构建：

```bash
python scripts/build.py movecall-moji2-esp32c5 build
```

配置、清理、烧录和监视均通过同一脚本执行：

```bash
python scripts/build.py movecall-moji2-esp32c5 menuconfig
python scripts/build.py movecall-moji2-esp32c5 fullclean
python scripts/build.py movecall-moji2-esp32c5 flash-monitor -p /dev/cu.usbmodemXXXX
```

目标芯片由 `config.json` 自动设置，不要执行 `idf.py set-target`。新增板型时复制
本目录并替换其中的板级文件，公共契约见相邻的 [README.md](../README.md)。
