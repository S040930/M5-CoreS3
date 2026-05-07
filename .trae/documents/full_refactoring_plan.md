# 项目全面重构计划

## 概述

对 airplay 项目进行全面分阶段重构，目标：架构清晰度、代码质量、性能优化、可维护性，以及协调 AirPlay + 语音助手的资源冲突。每个阶段完成后系统必须可运行。

---

## 当前状态分析

### 架构问题

| 问题 | 严重程度 | 说明 |
|------|---------|------|
| 资源冲突机制分散 | 🔴 高 | AirPlay/语音的扬声器仲裁散落在 audio_output_cores3.c、realtime_voice.c、airplay_service.c、app_core.c 四处，无统一协调者 |
| 环形缓冲区代码重复 | 🟡 中 | voice_reference.c 和 voice_playout.c 各自实现了几乎相同的环形缓冲区 |
| 组件接口泄漏实现细节 | 🟡 中 | audio_decoder.h 暴露解码器内部结构，board_common.h 泄漏板级实现 |
| 层级违规 | 🟡 中 | 部分 CMakeLists.txt 注释指出存在层级依赖违规 |
| 错误处理不一致 | 🟢 低 | 部分组件返回错误码，部分不返回 |
| 全局状态散落 | 🟢 低 | 多个组件使用 static 全局状态，无统一封装 |

### 资源冲突现状

当前 AirPlay + 语音助手的资源协调机制：

```
realtime_voice.c  →  audio_output_is_active()  →  判断是否阻塞语音
realtime_voice.c  →  audio_output_acquire_speaker()  →  获取扬声器
realtime_voice.c  →  audio_output_release_speaker()  →  释放扬声器
airplay_service.c →  playback_worker  →  控制音频管道启停
app_core.c        →  network状态变化  →  同步语音模式
```

**问题**：
1. 无统一资源管理器，仲裁逻辑分散
2. 语音获取扬声器后，AirPlay 无法抢占（即使语音已结束播放）
3. 音量管理分散：语音 boost + AirPlay restore 逻辑分散在多处
4. 状态同步依赖事件轮询，无主动通知机制

---

## 重构阶段

### 阶段 1：提取共享模块（风险最低，纯代码质量改善）

**目标**：消除代码重复，建立共享基础设施

**修改文件**：
- 新建 `components/realtime_voice/audio_ringbuf.c` + `audio_ringbuf.h`
- 修改 `components/realtime_voice/voice_reference.c` — 使用共享环形缓冲区
- 修改 `components/realtime_voice/voice_playout.c` — 使用共享环形缓冲区
- 修改 `components/realtime_voice/CMakeLists.txt` — 添加新源文件

**具体改动**：

1. 从 voice_reference.c 和 voice_playout.c 提取公共环形缓冲区实现：
   - `audio_ringbuf_init()` — 初始化（支持配置容量）
   - `audio_ringbuf_deinit()` — 销毁
   - `audio_ringbuf_push()` — 写入（两段 memcpy + 自旋锁）
   - `audio_ringbuf_pop()` — 读取（两段 memcpy + 自旋锁）
   - `audio_ringbuf_avail()` — 可读量
   - `audio_ringbuf_free()` — 可写量
   - `audio_ringbuf_reset()` — 重置

2. voice_reference.c 改为包装 audio_ringbuf，保留其特有的重采样和混音逻辑
3. voice_playout.c 改为包装 audio_ringbuf，保留其特有的 last_stereo 逻辑

**验收**：编译通过，功能行为不变

---

### 阶段 2：资源仲裁器（中等风险，最高影响）

**目标**：统一 AirPlay + 语音助手的资源冲突协调

**修改文件**：
- 新建 `components/audio_core/resource/resource_manager.c` + `resource_manager.h`
- 修改 `components/audio_core/audio/audio_output_cores3.c` — 扬声器所有权委托给 resource_manager
- 修改 `components/realtime_voice/realtime_voice.c` — 通过 resource_manager 请求资源
- 修改 `components/airplay_core/airplay_service.c` — 通过 resource_manager 注册播放状态
- 修改 `components/app_core/app_core.c` — 初始化 resource_manager
- 修改 `components/audio_core/CMakeLists.txt` — 添加新源文件

**资源管理器设计**：

```c
typedef enum {
  RESOURCE_OWNER_NONE = 0,
  RESOURCE_OWNER_AIRPLAY,
  RESOURCE_OWNER_VOICE,
} resource_owner_t;

typedef struct {
  resource_owner_t speaker_owner;
  bool airplay_active;
  bool voice_active;
  float airplay_volume_db;
  float voice_volume_db;
} resource_state_t;

void resource_manager_init(void);

// 请求扬声器（阻塞直到获取或超时）
bool resource_manager_acquire_speaker(resource_owner_t owner, int timeout_ms);

// 释放扬声器
void resource_manager_release_speaker(resource_owner_t owner);

// 通知 AirPlay 播放状态变化
void resource_manager_set_airplay_active(bool active);

// 通知语音活动状态变化
void resource_manager_set_voice_active(bool active);

// 查询当前状态
resource_state_t resource_manager_get_state(void);

// 注册状态变化回调
typedef void (*resource_change_cb_t)(const resource_state_t *state, void *ctx);
void resource_manager_register_callback(resource_change_cb_t cb, void *ctx);
```

