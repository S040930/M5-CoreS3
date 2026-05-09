# 修复 UI 颜色逻辑

## 需求

- 录音前（待机状态）：绿色
- 录音时（触摸 PTT）：蓝色
- 不使用唤醒词

## 当前状态分析

### UI 状态定义（screen_ui.h）

```c
typedef enum {
  SCREEN_UI_VOICE_OFF = 0,
  SCREEN_UI_VOICE_CONNECTING,
  SCREEN_UI_VOICE_LISTENING,   // 当前用于 IDLE 和 RECORDING
  SCREEN_UI_VOICE_SENDING,
  SCREEN_UI_VOICE_THINKING,
  SCREEN_UI_VOICE_SPEAKING,
  SCREEN_UI_VOICE_ERROR,
  SCREEN_UI_VOICE_STANDBY,
} screen_ui_voice_state_t;
```

### 当前颜色映射（screen_ui.c）

| UI 状态 | 麦克风按钮颜色 | 面部颜色 |
|---------|--------------|---------|
| STANDBY | 绿色 (0x4CAF50) | 深灰/深绿 |
| CONNECTING | 蓝色 (0x2196F3) | 白色/蓝色 |
| LISTENING | 蓝色 (0x2196F3) | 白色/青色 |
| SENDING | 蓝色 (0x2196F3) | 白色/青色 |
| THINKING | 橙色 (0xFF9800) | 白色/黄色 |
| SPEAKING | 橙色 (0xFF9800) | 白色/青色 |
| ERROR | 绿色 (0x4CAF50) | 红色 |

### 控制器状态映射（voice_controller.c）

| 控制器状态 | 当前 UI 状态 | 问题 |
|-----------|-------------|------|
| IDLE (待机) | LISTENING | 应该是绿色，但用了 LISTENING（蓝色） |
| RECORDING (录音) | LISTENING | 正确，蓝色 |
| REQUESTING (请求中) | THINKING | 正确，橙色 |
| PLAYING (播放中) | SPEAKING | 正确，橙色 |

## 问题根因

1. **IDLE 状态映射错误**：`CTRL_STATE_IDLE` 被映射到 `REALTIME_VOICE_STATE_LISTENING`，导致待机时显示蓝色
2. **没有独立的 RECORDING UI 状态**：录音和待机共用 `LISTENING` 状态

## 修复方案

### 方案：修改 IDLE 状态的 UI 映射

**文件**: `components/realtime_voice/voice_controller.c`

**修改**：将 IDLE 状态的 UI 从 `REALTIME_VOICE_STATE_LISTENING` 改为 `REALTIME_VOICE_STATE_STANDBY`

**当前代码**（第 261 行）：
```c
s.state = CTRL_STATE_IDLE;
s.state_enter_ms = esp_timer_get_time() / 1000;
set_ui_state(REALTIME_VOICE_STATE_LISTENING, NULL, NULL, NULL);
```

**修改为**：
```c
s.state = CTRL_STATE_IDLE;
s.state_enter_ms = esp_timer_get_time() / 1000;
set_ui_state(REALTIME_VOICE_STATE_STANDBY, NULL, NULL, NULL);
```

**同样修改**（其他 IDLE 状态恢复的地方）：
- 第 172 行：`set_ui_state(REALTIME_VOICE_STATE_LISTENING, NULL, NULL, NULL);` → `STANDBY`
- 第 305 行：`set_ui_state(REALTIME_VOICE_STATE_LISTENING, s.last_user, NULL, NULL);` → 保持 LISTENING（这是录音状态）
- 第 368 行：`set_ui_state(REALTIME_VOICE_STATE_LISTENING, NULL, NULL, NULL);` → `STANDBY`
- 第 386 行：`set_ui_state(REALTIME_VOICE_STATE_LISTENING, NULL, NULL, NULL);` → `STANDBY`
- 第 411 行：`set_ui_state(REALTIME_VOICE_STATE_LISTENING, NULL, NULL, NULL);` → `STANDBY`
- 第 592 行：`set_ui_state(REALTIME_VOICE_STATE_LISTENING, s.last_user, NULL, NULL);` → 保持 LISTENING

### STANDBY 颜色确认

根据 `screen_ui.c` 第 339-345 行：
```c
case SCREEN_UI_VOICE_STANDBY:
  lv_obj_set_style_bg_color(s_ui.mic_btn, lv_color_hex(0x4CAF50), 0);  // 绿色
  lv_obj_set_style_bg_opa(s_ui.mic_btn, LV_OPA_80, 0);
  lv_obj_remove_state(s_ui.mic_btn, LV_STATE_DISABLED);
  lv_obj_set_style_text_color(s_ui.mic_label, lv_color_hex(0xFFFFFF), 0);
  break;
```

STANDBY 已经是绿色，符合需求。

### 唤醒词禁用

用户选择不使用唤醒词。当前代码中唤醒词逻辑由 `CONFIG_VOICE_ACTIVATION_PHRASE_ENABLE` 控制。

**检查 sdkconfig**：确认 `CONFIG_VOICE_ACTIVATION_PHRASE_ENABLE` 是否为 false/n。

## 实施步骤

### 步骤 1: 修改 IDLE 状态的 UI 映射

**文件**: `components/realtime_voice/voice_controller.c`

将所有从 IDLE 状态恢复时的 `REALTIME_VOICE_STATE_LISTENING` 改为 `REALTIME_VOICE_STATE_STANDBY`。

### 步骤 2: 确认唤醒词已禁用

**文件**: `config/config.toml` 或 sdkconfig

确保 `CONFIG_VOICE_ACTIVATION_PHRASE_ENABLE` 未启用。

### 步骤 3: 验证颜色映射

| 场景 | 控制器状态 | UI 状态 | 麦克风颜色 | 面部颜色 |
|------|-----------|---------|-----------|---------|
| 待机（录音前） | IDLE | STANDBY | 绿色 | 深灰 |
| 录音中（触摸PTT） | RECORDING | LISTENING | 蓝色 | 白色/青色 |
| 请求中 | REQUESTING | THINKING | 橙色 | 白色/黄色 |
| 播放中 | PLAYING | SPEAKING | 橙色 | 白色/青色 |
| 错误 | - | ERROR | 绿色 | 红色 |

## 风险评估

1. **STANDBY 状态的颜色**：当前 STANDBY 的面部颜色是深灰/深绿，可能不够明显。可以调整为更亮的绿色。
2. **唤醒词完全禁用**：如果用户后续想要唤醒词，需要重新启用。

## 决策

- **主要方案**：将 IDLE 状态的 UI 映射从 LISTENING 改为 STANDBY
- **唤醒词**：保持禁用（用户选择）
- **颜色调整**：如有需要，后续可调整 STANDBY 的面部颜色
