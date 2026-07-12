# 后端 API 接入说明

## 1. 接入范围

固件通过独立后台 Worker 接入 `https://api.tuntun.life`，用于以下功能：

- 设备自动激活和访问令牌重签
- 屏保天气读取与天气位置语音设置
- 屏保备忘录读取和多条轮播
- 备忘录创建、列表、统计、匹配、修改、完成、恢复和删除

小智原有 ASR、大模型、TTS、OTA 和语音协议不依赖该业务后端。业务后端激活失败时，
设备只显示一次非阻塞通知，原有语音助手功能仍可继续使用。

## 2. 固件配置

当前业务配置位于：

```text
main/backend/backend_service.cc
```

固定配置如下：

| 配置 | 当前值 | 作用 |
|---|---|---|
| API 根地址 | `https://api.tuntun.life` | 天气、备忘录和设备激活服务地址 |
| 设备型号 | `movecall-moji2-esp32c5` | 后端允许激活的唯一板型 |
| 默认时区 | `Asia/Shanghai` | MCP 未指定时区时使用 |
| HTTP 超时 | 10 秒 | 单次连接和响应等待上限 |
| Worker 队列 | 8 | 同时等待处理的后台作业上限 |
| Worker 栈 | 8 KB | TLS、JSON 和请求编排任务栈 |

激活码作为编译期配置写入固件，不会输出到日志。发布公开固件前，应将激活码改为按
设备生产或安全分发的凭据，避免通用激活码被提取和滥用。

## 3. 设备身份和 NVS

激活请求使用以下身份：

- `serial_number`：Wi-Fi STA MAC 地址
- `mac_address`：Wi-Fi STA MAC 地址
- `client_id`：`Board::GetUuid()` 生成并持久化的 UUID
- `model`：`movecall-moji2-esp32c5`
- `firmware_version`：当前应用描述中的版本号

激活成功后，`device_id` 和 `access_token` 保存到普通 NVS 的 `tuntun_api` 命名空间。
普通 Wi-Fi 重新配网不会删除这两个字段。后端返回业务码 `2001` 时，固件会删除旧凭据，
重新激活并重试一次原请求。

后端允许同一物理设备重签，但要求序列号、MAC、客户端 UUID 和型号与原记录完全一致。
重签会撤销该设备的所有旧令牌，并保留原用户和原 `device_id`。

## 4. 请求协议

除激活接口外，业务请求统一携带：

```http
Authorization: Bearer <access_token>
X-Device-Id: <device_id>
X-Client-Id: <Board UUID>
X-Request-Id: <每次请求新生成的 UUID>
X-Firmware-Version: <固件版本>
Content-Type: application/json
Accept: application/json
```

创建备忘录额外携带 `Idempotency-Key`。同一次创建作业的网络重试复用相同幂等键，
其他请求和下一次创建使用新的 UUID。

激活和天气响应最多读取 4 KB，备忘录响应最多读取 12 KB。超出上限的响应会被拒绝，
避免异常响应占满设备堆内存。

## 5. 屏保刷新

进入屏保后按以下顺序立即发起请求：

1. `POST /api/weather/get`
2. `POST /api/memo/screensaver/get`

天气之后每 30 分钟刷新，备忘录之后每 5 分钟刷新。两类请求在同一个 Worker 中串行执行，
不会并发占用 TLS 内存。网络错误、超时或 HTTP 5xx 按 10 秒、30 秒、60 秒退避重试；
普通业务错误不重试。

小智 MCP 协议在请求期间断开时，列表、统计和匹配等读操作停止后续重试；创建、修改、
完成、恢复、删除和天气位置设置等写操作仍在 Worker 中继续，避免已经确认的语音操作
因为协议瞬时断开而静默丢失。

请求失败时保留最近一次成功数据。响应返回较晚时会更新 RAM 缓存，但只有屏保仍然可见且
请求世代与当前屏保一致时才更新 LVGL，因此后台响应不会切换到对话界面。

屏保最多缓存并轮播 5 条备忘录。短文本展示 8 秒；长文本滚动后额外停留约 3 秒；
后端返回新数组时从第一条重新开始。

## 6. MCP 工具

固件向小智云端注册以下异步工具：

| 工具 | 后端接口 | 作用 |
|---|---|---|
| `tuntun.weather.set_location` | `/api/weather/location/set` | 支持省、市、区县完整文本，并在歧义候选确认后设置天气位置 |
| `tuntun.memo.create` | `/api/memo/create` | 创建普通或定时备忘录 |
| `tuntun.memo.list` | `/api/memo/list` | 按范围、状态、关键词分页查询，默认 5 条、最多 10 条 |
| `tuntun.memo.statistics` | `/api/memo/statistics` | 查询全部、未完成、未到期、今天、明天或本周数量 |
| `tuntun.memo.match` | `/api/memo/match` | 根据语音关键词取得需要用户确认的候选 |
| `tuntun.memo.update` | `/api/memo/update` | 使用 `memo_id` 和 `version` 修改内容或提醒时间 |
| `tuntun.memo.complete` | `/api/memo/complete` | 将备忘录标记为已完成 |
| `tuntun.memo.reopen` | `/api/memo/reopen` | 将已完成备忘录恢复为未完成 |
| `tuntun.memo.delete` | `/api/memo/delete` | 用户明确确认后软删除备忘录 |

时间参数使用带时区偏移的 ISO 8601 文本，时区使用 IANA 名称。工具成功时把后端统一响应
作为结构化 JSON 文本返回；后端业务失败时设置 MCP `isError=true`。

## 7. 日志和安全

固件日志只记录接口路径、HTTP 状态码和业务码，不记录以下内容：

- 激活码
- Bearer Token
- 完整激活响应
- 备忘录正文

生产环境必须使用有效域名证书。固件通过现有 HTTPS 网络实现和系统 CA 证书包校验
`api.tuntun.life`，不得改为跳过证书或域名校验。