**仲裁规则**：
1. AirPlay 播放中 → 语音被阻塞（当前行为保留）
2. AirPlay 空闲 → 语音可获取扬声器
3. 语音播放结束 → 自动释放扬声器，通知 AirPlay 恢复
4. AirPlay 开始播放 → 语音前端暂停（当前行为保留）
5. 音量切换统一管理：获取扬声器时切换到对应音量，释放时恢复

**验收**：AirPlay 播放 + 语音唤醒互不干扰，切换顺畅

---

### 阶段 3：接口清理与封装（中等风险）

**目标**：修复层级违规，隐藏实现细节，明确组件边界

**修改文件**：
- 修改 `components/audio_core/audio/audio_decoder.h` — 隐藏内部结构体
- 修改 `components/board_cores3/board_common.h` — 分离公共接口和内部实现
- 修改 `components/realtime_voice/realtime_voice.h` — 清理未实现的声明
- 修改各组件 CMakeLists.txt — 修复层级依赖违规
- 修改 `components/airplay_core/CMakeLists.txt` — 移除对 screen_ui 的不当依赖

**具体改动**：

1. **audio_decoder.h** — 将 `audio_decoder_t` 结构体定义移入 .c 文件，头文件只保留前向声明和 API
2. **board_common.h** — 分离为 `board.h`（公共接口）和 `board_internal.h`（内部实现）
3. **realtime_voice.h** — 移除未实现的函数声明
4. **CMakeLists.txt** — 修复注释中指出的层级违规，将不当依赖改为通过回调/事件解耦

**验收**：编译通过，无新增警告

---

### 阶段 4：性能优化（中等风险）

**目标**：减少关键路径延迟和内存峰值

**修改文件**：
- 修改 `components/realtime_voice/voice_request.c` — 流式 Base64 编码（边编码边发送）
- 修改 `components/realtime_voice/realtime_voice.c` — 优化 playout drain 路径
- 修改 `components/audio_core/audio/audio_output_common.c` — 减少播放任务中的内存拷贝

**具体改动**：

1. **voice_request.c** — 当前流程是：录音完成 → 编码WAV → Base64 → 构建JSON → HTTP发送。可以考虑：
   - 使用 chunked transfer encoding，边编码边发送
   - 但这需要重构 HTTP 客户端使用方式，风险较高
   - **保守方案**：保持当前一次性发送，但优化内存分配顺序（已在上次修改中完成）

2. **realtime_voice.c playout drain** — 当前每次 pop 后做重采样，可以批量 pop + 批量重采样减少函数调用开销

3. **audio_output_common.c** — 检查播放任务中是否有不必要的 memcpy，优化为零拷贝路径

**验收**：语音交互端到端延迟降低 10%+

---

### 阶段 5：代码质量统一（风险最低）

**目标**：统一编码风格、错误处理、日志规范

**修改文件**：
- 所有组件的 .c 文件

**具体改动**：

1. **错误处理统一**：
   - 所有返回 esp_err_t 的函数必须检查并传播错误
   - 统一使用 `ESP_RETURN_ON_ERROR()` 宏
   - 统一使用 `ESP_GOTO_ON_ERROR()` 进行资源清理

2. **日志规范**：
   - 统一 TAG 命名：与组件名一致
   - 统一日志级别：ERROR = 真正错误，WARN = 可恢复问题，INFO = 关键事件，DEBUG = 调试信息
   - 移除生产环境中的 DEBUG 日志（或改为条件编译）

3. **命名规范**：
   - 公共函数：`组件名_动词_名词()` 如 `voice_request_send_audio()`
   - 私有函数：`模块名_动词_名词()` 加 `static`
   - 类型：`组件名_名词_t`
   - 常量：`COMPONENT_NAME_CONSTANT`

4. **移除死代码**：
   - 搜索并移除未使用的 static 函数
   - 搜索并移除未引用的全局变量

**验收**：代码审查通过，无新增功能变化

---

## 实施优先级

| 阶段 | 风险 | 影响 | 预计工作量 |
|------|------|------|-----------|
| 阶段 1：共享模块 | 🟢 低 | 🟡 中 | 2h |
| 阶段 2：资源仲裁器 | 🟡 中 | 🔴 高 | 4h |
| 阶段 3：接口清理 | 🟡 中 | 🟡 中 | 3h |
| 阶段 4：性能优化 | 🟡 中 | 🟡 中 | 3h |
| 阶段 5：代码质量 | 🟢 低 | 🟢 低 | 4h |

**建议执行顺序**：阶段 1 → 阶段 2 → 阶段 3 → 阶段 5 → 阶段 4

理由：先建立基础设施（阶段1），再解决最关键的资源冲突（阶段2），然后清理接口（阶段3），统一代码质量（阶段5），最后做性能优化（阶段4，因为性能优化需要稳定的架构基础）。

---

## 每阶段验收标准

每个阶段完成后必须：
1. ✅ 编译通过（零错误）
2. ✅ 烧录后 AirPlay 播放正常
3. ✅ 烧录后语音助手正常（唤醒→识别→播放）
4. ✅ AirPlay + 语音互斥正常
5. ✅ 无内存泄漏（SPIRAM 空闲量稳定）
