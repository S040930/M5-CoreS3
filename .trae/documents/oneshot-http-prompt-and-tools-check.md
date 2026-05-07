# One-shot HTTP 系统提示词与工具调用检查

## 结论：无需修改

### 系统提示词现状

One-shot HTTP 路径已完整集成预设系统提示词（上一轮删除 WebSocket 时完成）：

- **文件**: `components/realtime_voice/voice_request.c` L30-57
- **实现**: `prompt_preset_instructions()` 提供 3 种预设（balanced/conversational/factual），由 `config.toml` 的 `voice.session.prompt_preset` 控制
- **工具描述**: 当 `CONFIG_VOICE_TOOLS_ENABLE` 开启时，`build_session_instructions()` 自动追加工具说明后缀
- **请求构建**: `build_request_body()` L409-414 将系统提示词作为 `role: "system"` 消息加入请求

### 当前可用工具（11 个）

| # | 工具名 | 功能 | 参数 |
|---|--------|------|------|
| 1 | `set_timer` | 设置一次性定时器 | `duration_sec`(必填), `label`(可选) |
| 2 | `cancel_timer` | 取消定时器 | `timer_id`(必填) |
| 3 | `get_device_status` | 设备状态概览 | 无 |
| 4 | `set_volume` | 调节音量 | `percent`, `delta_percent`, `muted` |
| 5 | `get_volume` | 读取音量 | 无 |
| 6 | `get_time` | 获取当前时间 | 无 |
| 7 | `get_date` | 获取当前日期 | 无 |
| 8 | `get_network_status` | 网络/WiFi状态 | 无 |
| 9 | `set_screen_brightness` | 屏幕亮度 | `brightness_percent`(必填) |
| 10 | `play_local_chime` | 播放提示音 | `frequency_hz`, `duration_ms`, `amplitude_pct` |
| 11 | `airplay_status` | AirPlay状态 | 无 |

所有工具 schema 通过 `voice_tools_append_session_schemas()` 注册到 Chat Completions 请求的 `tools` 字段中。

### 无需操作

系统提示词已预设，工具调用已完整集成，无需额外修改。
