# 项目架构重构 Spec

## Why

当前项目存在三类结构性问题：1) `realtime_voice.c` 是 1942 行的上帝模块，承担了 WebSocket 管理、扬声器仲裁、音量控制、播放 drain、录音、工具函数 JSON Schema、会话管理、AirPlay 门控、健康监控等职责；2) 环形缓冲区代码在 `voice_playout.c` 和 `voice_reference.c` 中重复实现，且 `voice_reference.c` 仍使用逐样本拷贝而非两段 memcpy；3) 资源仲裁逻辑散落在 `audio_output`、`realtime_voice`、`airplay_service`、`app_core` 四处，无统一协调者。这些问题导致新功能难以安全添加、bug 难以定位、组件间依赖关系混乱。

## What Changes

- 提取共享环形缓冲区模块 `audio_ringbuf`，消除 `voice_playout` 和 `voice_reference` 的重复实现
- 提取共享基础设施模块 `voice_common`（内存分配、时间工具），消除跨文件重复定义
- 将 `realtime_voice.c` 拆分为 4 个职责单一的子模块：`voice_session`（会话/状态机）、`voice_speaker`（扬声器仲裁/音量）、`voice_playout_drain`（播放 drain/重采样/软限幅/间隙掩盖）、`voice_ws`（WebSocket 连接/消息处理）
- 将工具函数 JSON Schema 定义从 `realtime_voice.c` 移入 `voice_tools.c`
- 统一 AirPlay + 语音资源仲裁器 `resource_manager`
- 修复 `voice_reference.c` 环形缓冲区逐样本拷贝为两段 memcpy
- 消除 `resampler_bridge.c` 的 `.c` 文件 include 反模式
- 清理 `realtime_voice` 对 `airplay_core`、`screen_ui`、`network_core` 的直接依赖，改为通过回调/事件解耦
- 将 `voice_request.c` 硬编码模型名改为使用 config 参数

## Impact

- Affected specs: 语音交互集成、AirPlay/Voice 互斥门控
- Affected code:
  - `components/realtime_voice/` — 全部文件重构
  - `components/audio_core/` — 新增 `resource/` 子目录
  - `components/airplay_core/airplay_service.c` — 委托资源管理
  - `components/app_core/app_core.c` — 初始化资源管理器

## ADDED Requirements

### Requirement: 共享环形缓冲区模块

系统 SHALL 提供 `audio_ringbuf` 模块，封装线程安全的 SPSC 环形缓冲区。

#### Scenario: 初始化与使用
- **WHEN** 调用 `audio_ringbuf_init(capacity_samples)` 
- **THEN** 返回一个环形缓冲区句柄，内部使用 SPIRAM 优先分配，两段 memcpy 读写，portMUX_TYPE 自旋锁保护

#### Scenario: 替换 voice_playout 和 voice_reference
- **WHEN** `voice_playout.c` 和 `voice_reference.c` 改为使用 `audio_ringbuf`
- **THEN** 行为与当前一致，但 `voice_reference.c` 的逐样本拷贝被消除

### Requirement: 共享基础设施模块 voice_common

系统 SHALL 提供 `voice_common.h`，包含：
- `voice_buf_alloc()` / `voice_buf_free()` — SPIRAM 优先内存分配
- `voice_now_ms()` — 统一毫秒时间戳
- `voice_hw_codec_rate_hz()` / `voice_hw_mclk_multiple()` — 硬件参数查询

#### Scenario: 消除重复定义
- **WHEN** `voice_dsp.c`、`realtime_voice.c`、`voice_frontend.c` 中的 `voice_buf_alloc`/`voice_buf_free`/`now_ms` 被替换为 `voice_common` 的版本
- **THEN** 编译通过，行为不变

### Requirement: realtime_voice.c 拆分

`realtime_voice.c` SHALL 被拆分为以下子模块：

1. **`voice_session.c`** — 会话状态机、录音逻辑、VAD 判断、oneshot 问答流程
2. **`voice_speaker.c`** — 扬声器获取/释放、音量 boost/restore、codec open/close
3. **`voice_playout_drain.c`** — 播放 drain（重采样、软限幅、单声道→立体声、间隙掩盖、诊断日志）
4. **`voice_ws.c`** — WebSocket 连接/断连/重试、消息累积与分发、session.update 构建

