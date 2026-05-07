# Pixel Buddy Face UI — 优化实现计划

## 设计方向确认

- **渲染方式**: LVGL 原生对象（圆角矩形 + 标签）
- **视觉风格**: 极简几何 — 圆角矩形眼睛 + 小圆点/频谱条嘴巴
- **AirPlay 显示**: 待机脸 + 小 AirPlay 指示器（不替换脸）
- **SPEAKING 动画**: 频谱条代替嘴巴（复用现有 ui_spectrum）
- **THINKING 动画**: 三个跳动圆点

## 当前代码状态

### screen_ui.h 公共 API（保持不变）

```c
typedef enum {
  SCREEN_UI_STATE_BOOT = 0,
  SCREEN_UI_STATE_CONFIG_REQUIRED,
  SCREEN_UI_STATE_NETWORK_READY,
  SCREEN_UI_STATE_DISCOVERABLE,
  SCREEN_UI_STATE_SESSION_ESTABLISHING,
  SCREEN_UI_STATE_STREAMING,
  SCREEN_UI_STATE_RECOVERING,
  SCREEN_UI_STATE_FAULT,
} screen_ui_state_t;

typedef enum {
  SCREEN_UI_VOICE_OFF = 0,
  SCREEN_UI_VOICE_CONNECTING,
  SCREEN_UI_VOICE_LISTENING,
  SCREEN_UI_VOICE_SENDING,
  SCREEN_UI_VOICE_THINKING,
  SCREEN_UI_VOICE_SPEAKING,
  SCREEN_UI_VOICE_ERROR,
  SCREEN_UI_VOICE_STANDBY,
} screen_ui_voice_state_t;

esp_err_t screen_ui_init(void);
void screen_ui_deinit(void);
void screen_ui_set_state(screen_ui_state_t state, bool wifi_connected,
                         bool airplay_ready, bool streaming);
void screen_ui_set_metadata(const screen_ui_metadata_t *metadata);
void screen_ui_set_playing(bool playing);
void screen_ui_set_voice_state(screen_ui_voice_state_t state, const char *user_text,
                               const char *assistant_text, const char *error_text);
void screen_ui_set_voice_ptt_callback(void (*callback)(void));
void screen_ui_set_voice_network_busy(bool busy);
```

### screen_ui.c 当前状态

- 只有 2 个标签：`airplay_label`（顶部 "AirPlay"）和 `omni_label`（底部 "omni"）
- `anim_timer_cb` 每 100ms 调用 `screen_ui_update_labels()`，只控制两个标签的显示/隐藏
- `screen_ui_set_voice_state()` 只更新 `s_ui.voice_state` 并触发 `screen_ui_update_labels()`
- 背景色：`lv_color_hex(0x000000)` 纯黑

### 现有渲染子系统（ui_renderer / canvas 体系）

项目已有完整的像素级 canvas 渲染框架：
- `ui_renderer.c` — 320x240 RGB565 canvas，支持 pixel/rect/circle/line/text/gradient
- `ui_particles.c` — 40 个浮动粒子，用 `ui_renderer_draw_circle` 渲染
- `ui_spectrum.c` — 左右各 6 个频谱条，用 `ui_renderer_draw_gradient_rect` 渲染
- `ui_decor.c` — 四角括号装饰 + 扫描线效果

**这些模块当前未被 screen_ui.c 使用**，是独立的子系统。

## 优化后的实现方案

### 核心决策：用 LVGL 对象做脸，复用现有 API

PLAN9 要求"Use LVGL primitive objects only"，且用户选择 B（LVGL 原生对象）。因此：

1. **眼睛**: 用 `lv_obj` + `lv_style_set_radius()` 做成圆角矩形，通过改变 `width/height/x/y` 实现眨眼/半闭
2. **瞳孔**: 眼睛内部的小圆角矩形，通过改变 `x` 实现左右漂移
3. **嘴巴**: 
   - STANDBY/LISTENING/SENDING/ERROR: 一个小圆角矩形（闭嘴或微张）
   - THINKING: 三个小圆点，用 LVGL 动画或定时器改变 `y` 实现跳动
   - SPEAKING: **复用 ui_spectrum** — 在眼睛下方显示频谱条
