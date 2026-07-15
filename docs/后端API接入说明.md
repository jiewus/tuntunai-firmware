# 后端 API 接入说明

## 1. 当前状态

固件已经接入囤囤管家的设备绑定和屏保天气接口。当前实现范围如下：

| 能力 | 当前状态 |
|---|---|
| 语音申请设备绑定码 | 已实现 |
| 圆屏绑定码页面 | 已实现 |
| 绑定状态轮询 | 已实现 |
| 设备访问 Token 领取与 NVS 持久化 | 已实现 |
| Web 端输入绑定码 | 已由 Web 项目的设备页面实现 |
| 屏保天气查询与内存缓存 | 已实现 |
| 断网保留旧天气与重连补刷 | 已实现 |
| 备忘录查询 | 暂时显示占位内容 |
| 动态 MCP 工具清单同步与代理执行 | 已实现 |
| 主动提醒 | 尚未接入 |

设备绑定不影响小智原有的配网、OTA、ASR、大模型、TTS 和语音协议。

## 2. 设备 MCP 工具

### 2.1 设备绑定

固件启动时注册以下异步 MCP 工具：

```text
self.tuntun.bind_device
```

工具没有输入参数。只有用户明确提出绑定或连接囤囤管家平台时，大模型才应调用该工具。
例如用户可以说：

```text
绑定囤囤管家平台
```

工具返回后，小智会告知用户前往囤囤管家网页端完成绑定；绑定码只显示在设备屏幕上，避免
大模型改写或错误播报数字。

调用绑定工具时，固件先检查 NVS 中是否同时存在 `device_id` 和 `device_token`。两个凭据均存在
表示设备已经绑定，此时不访问网络、不创建绑定任务、不生成新绑定码，直接返回“当前设备已经绑定
囤囤管家”。该判断优先于网络状态，因此设备离线时也不会把已绑定设备误报为无法绑定。

固件另行注册无参数工具：

```text
self.tuntun.unbind_device
```

用户明确要求解绑或删除当前设备时，小智通过该工具告知用户登录囤囤管家后台，在设备管理中完成
解绑。固件不提供语音解绑能力，不调用解绑接口，也不会通过语音指令清除 NVS 中的设备凭据。

### 2.2 语音设置天气位置

固件同时注册：

```text
self.tuntun.set_weather_location
```

用户说“把天气城市设置为上海市松江区”等明确指令时，大模型把完整城市或区县名称写入
`location_name` 参数。工具使用设备 Token 调用：

```http
PUT /api/device/weather/settings
Authorization: Bearer {device_token}
Content-Type: application/json

{
  "location_mode": 1,
  "location_name": "上海市松江区"
}
```

固件和 Web 页面均不提交高德行政区编码。后端负责调用高德地理编码接口，把名称解析为标准位置
名称和内部 `adcode` 后保存。语音设置成功后，固件立即清除运行内存中的旧天气快照；对话结束进入
屏保时会直接请求新位置天气，不继续显示旧城市缓存。

用户说“天气改为 IP 自动定位”“根据当前位置自动获取天气”等指令时，大模型调用无参数工具：

```text
self.tuntun.set_weather_ip_auto
```

工具使用同一个设备天气设置接口提交：

```http
PUT /api/device/weather/settings
Authorization: Bearer {device_token}
Content-Type: application/json

{
  "location_mode": 2,
  "location_name": null
}
```

后端保存 IP 自动定位模式并清空原固定城市和行政区编码。固件同时清除旧天气快照；随后请求天气时，
后端使用设备请求自身的公网 IP 定位，不使用浏览器、服务器或反向代理的 IP。

## 3. API 配置

平台根地址由 Kconfig 配置项控制：

```text
CONFIG_TUNTUN_API_URL="https://api.tuntun.life"
```

默认值位于 `sdkconfig.defaults`。需要切换环境时，通过 `idf.py menuconfig` 修改
`TuntunLife API URL`，不要在业务代码中硬编码不同环境地址。固件会去除根地址末尾的斜杠，
再拼接固定接口路径。

## 4. 绑定协议

### 4.1 申请绑定码

设备调用：

```http
POST /api/device/binding/request
Content-Type: application/json
```

请求包含稳定硬件标识、设备型号、客户端 UUID 和当前固件版本。成功后平台返回短绑定码、
私有绑定会话 Token 和过期时间。固件只显示短绑定码，会话 Token 只保存在 NVS 中。

### 4.2 用户绑定

用户登录囤囤管家网页端，在设备页面输入屏幕上的短绑定码。Web 调用：

```http
POST /api/user/devices/bind
Authorization: Bearer {user_access_token}
```

该请求由 Web 项目负责，固件不参与用户 Token 的传输。

### 4.3 状态轮询

固件每 3 秒调用：

```http
GET /api/device/binding/status
Authorization: Bearer {binding_session_token}
```

状态为 `Pending` 时继续等待；网络断开时暂停 HTTP 请求，网络恢复后使用原会话继续轮询。
设备重启后也会从 NVS 恢复尚未完成的绑定会话。

### 4.4 完成绑定

状态变为 `Bound` 后，固件调用：

```http
POST /api/device/binding/complete
Authorization: Bearer {binding_session_token}
Content-Type: application/json
```

平台返回设备 ID 和设备访问 Token。固件先把最终凭据写入 NVS，再清除临时绑定码和绑定会话
Token，显示“绑定成功”3 秒后关闭绑定页面。

## 5. 屏幕行为

