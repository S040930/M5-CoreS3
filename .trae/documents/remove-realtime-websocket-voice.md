# 删除 Realtime WebSocket 全双工语音路径方案

## Summary

删除 Realtime WebSocket 全双工语音路径，将语音交互统一到 one-shot HTTP Chat Completions 路径。同时将 voice_tools/voice_timers 的工具调用能力集成到 HTTP 路径中，使用 DashScope Chat Completions 原生 function calling 实现。

## Current State Analysis

项目存在两条语音交互路径：

1. **WebSocket 实时路径**（要删除）：`voice_ws.c/h` 负责 WebSocket 连接管理、`session.update`/`response.create` 等 Realtime API 协议交互、服务端推送的 `response.audio.delta` 音频流接收、function calling 工具调用处理。此路径使用 `esp_websocket_client` 库，配置项包括 `CONFIG_VOICE_REALTIME_URL`（wss://）、`CONFIG_VOICE_WS_*`、`CONFIG_VOICE_TTS_VOICE`、`CONFIG_VOICE_ENABLE_INPUT_TRANSCRIPTION` 等。

2. **One-shot HTTP 路径**（保留并增强）：`voice_request.c` 负责 HTTP Chat Completions 请求，将录音 PCM 编码为 WAV → base64 → 发送到 DashScope Chat Completions API → SSE 流式接收文本和音频 delta → 播放。当前系统提示词硬编码在 `VOICE_REQUEST_BODY_PREFIX` 中，不支持 tools。

**关键依赖关系：**
- `realtime_voice.c`（主任务循环）同时调用 WS 和 one-shot 路径
- `voice_session.c` 的健康监控引用 `voice_ws_ctx()` 的连接状态
- `voice_tools.c/h` 和 `voice_timers.c/h` 仅通过 WS 路径的 function calling 触发
- `realtime_voice_internal.h` 中 `realtime_ctx_t` 包含 `#if CONFIG_VOICE_TOOLS_ENABLE` 条件字段
- `app_core.c` 使用 `CONFIG_VOICE_REALTIME_URL` 配置语音 URL
- `screen_ui.h` 定义了 `SCREEN_UI_VOICE_CONNECTING` 状态
- `CMakeLists.txt` 依赖 `esp_websocket_client`
- `idf_component.yml` 声明 `espressif/esp_websocket_client: "^1.4.0"` 依赖
- `pio_prebuild.py` 映射了 `[voice.websocket]`、`voice.tts_voice`、`voice.transcription`、`voice.tools.enabled` 等配置项
- `config.toml` 包含 `[voice.websocket]` 段和相关字段

## Proposed Changes

### 1. 删除 `voice_ws.c` 和 `voice_ws.h`

**文件：** `components/realtime_voice/voice_ws.c`、`components/realtime_voice/voice_ws.h`

- 完整删除这两个文件
- 这是 WebSocket 路径的核心，包含：WS 连接管理、`session.update` 构建、`response.create` 发送、`handle_realtime_message` 事件处理、function calling 响应处理

### 2. 修改 `realtime_voice.c` — 移除 WS 依赖

**文件：** `components/realtime_voice/realtime_voice.c`

- 移除 `#include "voice_ws.h"`
- 移除 `voice_ws_init()` 调用（L430）
- 移除 `voice_ws_reset_retry()` 调用（L183）
- 移除 `voice_ws_disconnect()` 调用（L213, L256, L398）
- 移除 `voice_ws_deinit()` 调用（L400）
- 移除 `voice_ws_ctx()->ws != NULL` 检查（L246）
- 移除 standby 分支中的 WS 断开逻辑（L213）
- 移除 AirPlay gate 分支中的 WS 断开逻辑（L246-256）
- 移除 teardown 中的 WS 清理（L398-400）
- 保留 `CONFIG_VOICE_REALTIME_URL` 的读取逻辑，但 URL 现在指向 HTTP 端点

### 3. 修改 `voice_session.c` — 移除 WS 健康监控引用

**文件：** `components/realtime_voice/voice_session.c`

- 移除 `#include "voice_ws.h"`
- 移除 `voice_session_loop_drain_and_monitor` 中对 `voice_ws_ctx()` 的引用（L289, L291, L298, L302-303, L304, L310, L331-332）
  - `last_append_ms` / `append_ok_count` 监控 → 删除（仅 WS 路径有意义）
  - `connected` / `session_ready` / `last_ws_recv_ms` 监控 → 删除
  - `CONFIG_VOICE_WS_PING_INTERVAL_SEC` watchdog → 删除

### 4. 修改 `voice_request.c` — 集成 tools 支持

