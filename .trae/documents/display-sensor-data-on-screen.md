# 屏幕显示传感器数据方案

## 总结

在 Pixel Buddy 表情的 STANDBY 状态下，在屏幕左右两侧显示环境传感器数据（温度、湿度、气压）。录音/播放等活跃状态时自动隐藏。

## 当前状态分析

### 现有 UI 架构
- **screen_ui.c**：Pixel Buddy 像素表情，320×240 屏幕，canvas 渲染
- **ui_renderer**：提供 `draw_text`、`draw_rect` 等像素级绘制 API
- **STANDBY 状态**：眼睛半闭（0.4），嘴巴闭合，灰绿色调，有呼吸动画
- **屏幕布局**：眼睛 Y=80，嘴巴 Y=150，麦克风按钮在底部 Y=140~220

### 传感器数据源
- **env_monitor.c**：SHT30（温度+湿度）+ QMP6988（气压），30秒轮询
- **`env_monitor_get_latest()`**：获取最新温度、湿度、气压，线程安全
- **注意**：日志中显示 `SHT30 probe failed`，传感器可能未就绪，需处理无数据情况

### 屏幕空间分析（320×240）
- 表情居中：眼睛 X=122~198，嘴巴 X=152~168
- 左侧可用区域：X=4~110，Y=60~180
- 右侧可用区域：X=210~316，Y=60~180
- 麦克风按钮：X=120~200，Y=140~220（需避开）

## 修改方案

### 修改文件：`components/screen_ui/screen_ui.c`

#### 1. 添加 env_monitor 头文件引用

```c
#include "env_monitor.h"
```

#### 2. 在 ctx 结构体中添加传感器数据缓存

```c
float env_temp_c;
float env_humidity_pct;
float env_pressure_kpa;
bool env_data_valid;
uint32_t env_update_phase;  // 控制更新频率
```

#### 3. 在 `render_pixel_face()` 中 STANDBY 状态下绘制传感器数据

在表情渲染完成后，当 `voice_state == SCREEN_UI_VOICE_STANDBY` 时，在左右两侧绘制传感器数据：

**左侧**（温度 + 湿度）：
- 温度：X=8, Y=70，格式 "24.5°C"
- 湿度：X=8, Y=100，格式 "65.3%"

**右侧**（气压）：
- 气压：X=220, Y=85，格式 "101.3kPa"

使用 `ui_renderer_draw_text` 绘制，字体用 `lv_font_montserrat_14`，颜色用暗灰色（0x666666），与 STANDBY 的低调风格一致。

无数据时显示 "--" 占位。

#### 4. 在 `anim_timer_cb` 中定期刷新传感器数据

每 30 帧（约 15 秒，anim_interval=500ms）调用一次 `env_monitor_get_latest()`，更新缓存并标记 `render_dirty`。

#### 5. 绘制细节

- 仅在 `SCREEN_UI_VOICE_STANDBY` 状态下绘制
- 使用半透明背景矩形提升可读性
- 文字颜色：0x888888（中灰色，不抢眼）
- 背景色：0x1a1a1a，opa=160（半透明深色底）

## 验证步骤

1. 编译：`pio run -e m5cores3`
2. 烧录测试
3. 验证：STANDBY 状态下左右两侧显示传感器数值，点击录音后数据隐藏