绑定页面是独立的全屏 LVGL 覆盖层，使用与表盘一致的金属黑配色。页面分为标题、大号绑定码
和操作说明三部分。申请期间、成功、过期或失败状态不显示空的绑定码框，只显示状态文字。

绑定页面创建一次后重复使用，不在轮询过程中反复创建控件。页面层级高于普通对话和屏保，
因此语音会话结束触发屏保时，绑定码仍保持可见。只有绑定成功、会话过期或绑定失败时，
绑定任务才会关闭页面并露出下方原有界面。

## 6. NVS 数据

绑定状态使用 `tuntun_api` 命名空间：

| 键 | 内容 | 生命周期 |
|---|---|---|
| `bind_code` | 当前短绑定码 | 绑定完成、过期或失败后删除 |
| `bind_token` | 私有绑定会话 Token | 绑定完成、过期或失败后删除 |
| `device_id` | 平台设备 ID | 绑定完成后长期保存 |
| `device_token` | 设备访问 Token | 绑定完成后长期保存 |

日志不会输出 HTTP 请求正文、响应正文、绑定码、会话 Token 或设备 Token。绑定页面也不会显示
会话 Token 和设备 Token。

## 7. 失败和恢复

- 设备未联网时，MCP 工具直接返回可识别的失败结果，不创建绑定任务。
- 申请接口失败时，页面显示安全错误说明，用户可以稍后重新发起语音绑定。
- 轮询期间临时网络失败时保留绑定码，每 5 秒重试，不创建多个并行任务。
- 网络断开时保留会话并暂停请求，恢复联网后继续轮询。
- 会话认证失败、过期或绑定失败时清除临时 NVS 数据，并提示用户重新申请。
- 固件只允许一个绑定任务运行；重复调用工具时复用当前绑定码和页面。

## 8. 屏保天气同步

屏保显示时，固件使用 NVS 中的 `device_token` 调用：

```http
GET /api/device/weather
Authorization: Bearer {device_token}
Accept: application/json
```

成功响应的 `data` 必须包含以下结构：

```json
{
  "location_code": "310117",
  "location_name": "松江区",
  "temperature": 28,
  "weather": "多云",
  "low_temperature": 24,
  "high_temperature": 31,
  "updated_at": "2026-07-15T22:30:00+08:00"
}
```

固件仅在屏保可见时创建天气 HTTP 任务：

- 进入屏保时立即检查并按需刷新。
- 成功数据在 RAM 中缓存 30 分钟，不写入 NVS。
- 每 5 分钟由轻量定时器检查缓存是否过期，检查本身不执行网络请求。
- 退出屏保后不创建新请求；已发出的请求可以完成并更新内存缓存，但不更新隐藏的界面。
- 网络临时断开或后端请求失败时保留最近一次成功天气；没有历史缓存时显示安全状态文本。
- 网络恢复且屏保可见时会再次检查并按需补刷。
- 设备 Token 缺失时显示“请先绑定囤囤管家”；返回 `401` 或 `403` 且没有历史缓存时显示
  “设备认证已失效”。

单次天气 Worker 使用 8KB 任务栈，完成后立即释放。HTTP 响应最多读取 4KB，位置、天气描述和
温度在写入圆屏前均执行边界校验。

## 9. 动态 MCP 工具

设备绑定并联网后，固件使用 `device_token` 获取当前设备的权威工具清单：

```http
GET /api/mcp-tools/manifest
Authorization: Bearer {device_token}
Accept: application/json
```

固件在以下时机同步清单：

- 网络连接成功后立即同步。
- 首次完成设备绑定并保存设备 Token 后立即同步。
- 每 30 秒检查一次清单修订号。
- 工具执行返回 HTTP `409` 版本冲突后重新同步。

新 revision 成功安装或认证失效导致动态工具被清空后，设备发送 MCP 标准通知
`notifications/tools/list_changed`。设备在 MCP `initialize` 响应中声明 `tools.listChanged=true`，
小智云端收到通知后应重新请求 `tools/list`，避免继续使用当前会话早期缓存的旧工具清单。

第一版最多安装 10 个动态工具。固件要求工具名以 `custom.` 开头、名称不超过64字节、说明不超过
512个 UTF-8 字节、`parameters` 为空数组且 `result_schema_version` 为 `1.0`。任意工具不符合约束时，
整份新清单都不会替换当前工具。普通断网、网关错误或 JSON 错误会保留最近一次成功清单；设备 Token
明确返回 `401` 或 `403` 时清空动态工具，避免失效凭据继续暴露原用户能力。

小智调用动态工具时，固件不接收运行时参数，并按清单固定名称和版本请求：

```http
POST /api/mcp-tools/execute
Authorization: Bearer {device_token}
Content-Type: application/json

{
  "tool_name": "custom.order.query",
  "tool_revision": 1,
  "arguments": []
}
```

执行接口必须返回 `ServiceExecutionResult v1`。固件校验 `schema_version`、`tool_name`、数值状态和
文本长度，只把有效 `content` 返回小智模型；`status=2` 时设置 MCP `isError=true`，失败且内容为空时
使用设备保底错误文本。清单同步和工具执行分别运行在独立 8KB Worker 中，第一版同一时间最多执行
一个动态工具，避免多个 HTTPS 请求同时占用 ESP32-C5 内存。

## 10. 尚未接入的业务

备忘录目前仍由 `BackendService::OnScreensaverChanged()` 写入 `备忘录服务开发中` 占位内容。
后续接入备忘录和主动提醒时，应复用已经持久化的设备访问 Token，并继续通过
`BackendService` 统一管理网络请求，不能把业务请求散落到显示层或应用状态机中。
