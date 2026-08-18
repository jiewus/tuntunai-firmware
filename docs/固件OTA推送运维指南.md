# 固件 OTA 推送运维指南

本文档说明如何把新固件推送给**所有已联网设备**（OTA 批量升级），包括固件如何检查更新、
如何发布、如何托管镜像，以及回滚注意事项。

> 适用对象：拥有设备在网、需要灰度/全量推送新固件的管理者。
> 配套：本项目基于 `xiaozhi-esp32` v2.2.6 的 OTA 机制 + Tuntun 定制，已适配 ESP-IDF 6.0.2。

---

## 1. OTA 工作原理（设备侧）

### 1.1 何时检查更新

设备**每次开机**（或重启后进入主流程）调用一次 `CheckNewVersion()`：
- 位置：`main/app/application.cc::CheckNewVersion()`
- 向 `CONFIG_OTA_URL` 配置的版本检查接口（开源默认指向公开 OTA 服务，例如
  `https://your-ota.example.com/xiaozhi/ota/`，也可通过 NVS 键 `wifi/ota_url` 覆盖）发 **POST**，
  请求体是设备系统信息 JSON（`Board::GetSystemInfoJson()`）。
- 失败最多重试 10 次、指数退避（初始 10s）。成功后若发现新版本则立即进入升级；无新版本则确认当前分区有效并进入正常流程。

### 1.2 服务器返回协议

版本检查接口响应是一个 JSON，设备只关心其中的 `firmware` 字段：

```json
{
  "firmware": {
    "version": "1.1.0",
    "url": "https://your-ota.host/xiaozhi/1.1.0/movecall-moji2-esp32c5.bin",
    "force": 0
  },
  // 以下可选：activation / mqtt / websocket / server_time（设备会解析）
  "activation": { ... },
  "mqtt": { ... },
  "websocket": { ... },
  "server_time": { "timestamp": ..., "timezone_offset": ... }
}
```

设备判断逻辑（`ota.cc`）：
- `has_new_version_ = IsNewVersionAvailable(current_version, firmware.version)`——**按数字段逐段比较**（例如 `1.1.0` > `1.0.9`）。
- 或 `firmware.force == 1` 时**忽略版本比较，强制安装**。
- 仅在 `version` 和 `url` 都是字符串时才触发升级。

### 1.3 升级过程

- 从 `firmware.url` 下载镜像 → 写入 `ota_0`/`ota_1` 非运行槽 → `esp_ota_set_boot_partition()` 切换到新槽 → 重启。
- 分区表为**双 OTA 槽**（`partitions/v2/16m.csv`：`ota_0`+`ota_1`，各 4032K）+ `assets`（8M 资源分区）。
- 重启后若新镜像校验失败或启动异常，bootloader 自动回滚到旧槽（`MarkCurrentVersionValid()` 在确认正常启动后才取消回滚）。

---

## 2. 发布一个新固件（本地操作）

### 2.1 更新版本号

固件版本号取自**工程版本 `PROJECT_VER`**，位于根 `CMakeLists.txt`：

```cmake
set(PROJECT_VER "1.1.0")   # ← 这里写新版本号
```

> 版本号必须是点分数字（如 `1.1.0`），设备按数字段比较。**每发布一版都要递增版本号**，否则设备认为无新版本。

### 2.2 构建并打包发布镜像

```bash
# 加载 ESP-IDF 6.0.2 环境
source ~/.espressif/v6.0.2/esp-idf/export.sh

# 构建正式发布版（非安全加固）并打包
python scripts/release.py movecall-moji2-esp32c5
```

`release.py` 会：
1. 用 `sdkconfig.defaults + 板型/sdkconfig.defaults` 以 **release** 模式构建（非安全加固，无 Secure Boot / Flash 加密 / 防回滚）；
2. `idf.py merge-bin` 合并出完整烧录镜像 `tuntun-binary.bin`；
3. 打包成 `releases/v<PROJECT_VER>_movecall-moji2-esp32c5.zip`。