**文件：** `components/realtime_voice/voice_request.c`

**4a. 重构请求体构建：**

当前 `VOICE_REQUEST_BODY_PREFIX` 和 `VOICE_REQUEST_BODY_MID` 是硬编码的静态字符串。需要改为动态构建请求体，以支持：
- 可配置的系统提示词（从 prompt_preset 移植）
- tools schema（从 `voice_tools_append_session_schemas` 移植）
- function calling 响应处理

**4b. 添加 tools schema 到请求体：**

在 `build_request_body` 中，当 `CONFIG_VOICE_TOOLS_ENABLE` 时，调用 `voice_tools_append_session_schemas` 将工具定义加入请求。需要先将 tools schemas 构建为 cJSON 数组，再序列化为字符串嵌入请求体。

**4c. 添加 function calling 响应处理：**

在 SSE 流解析 `stream_handle_sse_line` 中，检测 `delta.tool_calls` 字段。当收到完整的 tool call（`finish_reason="tool_calls"`）时：
1. 解析 `tool_calls[i].function.name` 和 `tool_calls[i].function.arguments`
2. 调用 `voice_tools_dispatch` 执行工具
3. 构建新的 HTTP 请求，messages 中包含 assistant 的 tool_calls 消息和 tool 角色的执行结果
4. 发起新的 HTTP 请求获取最终回复

**4d. 移植 prompt_preset 逻辑：**

将 `voice_ws.c` 中的 `prompt_preset_instructions()` 和 `build_session_instructions()` 移植到 `voice_request.c`，替换当前硬编码的系统提示词。

**4e. 修改 URL 处理：**

`endpoint_from_url` 已支持 HTTP 端点推导。`voice.url` 配置改为 HTTP 端点后，此函数无需修改。

### 5. 修改 `realtime_voice_internal.h` — 保留 tools 字段

**文件：** `components/realtime_voice/realtime_voice_internal.h`

- 保留 `#if CONFIG_VOICE_TOOLS_ENABLE` 条件字段（`server_response_active`, `tool_call_inflight`, `tool_followup_pending`, `last_tool_call_id`），因为 HTTP 路径的工具调用也需要这些状态
- 可能需要新增字段来跟踪 HTTP 路径的 tool call 状态（如 `http_tool_call_id`、`http_tool_call_name`、`http_tool_call_arguments`）

### 6. 修改 `realtime_voice.h` — 重新定义 CONNECTING 状态

**文件：** `components/realtime_voice/realtime_voice.h`

- 保留 `REALTIME_VOICE_STATE_CONNECTING` 枚举值
- 重新定义其语义为"HTTP 请求建立连接中"
- 在 `voice_request_send_audio` 发起 HTTP 请求前，设置状态为 CONNECTING

### 7. 修改 `CMakeLists.txt` — 移除 WS 依赖

**文件：** `components/realtime_voice/CMakeLists.txt`

- 从 SRCS 中移除 `"voice_ws.c"`
- 从 REQUIRES 中移除 `esp_websocket_client`

### 8. 修改 `idf_component.yml` — 移除 WS 依赖

**文件：** `components/realtime_voice/idf_component.yml`

- 移除 `espressif/esp_websocket_client: "^1.4.0"` 依赖行

### 9. 修改 `pio_prebuild.py` — 清理 WS 配置映射

**文件：** `scripts/pio_prebuild.py`

从 `CONFIG_MAP` 中移除以下条目：
- `"voice.tts_voice"` → `CONFIG_VOICE_TTS_VOICE`（仅 WS 路径使用）
- `"voice.websocket.connect_timeout_ms"` → `CONFIG_VOICE_WS_CONNECT_TIMEOUT_MS`
- `"voice.websocket.retry_base_ms"` → `CONFIG_VOICE_WS_RETRY_BASE_MS`
- `"voice.websocket.retry_max_ms"` → `CONFIG_VOICE_WS_MAX_MS`
- `"voice.websocket.retry_jitter_ms"` → `CONFIG_VOICE_WS_RETRY_JITTER_MS`
- `"voice.websocket.ping_interval_sec"` → `CONFIG_VOICE_WS_PING_INTERVAL_SEC`
- `"voice.websocket.send_timeout_ms"` → `CONFIG_VOICE_WS_SEND_TIMEOUT_MS`