4. **AirPlay 指示器**: 右上角一个小圆点 + 标签
5. **状态文字**: 底部一行小字（只在 SENDING/THINKING/ERROR 时显示）
6. **背景**: 保持纯黑，可选添加 subtle 粒子效果（后续迭代）

### 为什么不用 ui_renderer canvas

- canvas 需要手动管理 320x240x2=153KB 缓冲区，刷新时需要全屏重绘
- LVGL 对象由 LVGL 内部脏矩形机制优化，只重绘变化区域
- 在 320x240 小屏幕上，LVGL 对象的性能完全足够
- 代码更简洁，与现有 screen_ui.c 的风格一致

### 文件改动

#### 1. screen_ui.c — 核心重构

**新增内部结构体**（扩展 `screen_ui_ctx_t`）：

```c
typedef struct {
  lv_obj_t *screen;
  lv_obj_t *airplay_label;
  lv_obj_t *omni_label;
  lv_timer_t *anim_timer;
  bool initialized;
  bool streaming;
  bool playing;
  screen_ui_state_t state;
  screen_ui_voice_state_t voice_state;
  void (*voice_ptt_callback)(void);
  
  // === NEW: Face objects ===
  lv_obj_t *face_container;      // 脸的整体容器，方便整体显示/隐藏
  lv_obj_t *left_eye;            // 左眼外框（圆角矩形）
  lv_obj_t *left_pupil;          // 左眼瞳孔
  lv_obj_t *right_eye;           // 右眼外框
  lv_obj_t *right_pupil;         // 右眼瞳孔
  lv_obj_t *mouth;               // 嘴巴（圆角矩形）
  lv_obj_t *thinking_dots[3];    // THINKING 时的三个圆点
  lv_obj_t *status_label;        // 底部状态文字
  lv_obj_t *airplay_indicator;   // AirPlay 小圆点指示器
  
  // === NEW: Animation state ===
  uint32_t anim_phase;           // 动画相位计数器 (0-255)
  float mouth_openness;          // 嘴巴张开程度 (0.0-1.0)
  float eye_openness;            // 眼睛张开程度 (0.0-1.0)
  float pupil_offset_x;          // 瞳孔水平偏移
  int32_t target_eye_h;          // 目标眼睛高度（用于平滑过渡）
  int32_t target_mouth_h;        // 目标嘴巴高度
  
  // === NEW: Colors per state ===
  lv_color_t eye_color;
  lv_color_t pupil_color;
  lv_color_t mouth_color;
} screen_ui_ctx_t;
```

**新增常量**（320x240 布局）：

```c
#define FACE_EYE_W        48
#define FACE_EYE_H        48
#define FACE_EYE_H_HALF   24   // 半闭时的高度
#define FACE_EYE_H_CLOSED 4    // 闭眼时的高度
#define FACE_PUPIL_W      16
#define FACE_PUPIL_H      16
#define FACE_MOUTH_W      24
#define FACE_MOUTH_H      8
#define FACE_MOUTH_H_OPEN 20
#define FACE_DOT_SIZE     6
#define FACE_EYE_Y        85
#define FACE_EYE_GAP      24
#define FACE_MOUTH_Y      155
#define FACE_STATUS_Y     210
```

**颜色方案**（按状态）：

| 状态 | 眼睛颜色 | 瞳孔颜色 | 嘴巴颜色 |
|------|---------|---------|---------|
| STANDBY | `#1a3a2a` (暗绿) | `#0d1f15` | `#1a3a2a` |
| LISTENING | `#00D4AA` (青绿) | `#004d3d` | `#00D4AA` |
| SENDING | `#00D4AA` | `#004d3d` | `#00D4AA` |
| THINKING | `#E8C547` (暖黄) | `#5c4a1a` | `#E8C547` |
| SPEAKING | `#00D4AA` | `#004d3d` | `#00D4AA` |
| ERROR | `#E84A4A` (红) | `#5c1a1a` | `#E84A4A` |
| CONNECTING | `#4A90E8` (蓝) | `#1a3a5c` | `#4A90E8` |

