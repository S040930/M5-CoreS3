# 删除 5 个语音工具

## 摘要

删除 `get_device_status`、`get_network_status`、`set_screen_brightness`、`play_local_chime`、`airplay_status` 五个工具，并清理因此变为死代码的回调注册机制。

删除后保留 6 个工具：`set_timer`、`cancel_timer`、`set_volume`、`get_volume`、`get_time`、`get_date`。

## 当前状态

5 个待删工具在 `voice_tools.c` 中各占两处代码：
1. **dispatch 分支**（`voice_tools_dispatch` 函数内的 `if (strcmp(name, ...))` 块）
2. **schema 注册**（`voice_tools_append_session_schemas` 函数内的 cJSON 构建块）

这 5 个工具是 8 个回调函数的唯一消费者：

| 回调 | 被谁使用 |
|------|---------|
| `s_airplay_active_fn` | `airplay_status` |
| `s_wifi_connected_fn` | `get_device_status` + `get_network_status` |
| `s_wifi_get_ip_fn` | `get_device_status` + `get_network_status` |
| `s_network_ready_fn` | `get_device_status` + `get_network_status` |
| `s_receiver_state_str_fn` | `get_device_status` + `get_network_status` |
| `s_streaming_fn` | `get_device_status` + `get_network_status` |
| `s_discoverable_fn` | `get_network_status` |
| `s_display_brightness_set_fn` | `set_screen_brightness` |

删除 5 个工具后，这 8 个回调全部变为死代码，需一并清理。

`#include "auto_brightness.h"` 仅被 `set_screen_brightness` 工具使用（调用 `auto_brightness_notify_manual_override()`），删除后也可移除。

## 修改清单

### 1. `components/realtime_voice/voice_tools.c`

- 移除 `#include "auto_brightness.h"`
- 移除 8 个 `static` 回调变量（L24-L31）及其 8 个 setter 函数（L33-L40）
- 移除 dispatch 中 5 个工具的 `if` 分支：
  - `get_network_status`（L249-L268）
  - `set_screen_brightness`（L270-L296）
  - `play_local_chime`（L298-L337）
  - `airplay_status`（L339-L344）
  - `get_device_status`（L346-L365）
- 移除 `voice_tools_append_session_schemas` 中 5 个工具的 schema 构建块：
  - `get_device_status`（t3, L415-L423）
  - `get_network_status`（t8, L477-L485）
  - `set_screen_brightness`（t9, L487-L500）
  - `play_local_chime`（t10, L502-L521）
  - `airplay_status`（t11, L523-L531）

### 2. `components/realtime_voice/voice_tools.h`

- 移除 8 个 typedef（L12-L19）
- 移除 8 个 setter 声明（L21-L28）

### 3. `components/app_core/app_core.c`

- 移除 L375-L382 的 8 行 `voice_tools_set_*_fn()` 调用

## 不修改

- `voice_timers_active_count()` 和 `realtime_voice_is_activation_armed()` 虽然删除后不再被 voice_tools 调用，但作为通用工具函数保留定义和声明
- `auto_brightness` 组件本身保留（自动亮度功能仍在运行），仅移除 voice_tools 对它的引用

## 验证

编译通过即可：`pio run -e m5cores3`
