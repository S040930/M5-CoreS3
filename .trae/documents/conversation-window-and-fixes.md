# 对话窗口 + 工具修复 + 自动亮度修复

## 摘要

三个任务：
1. **对话窗口**：唤醒后 120 秒内可直接说话，每次对话后重置计时
2. **工具调用修复**：系统提示词仍列出已删除工具，导致模型调用不存在的工具失败
3. **自动亮度检查**：代码逻辑正确，需确认硬件是否正常（无法在编译期验证）

## 1. 对话窗口

### 当前行为

唤醒词 "Hi ESP" 触发 `voice_session_arm_set(true)`，对话完成后 `voice_session_complete_recording()` 立即调用 `voice_session_arm_set(false)` 解除激活。每次都要重新唤醒。

另有 `CONFIG_VOICE_SESSION_IDLE_TIMEOUT_MS`（15秒）超时也会解除激活。

### 目标行为

唤醒后保持激活状态 120 秒，期间可直接说话。每次对话完成后重置 120 秒计时。超时后才需要重新唤醒。

### 修改方案

**核心思路**：对话完成后不再立即 `voice_session_arm_set(false)`，改为更新超时时间戳。在主循环中检查超时后再解除激活。

#### 1.1 `realtime_voice_internal.h` — 新增字段

在 `realtime_ctx_t` 中新增：
```c
uint64_t activation_window_end_ms;
```

#### 1.2 `realtime_voice.c` — 主循环修改

唤醒事件处理中（L307-L317），设置窗口结束时间：
```c
ctx->activation_window_end_ms = read_now_ms + 120000;
```

主循环中，将现有的 idle timeout 检查（L360-L367）替换为激活窗口超时检查：
```c
if (voice_session_arm_get() &&
    ctx->activation_window_end_ms > 0 &&
    read_now_ms >= ctx->activation_window_end_ms) {
  voice_session_arm_set(false);
  realtime_voice_reset_session();
  realtime_voice_set_state_internal(REALTIME_VOICE_STATE_LISTENING, NULL);
}
```

删除原来的 `CONFIG_VOICE_SESSION_IDLE_TIMEOUT_MS` 超时逻辑（因为激活窗口超时已覆盖此功能）。

#### 1.3 `voice_session.c` — 对话完成后重置窗口

在 `voice_session_complete_recording()` 中：
- 移除 `voice_session_arm_set(false)` （L523）
- 改为重置激活窗口：`ctx->activation_window_end_ms = voice_now_ms() + 120000;`
- 同时重置录音状态但保持 armed

#### 1.4 配置化

新增 `CONFIG_VOICE_ACTIVATION_WINDOW_MS` 配置项（默认 120000），通过 config.toml 的 `voice.activation.window_ms` 设置。

在 `pio_prebuild.py` 中添加映射：
```python
"voice.activation.window_ms": ("CONFIG_VOICE_ACTIVATION_WINDOW_MS", _int_str),
```

在 `config.toml` 和 `config.toml.example` 中添加：
```toml
[voice.activation]
window_ms = 120000
```

#### 1.5 `realtime_voice.c` — 默认值

```c
#ifndef CONFIG_VOICE_ACTIVATION_WINDOW_MS
#define CONFIG_VOICE_ACTIVATION_WINDOW_MS 120000
#endif
```

### 边界情况

- **AirPlay 激活时**：已有 `voice_session_arm_set(false)` 逻辑，会正确解除激活
- **网络断开时**：已有 standby 逻辑中 `voice_session_arm_set(false)`，会正确解除
- **录音中**：窗口超时不影响正在进行的录音（`vrec.recording` 为 true 时不会检查超时）

## 2. 工具调用修复

### 问题

`voice_request.c` 的 `build_session_instructions()` 仍列出已删除的 5 个工具：`get_device_status`、`get_network_status`、`set_screen_brightness`、`play_local_chime`、`airplay_status`。模型可能尝试调用这些工具，但 dispatch 中已无对应处理，返回 "unsupported tool" 错误。

### 修改

更新 `build_session_instructions()` 中的工具后缀，只列出当前 6 个有效工具：

```c
const char *suffix =
    " You may call tools when needed: set_timer and cancel_timer (relative delays only, not "
    "wall-clock alarms); get_time; get_date; "
    "set_volume; get_volume. Use set_volume for louder/quieter/mute. After a tool runs, reply "
    "briefly in voice.";
```

## 3. 自动亮度

### 分析

代码逻辑正确。`auto_brightness_start()` 在 `app_core.c` 中被调用，使用 `i2c_master_get_bus_handle()` 获取 I2C 总线。`ltr553_init()` 会检测 PART_ID=0x92。

可能的问题：
- LTR-553 I2C 地址错误（当前 0x23）
- I2C 总线未正确初始化
- `bsp_display_brightness_set()` 实际控制的是 AXP2101 DLDO1 电压，而非 PWM

**无法在编译期验证**，需要查看串口日志确认：
- `LTR-553 detected, part_id=0x92` — 传感器检测成功
- `ALS read failed` — 读取失败
- `auto_bright task started` — 任务启动

如果 PART_ID 不匹配或 I2C 通信失败，`auto_brightness_start()` 会返回错误但不会阻止系统运行。

### 修改

无代码修改。在验证步骤中说明如何通过串口日志诊断。

## 修改文件清单

| 文件 | 修改内容 |
|------|---------|
| `components/realtime_voice/realtime_voice_internal.h` | 新增 `activation_window_end_ms` 字段 |
| `components/realtime_voice/realtime_voice.c` | 唤醒时设置窗口；替换 idle timeout 为窗口超时；新增默认值宏 |
| `components/realtime_voice/voice_session.c` | 对话完成后重置窗口而非解除激活 |
| `components/realtime_voice/voice_request.c` | 修复系统提示词中的工具列表 |
| `scripts/pio_prebuild.py` | 添加 `voice.activation.window_ms` 映射 |
| `config/config.toml` | 添加 `window_ms = 120000` |
| `config/config.toml.example` | 添加 `window_ms = 120000` |

## 验证

1. 编译通过：`pio run -e m5cores3`
2. 烧录后测试：
   - 说 "Hi ESP" 唤醒，对话完成后直接说话应可识别（120 秒内）
   - 等待超过 120 秒后，需重新唤醒
   - 说 "10 秒后提醒我上厕所"，10 秒后应收到定时器提醒
3. 串口日志确认自动亮度：
   - 搜索 `LTR-553 detected` 或 `LTR-553 init failed`
   - 搜索 `auto_bright task started`