**初始化流程**（`screen_ui_init()` 扩展）：

1. 创建 `face_container`（全屏，透明背景，clickable）
2. 创建左右眼睛（圆角矩形，`radius = width/2` 做成圆形/胶囊形）
3. 创建左右瞳孔（小圆角矩形，放在眼睛中心）
4. 创建嘴巴（小圆角矩形）
5. 创建三个 thinking 圆点（初始 hidden）
6. 创建 status_label（底部小字，初始 hidden）
7. 创建 airplay_indicator（右上角小圆点 + 文字，初始 hidden）
8. 设置所有对象的样式（无边框、无阴影、纯色填充）
9. 将屏幕点击事件从 `s_ui.screen` 移到 `face_container`

**动画定时器**（`anim_timer_cb` 扩展）：

```c
static void anim_timer_cb(lv_timer_t *timer) {
  (void)timer;
  if (!s_ui.initialized) return;
  
  s_ui.anim_phase++;
  
  // 根据 voice_state 计算目标参数
  switch (s_ui.voice_state) {
    case SCREEN_UI_VOICE_STANDBY:
      s_ui.target_eye_h = FACE_EYE_H_HALF;
      s_ui.target_mouth_h = FACE_MOUTH_H;
      // 呼吸效果：眼睛亮度缓慢变化
      break;
    case SCREEN_UI_VOICE_LISTENING:
      s_ui.target_eye_h = FACE_EYE_H;
      s_ui.target_mouth_h = FACE_MOUTH_H;
      // 脉冲效果：瞳孔轻微缩放
      break;
    case SCREEN_UI_VOICE_SENDING:
      s_ui.target_eye_h = (s_ui.anim_phase % 20 < 10) ? FACE_EYE_H : FACE_EYE_H_CLOSED;
      s_ui.target_mouth_h = FACE_MOUTH_H;
      break;
    case SCREEN_UI_VOICE_THINKING:
      s_ui.target_eye_h = FACE_EYE_H;
      s_ui.target_mouth_h = FACE_MOUTH_H;
      // 瞳孔左右漂移
      s_ui.pupil_offset_x = sinf(s_ui.anim_phase * 0.1f) * 8.0f;
      break;
    case SCREEN_UI_VOICE_SPEAKING:
      s_ui.target_eye_h = FACE_EYE_H;
      // 嘴巴随频谱跳动
      s_ui.target_mouth_h = FACE_MOUTH_H + (int)(get_spectrum_peak() * (FACE_MOUTH_H_OPEN - FACE_MOUTH_H));
      break;
    case SCREEN_UI_VOICE_ERROR:
      s_ui.target_eye_h = FACE_EYE_H_HALF;
      s_ui.target_mouth_h = FACE_MOUTH_H;
      break;
    default:
      break;
  }
  
  // 平滑过渡（lerp）
  apply_smooth_transition();
  
  // 更新所有对象的位置和尺寸
  update_face_geometry();
  
  // 更新颜色
  update_face_colors();
  
  // 更新 thinking dots 动画
  update_thinking_dots();
  
  // 更新原有标签
  screen_ui_update_labels();
}
```

**状态切换**（`screen_ui_set_voice_state` 扩展）：