`realtime_voice.c` 保留为编排层，仅包含 `voice_task` 主循环和公共 API。

#### Scenario: 编译与功能验证
- **WHEN** 拆分完成后编译
- **THEN** 零错误，烧录后 AirPlay 播放正常、语音助手正常（唤醒→识别→播放）、互斥正常

### Requirement: 工具函数 JSON Schema 移入 voice_tools

`realtime_voice.c` 中的 `voice_append_session_tools()` 函数（约 160 行 JSON Schema 构建）SHALL 移入 `voice_tools.c`。

#### Scenario: 编译验证
- **WHEN** 移动完成后
- **THEN** `realtime_voice.c` 不再包含 cJSON Schema 构建代码，工具功能不变

### Requirement: 资源仲裁器 resource_manager

系统 SHALL 提供 `resource_manager` 模块，统一协调 AirPlay 和语音助手的扬声器资源。

#### Scenario: 扬声器获取
- **WHEN** 语音助手请求扬声器且 AirPlay 未占用
- **THEN** 资源管理器授权获取，自动切换到语音音量

#### Scenario: AirPlay 优先
- **WHEN** AirPlay 开始播放
- **THEN** 资源管理器通知语音前端暂停，语音释放扬声器

#### Scenario: 语音释放后恢复
- **WHEN** 语音播放结束释放扬声器
- **THEN** 资源管理器通知 AirPlay 恢复播放

### Requirement: 修复 voice_reference.c 逐样本拷贝

`voice_reference_ring_push` 和 `voice_reference_ring_pop` SHALL 使用两段 memcpy 替代逐样本循环。

#### Scenario: 性能验证
- **WHEN** 修复完成后
- **THEN** 临界区内拷贝操作使用 memcpy，与 voice_playout 一致

### Requirement: 消除 resampler_bridge.c 反模式

`resampler_bridge.c` 当前使用 `#include "../audio_core/resampler/resampler.c"` 直接包含 .c 文件。SHALL 改为通过 CMakeLists.txt 正确链接 `resampler` 源文件。

#### Scenario: 编译验证
- **WHEN** 修改后
- **THEN** 无 .c 文件直接 include，resampler 通过正常编译单元链接

### Requirement: 清理跨层依赖

`realtime_voice` 组件 SHALL NOT 直接依赖 `airplay_core`、`screen_ui`、`network_core`。改为：
- 通过回调函数通知 UI 状态变化（由 `app_core` 注册）
- 通过 `resource_manager` 查询 AirPlay 状态
- 通过回调获取网络状态

#### Scenario: CMakeLists.txt 依赖清理
- **WHEN** 清理完成后
- **THEN** `realtime_voice/CMakeLists.txt` 的 REQUIRES 列表不再包含 `airplay_core`、`screen_ui`、`network_core`

### Requirement: voice_request 模型名参数化

`voice_request.c` 中的 `#define VOICE_REQUEST_MODEL "qwen3.5-omni-flash"` SHALL 改为从 `voice_request_config_t` 的 `model` 字段读取。

#### Scenario: 使用配置模型名
- **WHEN** 调用 `voice_request_send_audio` 时 `cfg->model` 非空
- **THEN** 使用 `cfg->model` 作为请求模型名

## MODIFIED Requirements

### Requirement: voice_dsp 模块职责

`voice_dsp.c` 原包含内存分配辅助函数和硬件参数查询。修改后仅保留重采样器管理职责，内存分配和硬件查询移入 `voice_common`。

### Requirement: voice_playout 模块

`voice_playout.c` 原自行实现环形缓冲区。修改后内部使用 `audio_ringbuf` 实例，保留 playout 特有的 last_stereo 缓存和 prefill 逻辑。

### Requirement: voice_reference 模块

`voice_reference.c` 原自行实现环形缓冲区（逐样本拷贝）。修改后内部使用 `audio_ringbuf` 实例，保留重采样和 AirPlay tap 逻辑。

## REMOVED Requirements

### Requirement: resampler_bridge.c 编译桥接
**Reason**: `.c` 文件直接 include 是反模式，改为通过 CMakeLists.txt 正确链接
**Migration**: 删除 `resampler_bridge.c`，在 CMakeLists.txt 中直接添加 resampler 源文件路径
