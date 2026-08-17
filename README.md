# TuntunLife 多板型语音助手固件

基于 **阿慕希（MoveCall）Moji2.0 智能语音终端硬件**及其板级固件代码，结合
[xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) 官方代码演进而来，并在此基础上
开发了囤囤AI平台接入等特有功能。当前已验证 `movecall-moji2-esp32c5` 板型，构建系统
支持通过独立板型目录扩展其他 ESP32 芯片和硬件。

## 项目起源与致谢

- **硬件设计**：[Moji2.0 · 嘉立创开源硬件平台](https://oshwhub.com/movecall/moji2)
- **板级固件参考**：[MoveCall（阿慕希）· xiaozhi-esp32 · movecall-moji2-esp32c5 板型](https://github.com/MoveCall/xiaozhi-esp32/tree/main/main/boards/movecall-moji2-esp32c5)
- **基础框架**：[xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) v2.2.6

## 硬件

- ESP32-C5 N16R8
- ES8311 音频 Codec
- ST77916 360x360 QSPI LCD
- 单麦克风、扬声器、WS2812 状态灯

## 特有功能

- **囤囤AI平台接入**：语音绑定设备、语音设置天气位置、备忘录全链路语音操作、
  动态 MCP 工具下发与代理执行、主动通知（详见[囤囤AI平台接入](#囤囤ai平台接入)）
- **圆屏交互**：金属黑时钟表盘屏保（本地公历转农历）、天气与备忘录屏保卡片、
  自定义 MCP 清单表盘
- **对话与音频优化**：唤醒词打断、播放排空回调、任务优先级调度
- **多板型构建框架**：板型目录自包含契约（`main/boards/<board>/`），新增板型不改公共代码
- **安全发布流程**：Secure Boot V2 + Release 模式 Flash Encryption + 防降级，一键打包合并镜像
- **ESP-IDF 6.0.2 适配**：依赖与 LCD 结构体已按 IDF 6 迁移并真机验证

## 构建

要求 **ESP-IDF 6.0.2**（本项目已按 IDF 6.0.2 适配；使用 5.5.x 会因组件与结构体差异无法编译）。

```bash
python scripts/build.py --list-boards
python scripts/build.py movecall-moji2-esp32c5 build
```

开发构建固定输出到 `build/<board>/debug`。目标芯片和 sdkconfig 默认值由板型目录
自动提供，不需要先执行 `idf.py set-target`。也可以直接使用：

```bash
idf.py -B build/movecall-moji2-esp32c5/debug \
  -DBOARD_TYPE=movecall-moji2-esp32c5 build
```

新增板型只需增加 `main/boards/<board>/`，无需修改公共 CMake、Kconfig 或构建脚本。
板型目录契约见 [main/boards/README.md](main/boards/README.md)。

发布脚本固定启用 Secure Boot V2、Release 模式 Flash Encryption 和固件防降级。首次构建前需要在
项目根目录生成不会提交到 Git 的 ECDSA-256 签名私钥：

```bash
espsecure.py generate_signing_key --version 2 --scheme ecdsa256 secure_boot_signing_key.pem
```

该私钥必须离线备份，后续 OTA 固件必须继续使用同一私钥签名。发布固件首次启动会写入不可逆 eFuse，
只能在已经验证量产流程的设备上烧录；普通板型开发构建和开发烧录不会启用这些不可逆发布配置。
从旧版固件首次升级到安全发布固件时，必须烧录发布脚本生成的完整镜像；只执行应用 OTA 不会更新
Bootloader 和分区表，也不会启用 Secure Boot 与 Flash Encryption。首次完整烧录会清空原有 NVS 配置。

也可以通过板型构建脚本配置显示样式、资源和调试选项：

```bash
python scripts/build.py movecall-moji2-esp32c5 menuconfig
python scripts/build.py movecall-moji2-esp32c5 flash-monitor -p /dev/cu.usbmodemXXXX
```

硬件和固件设计说明位于 `docs/hardware` 与 `docs/firmware`。

## 囤囤AI平台接入

固件内置 MCP 工具 `self.tuntun.bind_device`。用户唤醒小智并明确提出“绑定囤囤AI平台”
后，设备会向 `CONFIG_TUNTUN_API_URL` 申请一次性绑定码，并在圆屏上持续显示。用户在囤囤AI
网页端的设备页面输入该绑定码后，设备会自动完成绑定并保存设备访问凭据。

再次要求绑定时，已保存完整设备凭据的设备会直接回答“设备已绑定”，不会重复生成绑定码。用户
要求解绑时，`self.tuntun.unbind_device` 只会提示登录囤囤AI后台完成解绑，固件不会执行语音解绑
或清除本地凭据。

设备绑定后可以通过 `self.tuntun.set_weather_location` 响应“把天气城市设置为上海市松江区”等
语音指令，也可以通过 `self.tuntun.set_weather_ip_auto` 响应“天气改为 IP 自动定位”。固定模式下
设备只提交城市或区县名称，行政区编码由后端通过高德接口解析并保存。

绑定完成后，设备会在屏保可见期间通过设备访问凭据同步天气和前 5 条未完成且未过期备忘录。首次进入屏保
时立即检查数据；网络临时断开或接口失败时继续显示最近一次成功内容。天气和备忘录缓存只保存在
运行内存中，不写入 NVS，避免周期刷新增加 Flash 擦写。

设备内置 `self.tuntun.create_memo`、`self.tuntun.query_memos`、`self.tuntun.update_memo`、
`self.tuntun.delete_memo` 和 `self.tuntun.get_memo_statistics` 五个异步工具，支持语音创建普通或定时
备忘录、按状态和今天、明天、本周、未到期范围查询具体内容，以及修改、删除和统计备忘录。创建、
修改或删除成功后会使旧的屏保备忘录缓存失效，对话结束回到屏保时立即显示最新数据。

屏保中每条备忘录固定显示三行：第一行使用方括号显示提醒时间，下面两行显示正文并在超长时省略。
今天显示 `[HH:mm]`，今年内非今天显示 `[MM-dd]`，非今年显示 `[yyyy-MM-dd]`；没有提醒时间时
显示 `[未设置时间]`。日期时间首行使用点阵粗体，正文保持普通字重。

语音创建备忘录前必须明确提醒日期和具体时间。用户只说事项日期时，小智先询问何时提醒；只说提醒
日期时继续询问几点。修改提醒时间遵循相同规则；只有用户明确表示不需要提醒时才保存为空提醒时间。

设备会在联网和绑定完成后获取并加载后台为当前设备发布的动态 MCP 工具，不再周期轮询或主动执行
自定义工具。小智实际调用 `custom.` 工具时，固件才使用设备凭据代理请求后台固定版本执行接口，并把
统一结果中的 `content` 返回模型。工具版本冲突时按需刷新清单；临时网络错误保留最近清单，设备认证
失效时清空动态工具。

固件另行内置 `self.tuntun.list_custom_mcp_tools`。用户询问“我有几个自定义工具包”“有哪些自定义
MCP”等问题时，小智会读取设备当前已经加载的动态工具快照，逐项播报去掉 `custom.` 前缀后的名称和
后台说明。圆屏同时切换到自定义 MCP 清单表盘，只保留金属背景、外圈和刻度。30px 标题固定显示在
顶部，25px 粗体内容每次只显示一个 MCP，并在圆角区域中垂直居中；存在多项时每 5 秒切换下一项。
语音播报结束后页面立即退出并恢复聊天界面，对话结束后仍按原逻辑进入普通屏保。

默认平台地址为 `https://api.tuntun.life`，通过 `CONFIG_TUNTUN_API_URL` 配置，可用板型构建脚本的
`menuconfig` 操作修改（接入自有后端时改为自建地址，或留空禁用绑定/天气/备忘录/通知/动态 MCP 能力）。
完整协议和状态处理见
[`docs/后端API接入说明.md`](docs/后端API接入说明.md)。

## 许可

代码采用 MIT 许可（见 [LICENSE](LICENSE)），保留上游
[xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) 的版权声明。
上游项目、[MoveCall（阿慕希）板级固件](https://github.com/MoveCall/xiaozhi-esp32/tree/main/main/boards/movecall-moji2-esp32c5)、
字体/表情素材与第三方组件的许可明细见 [CREDITS.md](CREDITS.md)。