```c
void screen_ui_set_voice_state(screen_ui_voice_state_t state, const char *user_text,
                               const char *assistant_text, const char *error_text) {
  s_ui.voice_state = state;
  
  if (!s_ui.initialized) return;
  if (!bsp_display_lock(pdMS_TO_TICKS(80))) return;
  
  // 显示/隐藏 face_container
  bool show_face = (state != SCREEN_UI_VOICE_OFF);
  if (show_face) {
    lv_obj_remove_flag(s_ui.face_container, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(s_ui.face_container, LV_OBJ_FLAG_HIDDEN);
  }
  
  // 根据状态设置颜色
  switch (state) {
    case SCREEN_UI_VOICE_STANDBY:
      s_ui.eye_color = lv_color_hex(0x1a3a2a);
      s_ui.pupil_color = lv_color_hex(0x0d1f15);
      s_ui.mouth_color = lv_color_hex(0x1a3a2a);
      break;
    case SCREEN_UI_VOICE_LISTENING:
    case SCREEN_UI_VOICE_SENDING:
    case SCREEN_UI_VOICE_SPEAKING:
      s_ui.eye_color = lv_color_hex(0x00D4AA);
      s_ui.pupil_color = lv_color_hex(0x004d3d);
      s_ui.mouth_color = lv_color_hex(0x00D4AA);
      break;
    case SCREEN_UI_VOICE_THINKING:
      s_ui.eye_color = lv_color_hex(0xE8C547);
      s_ui.pupil_color = lv_color_hex(0x5c4a1a);
      s_ui.mouth_color = lv_color_hex(0xE8C547);
      break;
    case SCREEN_UI_VOICE_ERROR:
      s_ui.eye_color = lv_color_hex(0xE84A4A);
      s_ui.pupil_color = lv_color_hex(0x5c1a1a);
      s_ui.mouth_color = lv_color_hex(0xE84A4A);
      break;
    case SCREEN_UI_VOICE_CONNECTING:
      s_ui.eye_color = lv_color_hex(0x4A90E8);
      s_ui.pupil_color = lv_color_hex(0x1a3a5c);
      s_ui.mouth_color = lv_color_hex(0x4A90E8);
      break;
    default:
      break;
  }
  
  // 更新 status_label 文字
  if (error_text && error_text[0]) {
    lv_label_set_text(s_ui.status_label, error_text);
    lv_obj_remove_flag(s_ui.status_label, LV_OBJ_FLAG_HIDDEN);
  } else if (state == SCREEN_UI_VOICE_SENDING) {
    lv_label_set_text(s_ui.status_label, "sending...");
    lv_obj_remove_flag(s_ui.status_label, LV_OBJ_FLAG_HIDDEN);
  } else if (state == SCREEN_UI_VOICE_THINKING) {
    lv_label_set_text(s_ui.status_label, "thinking...");
    lv_obj_remove_flag(s_ui.status_label, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(s_ui.status_label, LV_OBJ_FLAG_HIDDEN);
  }
  
  // THINKING 时显示圆点，隐藏嘴巴
  if (state == SCREEN_UI_VOICE_THINKING) {
    lv_obj_add_flag(s_ui.mouth, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < 3; i++) {
      lv_obj_remove_flag(s_ui.thinking_dots[i], LV_OBJ_FLAG_HIDDEN);
    }
  } else {
    lv_obj_remove_flag(s_ui.mouth, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < 3; i++) {
      lv_obj_add_flag(s_ui.thinking_dots[i], LV_OBJ_FLAG_HIDDEN);
    }
  }
  
  bsp_display_unlock();
}
```

**AirPlay 状态**（`screen_ui_set_state` 扩展）：

```c
void screen_ui_set_state(screen_ui_state_t state, bool wifi_connected,
                         bool airplay_ready, bool streaming) {
  s_ui.state = state;
  s_ui.streaming = streaming;
  
  if (!s_ui.initialized) return;
  if (!bsp_display_lock(pdMS_TO_TICKS(80))) return;
  
  // AirPlay 指示器：当 voice 不在活跃状态且 AirPlay 就绪时显示
  bool show_airplay_indicator = (s_ui.voice_state == SCREEN_UI_VOICE_OFF) && airplay_ready;
  if (show_airplay_indicator) {
    lv_obj_remove_flag(s_ui.airplay_indicator, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(s_ui.airplay_indicator, LV_OBJ_FLAG_HIDDEN);
  }
  
  // 原有逻辑
  screen_ui_update_labels();
  bsp_display_unlock();
}
```

#### 2. screen_ui.h — 无需改动

公共 API 完全兼容，所有现有调用者（app_core.c, airplay_service.c, sedentary_alert.c）无需修改。

#### 3. ui_spectrum.c — 可选复用