**关键区分**：
- `tuntun-binary.bin`（merge-bin）→ 用于**完整烧录**（含 bootloader + 分区表 + app + 资源）。
- 已联网设备走 **OTA 只需要应用镜像** `build/movecall-moji2-esp32c5/release/xiaozhi.bin`。OTA 不会更新 bootloader / 分区表。

> 详见 `README.md` 的"发布"一节。

---

## 3. 托管固件镜像（放哪里）

1. 把发布产物中的**应用镜像**（`build/<board>/release/xiaozhi.bin`）上传到任意 **HTTPS 可达、支持 Range/大文件下载** 的静态文件服务器或对象存储（如你已有 OTA 服务的对象存储、S3、OSS、七牛等）。
2. 记下该文件的**完整 HTTPS URL**，这就是要在 `firmware.url` 里填的值。
   - 外部硬件接入时，也建议用一个固定/可预测的 URL 模板，便于批量生成。

---

## 4. 推送：让所有设备收到新版本

推送不需要逐个操作设备——只要让**版本检查接口返回新版本**，设备开机即会自动升级。两种方式：

### 方式 A：直接改 OTA 服务返回（最简单，设备开机自升）

如果你的 OTA 服务（默认 `CONFIG_OTA_URL`；自建或第三方均可）是按设备上报的版本动态返回 `firmware` 的，那么**只需在服务端数据库/配置里把目标设备组的新版本 URL 配好**，无需改设备。设备下次开机检查到 `firmware.version` 高于当前即自动下载升级。

关键字段就是你希望这批设备统一升到的版本号与 `firmware.url`。

### 方式 B：强制升级（忽略版本比较）

当你需要**无条件强制**把某批设备刷到指定版本（例如修复安全漏洞、回退错误版本）时，让服务端返回：

```json
{ "firmware": { "version": "1.1.0", "url": "https://.../xiaozhi.bin", "force": 1 } }
```

设备看到 `force == 1`，即使版本号不高也会强制执行升级。

> 想"给所有设备"（全量）就返回同一份新版本；想"灰度/分批"就按设备上报的 `uuid`/`mac_address`/`version` 有选择地返回新版本。

---

## 5. 推送策略建议

| 场景 | 做法 |
| --- | --- |
| **开发/自测** | 用 `python scripts/build.py xxx flash` 直接 USB 烧录，不走 OTA。 |
| **小范围灰度** | OTA 服务按 `uuid`/`mac` 白名单返回新版本给部分设备。 |
| **全量推送** | 确认灰度无异常后，OTA 服务对全部设备返回新版本。 |
| **紧急修复/强制** | `force:1` + 新版本 URL，等设备下次开机升级。 |
| **回滚错误版本** | 在服务端把 `firmware.version` 指回旧版本号 + 旧镜像 URL（设备会尝试降级）；或 `force:1` 指到目标旧版本。 |

**高峰注意**：全量推送时所有设备可能同时开机/同时拉镜像，建议用 CDN/对象存储静态 URL + OTA 服务限流或错峰，避免源站打爆。

---

## 5.1 给"自己的设备"直接烧录 merge-bin（不走 OTA）

如果你要更新的是**你自己手上的设备**（或你想跳过 OTA、直接重写主要分区），可以不走 OTA，直接用 merge-bin **完整镜像**烧录。merge-bin 是 `idf.py merge-bin` 把 bootloader + 分区表 + app + assets 等**按各自偏移合并成一份镜像**，并用 `0xFF` 填充分区之间的空隙。从地址 `0x0` 烧录时，镜像覆盖范围内的 NVS 也会被清空。

### 步骤

**第一步：生成当前版本的 merge-bin**