保留以下条目（HTTP 路径仍需要）：
- `"voice.url"` → `CONFIG_VOICE_REALTIME_URL`（值改为 HTTP 端点）
- `"voice.api_key"` → `CONFIG_VOICE_API_KEY`
- `"voice.model"` → `CONFIG_VOICE_MODEL`
- `"voice.tools.enabled"` → `CONFIG_VOICE_TOOLS_ENABLE`
- `"voice.transcription.enabled"` → `CONFIG_VOICE_ENABLE_INPUT_TRANSCRIPTION`（保留，HTTP 路径可能使用）
- `"voice.transcription.model"` → `CONFIG_VOICE_INPUT_TRANSCRIPTION_MODEL`

从 `STATIC_DEFAULTS` 中移除：
- `"CONFIG_ESP_WS_CLIENT_ENABLE_DYNAMIC_BUFFER=y"`
- `"CONFIG_ESP_WS_CLIENT_SEPARATE_TX_LOCK=y"`

### 10. 修改 `config.toml` 和 `config.toml.example`

**文件：** `config/config.toml`、`config/config.toml.example`

- 将 `voice.url` 默认值从 `wss://dashscope.aliyuncs.com/api-ws/v1/realtime` 改为 `https://dashscope.aliyuncs.com/compatible-mode/v1`
- 将 `voice.model` 从 `qwen3.5-omni-flash-realtime` 改为 `qwen3.5-omni-flash`（HTTP Chat Completions 模型名）
- 删除 `[voice.websocket]` 整段
- 删除 `voice.tts_voice` 字段（HTTP 路径的 TTS voice 通过请求体中的 `audio.voice` 字段控制，已在 voice_request.c 中硬编码为 "Tina"，可后续配置化）
- 保留 `[voice.tools]` 和 `[voice.transcription]`

### 11. 修改 `app_core.c` — 适配 URL 变更

**文件：** `components/app_core/app_core.c`

- `load_voice_config` 和 `provision_voice_config_if_needed` 中对 `CONFIG_VOICE_REALTIME_URL` 的引用保持不变（字段名不变，只是值从 wss:// 改为 https://）
- L378 的 `esp_websocket_client` 注释 → 删除

### 12. 修改 `settings.c` — 保留 voice_url 字段

**文件：** `components/app_core/settings.c`

- `voice_url` NVS 字段保持不变，只是存储的值从 wss:// 端点改为 https:// 端点
- 无需代码修改

### 13. 修改 `voice_session.c` — 适配 one-shot 主循环

**文件：** `components/realtime_voice/voice_session.c`

- `voice_session_complete_recording` 中的 one-shot 请求逻辑保持不变
- 但需要支持工具调用的多轮对话：当 `voice_request_send_audio` 返回 tool_calls 时，需要循环执行工具并重新请求

### 14. 更新文档

**文件：** `docs/voice-interaction.md`

- 移除所有 WebSocket 相关描述
- 更新为 HTTP one-shot 路径的架构说明
- 更新配置项说明

## Assumptions & Decisions

1. **仅删除 WS，保留 Tools/Timers**：voice_tools.c/h 和 voice_timers.c/h 保留，通过 Chat Completions 原生 function calling 集成到 HTTP 路径
2. **完整清理 WS 配置**：config.toml 中的 `[voice.websocket]` 段、`tts_voice` 等仅用于 WS 的配置项全部删除
3. **URL 改为 HTTP 端点**：voice.url 默认值改为 `https://dashscope.aliyuncs.com/compatible-mode/v1`
4. **CONNECTING 状态重新定义**：用于表示 HTTP 请求建立连接中
5. **Chat Completions 原生 function calling**：使用 DashScope API 的 `tools` 参数和 `tool_calls` 响应处理
6. **TTS voice 硬编码**：当前 voice_request.c 中 `audio.voice` 已硬编码为 "Tina"，暂不配置化
7. **model 名称变更**：从 `qwen3.5-omni-flash-realtime` 改为 `qwen3.5-omni-flash`

## Verification Steps

1. **编译验证**：`~/.platformio/penv/bin/pio run -e m5cores3` 确保无编译错误
2. **功能验证**：
   - 语音唤醒后能正常录音并发送 HTTP 请求
   - 收到音频回复后能正常播放
   - 工具调用（如"几点了"、"设个5分钟定时器"）能正常触发并执行
   - 工具执行后能继续对话
3. **配置验证**：确认 `config/generated/sdkconfig.defaults` 中不再包含 `CONFIG_VOICE_WS_*` 和 `CONFIG_VOICE_TTS_VOICE` 条目
4. **依赖验证**：确认 `esp_websocket_client` 不再被编译进固件（检查 .pio/build 目录）
5. **UI 验证**：语音交互时 UI 状态正确显示（LISTENING → CONNECTING → THINKING → SPEAKING）