如果要在 SPEAKING 时显示频谱条，需要：
- 在 `screen_ui.c` 中创建 `ui_spectrum_t` 实例
- 在 `anim_timer_cb` 中调用 `ui_spectrum_update()` 和 `ui_spectrum_render()`
- 但 `ui_spectrum_render()` 需要 `ui_renderer_t`，而我们现在用 LVGL 对象...

**决策**: 第一轮实现不用 ui_spectrum。SPEAKING 时的"频谱条"用 LVGL 的 `lv_bar` 对象模拟（6 个小 bar，高度随随机/正弦变化）。这样完全在 LVGL 对象体系内，不需要引入 canvas。

后续迭代可以添加：将 ui_spectrum 渲染到 canvas，然后把 canvas 作为 LVGL 图片对象叠加在脸下方。

#### 4. CMakeLists.txt — 无需改动

现有依赖（board_cores3, esp_timer, lvgl）已足够。

## 布局示意图（320x240）

```
+------------------------------------------+
|  [AirPlay●]                    [omi]     |  y=0~30
|                                          |
|       +----+        +----+               |  y=60~130
|       |    |        |    |   ← 眼睛      |
|       | ●  |        | ●  |   ← 瞳孔      |
|       +----+        +----+               |
|                                          |
|            +------+                      |  y=140~170
|            |      |   ← 嘴巴/频谱        |
|            +------+                      |
|                                          |
|         [thinking...]                    |  y=200~220
|                                          |
+------------------------------------------+
```

## 状态机映射

| Voice State | 眼睛 | 瞳孔 | 嘴巴 | 颜色 | 动画 |
|------------|------|------|------|------|------|
| OFF | 隐藏 | 隐藏 | 隐藏 | — | — |
| STANDBY | 半高 (24px) | 居中 | 小矩形 | 暗绿 | 呼吸亮度 |
| CONNECTING | 全高 | 居中 | 小矩形 | 蓝 | 缓慢脉冲 |
| LISTENING | 全高 | 居中 | 小矩形 | 青绿 | 快速脉冲 |
| SENDING | 眨眼 (10帧周期) | 居中 | 小矩形 | 青绿 | 眨眼 + "sending..." |
| THINKING | 全高 | 左右漂移 | 隐藏 | 暖黄 | 三个圆点跳动 + "thinking..." |
| SPEAKING | 全高 | 居中 | 频谱条 | 青绿 | 频谱条高度变化 |
| ERROR | 半高 | 居中 | 小矩形 | 红 | 静态 + error 文字 |

## 实现优先级

1. **P0**: 基础脸结构（眼睛+瞳孔+嘴巴）+ 状态颜色切换
2. **P0**: STANDBY / LISTENING / ERROR 状态
3. **P1**: THINKING 圆点动画
4. **P1**: SENDING 眨眼动画
5. **P1**: SPEAKING 频谱条（用 lv_bar 模拟）
6. **P2**: AirPlay 指示器
7. **P2**: 平滑过渡（lerp）
8. **P3**: 呼吸/脉冲效果

## 风险评估

1. **内存**: 新增 ~10 个 LVGL 对象，每个对象开销约 200-500 字节，总计 < 5KB，可忽略
2. **CPU**: 动画定时器 100ms 周期，每次更新 10 个对象的属性，开销极小
3. **刷新**: LVGL 脏矩形机制只重绘变化区域，比全屏 canvas 刷新更高效
4. **兼容性**: 公共 API 不变，所有调用者零改动

## 验证步骤

1. 编译通过：`pio run -e m5cores3`
2. 启动后显示 STANDBY 脸（暗绿色半闭眼睛）
3. 说 "Hi ESP" → 切换到 LISTENING（青绿色睁眼）
4. 说话后 → SENDING（眨眼 + "sending..."）
5. AI 思考中 → THINKING（暖黄色 + 三个跳动圆点）
6. AI 回复 → SPEAKING（频谱条动画）
7. 回复结束 → 回到 STANDBY
8. 错误时 → ERROR（红色半闭眼 + 错误文字）
9. AirPlay 连接时 → 待机脸 + 右上角小指示器