```bash
# 在固件仓库根目录执行（monorepo 中即 ESP32-C5.Firmware/）
source ~/.espressif/v6.0.2/esp-idf/export.sh

# 方式一（推荐，release.py 自动构建、merge 并打包）：
python scripts/release.py movecall-moji2-esp32c5
# 产物：build/movecall-moji2-esp32c5/release/tuntun-binary.bin
#       以及 releases/v<PROJECT_VER>_movecall-moji2-esp32c5.zip

# 方式二（手动，开发/调试用）：
python scripts/build.py movecall-moji2-esp32c5 build
idf.py -B build/movecall-moji2-esp32c5/debug \
  -DBOARD_TYPE=movecall-moji2-esp32c5 merge-bin -o merged.bin
```

> 注意：`release.py` 与 `build.py` 产出的都是**非安全加固**镜像（无 Secure Boot / Flash 加密 / 防回滚）；
> 区别在于 `release.py` 会按发布偏移 merge 并打包成完整镜像，`build.py` 是开发调试构建。两者均可直接烧录与随时重刷。

**第二步：确认串口并烧录**

```bash
ls /dev/cu.usbmodem*          # macOS 端口
python -m esptool --chip esp32c5 -p /dev/cu.usbmodem83101 -b 460800 \
  --before default-reset --after hard-reset write-flash \
  0x0 build/movecall-moji2-esp32c5/release/tuntun-binary.bin
```

> merge-bin 从 `0x0` 开始烧，会重写 bootloader、分区表、固件和资源分区，并以镜像中的 `0xFF` 填充值**清空原有 NVS 配置**（Wi-Fi、绑定、音量等）。烧完需重新配网、重新绑定。请务必确认该设备就是你要更新的那台，避免误刷线上设备。

> `python scripts/build.py movecall-moji2-esp32c5 flash -p <串口>` 采用分区式烧录，不写入 `0x11000` 的 NVS 分区，因此会保留已有配置；它与从 `0x0` 烧录 merge-bin 的行为不同。

### 可选：只烧 App（保留配置，介于 OTA 和 merge-bin 之间）

如果只是想快速更新应用代码、又不想走 OTA、且**想保留 NVS 设置**，可单独烧 App 分区（从 `0x20000`）而不碰 bootloader/分区/NVS：

```bash
python -m esptool --chip esp32c5 -p /dev/cu.usbmodem83101 -b 460800 \
  --before default-reset --after hard-reset write-flash \
  0x20000 build/movecall-moji2-esp32c5/debug/xiaozhi.bin
```

> 这只写应用镜像，保留 bootloader/分区/NVS，适合日常迭代，风险比整片 merge-bin 小。

---

## 6. 常见问题排查

| 现象 | 可能原因 | 处理 |
| --- | --- | --- |
| 设备显示"已是最新"但没升级 | 服务端 `firmware.version` ≤ 设备当前版本 | 服务端填更高版本号，或 `force:1` |
| 设备反复失败/拉不到镜像 | `firmware.url` 不可达、非 HTTPS、或静态服务不支持 | 用浏览器直接访问该 URL，确认能下载、证书有效 |
| 升级后开不了机 | 镜像损坏 / 校验失败 | bootloader 自动回滚到旧槽；确认上传的镜像完整、URL 正确后再升级 |
| OTA 不更新 bootloader/分区 | OTA 只写 App 分区（设计如此） | 结构变更（bootloader/分区）请用完整镜像（merge-bin）烧录 |
| 版本号 `1.1.0-beta` 无法比较 | 数字段比较遇到非数字 | 版本号只含数字和点号 |

---

## 7. 相关代码/配置速查

| 项 | 位置 |
| --- | --- |
| OTA 触发（开机） | `main/app/application.cc::CheckNewVersion()` |
| OTA 请求/解析/升级 | `main/xiaozhi/provisioning/ota.{h,cc}` |
| 设备上报 JSON | `main/boards/common/board.cc::GetSystemInfoJson()` |
| OTA 接口地址 | Kconfig `OTA_URL`（`main/Kconfig.projbuild`）；NVS 覆盖键 `wifi/ota_url` |
| 版本号 | 根 `CMakeLists.txt` `set(PROJECT_VER ...)` |
| 发布打包 | `scripts/release.py` |
| 分区表（双 OTA） | `partitions/v2/16m.csv` |
