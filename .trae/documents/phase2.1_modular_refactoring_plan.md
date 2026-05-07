
# Phase 2.1: 模块化重构计划

## 概述

当前 `realtime_voice.c` 已超过 3000 行，将其拆分为多个功能明确的模块，提升可维护性和可测试性。

---

## 当前文件分析

### 现有模块划分

| 模块 | 职责 | 行数估算 |
|------|------|
| voice_ws | WebSocket 连接管理 | ~400 |
| voice_audio | 音频采集/播放/重采样/AFE | ~700 |
| voice_vad | VAD 状态机/唤醒词 | ~300 |
| voice_tts | TTS 管线/缓冲/播放 | ~500 |
| voice_session | 会话管理/状态机 | ~400 |
| voice_protocol | DashScope 协议处理 | ~400 |
| realtime_voice | 顶层控制/主循环 | ~300 |

---

## 详细设计

### 1. voice_ws.h/c

**职责**：
- WebSocket 连接初始化和管理
- 连接重试逻辑
- 消息收发封装
- 心跳保活
- 连接状态跟踪

**导出接口：

```c
// voice_ws.h
typedef struct voice_ws voice_ws_t;

voice_ws_t* voice_ws_init(const char* url, const char* api_key);
void voice_ws_destroy(voice_ws_t* ws);

esp_err_t voice_ws_connect(voice_ws_t* ws);
void voice_ws_disconnect(voice_ws_t* ws);
bool voice_ws_is_connected(voice_ws_t* ws);

bool voice_ws_send_text(voice_ws_t* ws, const char* text, size_t len);
bool voice_ws_send_json(voice_ws_t* ws, cJSON* json);

int voice_ws_receive(voice_ws_t* ws, char* buf, size_t buf_size, int timeout_ms);
```

**内部状态：
- `s_ws_handle_t
- 连接重试状态
- 上一次发送/接收时间
- 累积缓冲区
- 发送/接收超时处理

### 2. voice_audio.h/c

**职责**：
- 麦克风/扬声器设备管理
- 音频重采样器
- AEC/AFE 处理
- 参考信号环路
- 音量控制

**导出接口：

```c
// voice_audio.h
typedef struct voice_audio voice_audio_t;

voice_audio_t* voice_audio_init(void);
void voice_audio_destroy(voice_audio_t* audio);

esp_err_t voice_audio_open_mic(voice_audio_t* audio);
void voice_audio_close_mic(voice_audio_t* audio);
esp_err_t voice_audio_open_spk(voice_audio_t* audio);
void voice_audio_close_spk(voice_audio_t* audio);

bool voice_audio_read_mic(voice_audio_t* audio, int16_t* samples, size_t frames);
bool voice_audio_write_spk(voice_audio_t* audio, const int16_t* samples, size_t frames);

#if CONFIG_VOICE_AFE_ENABLE
void voice_audio_push_ref(voice_audio_t* audio, const int16_t* samples, size_t frames);
bool voice_audio_process_afe(voice_audio_t* audio, const int16_t* mic, size_t frames, int16_t* out, size_t* out_frames);
#endif
```

### 3. voice_vad.h/c

**职责**：
- RMS / VADNet 检测
- 唤醒词检测
- VAD 状态机
- 端点检测

**导出接口**：

```c
// voice_vad.h
typedef enum {
    VOICE_VAD_STATE_IDLE,
    VOICE_VAD_STATE_START,
    VOICE_VAD_STATE_SPEECH,
    VOICE_VAD_STATE_END,
} voice_vad_state_t;

typedef struct voice_vad voice_vad_t;

voice_vad_t* voice_vad_init(void);
void voice_vad_destroy(voice_vad_t* vad);

void voice_vad_reset(voice_vad_t* vad);
voice_vad_state_t voice_vad_process(voice_vad_t* vad, const int16_t* samples, size_t frames);

bool voice_vad_is_wakeword_detected(voice_vad_t* vad);
```

### 4. voice_tts.h/c

**职责**：
- TTS 环形缓冲管理
- 预填充检查
- 播放/停止
- 软削波
- 间隙隐藏

**导出接口**：

```c
// voice_tts.h
typedef struct voice_tts voice_tts_t;

voice_tts_t* voice_tts_init(void);
void voice_tts_destroy(voice_tts_t* tts);

void voice_tts_reset(voice_tts_t* tts);
bool voice_tts_push(voice_tts_t* tts, const int16_t* pcm, size_t frames);
size_t voice_tts_get_avail(voice_tts_t* tts);
bool voice_tts_is_prefilled(voice_tts_t* tts);
size_t voice_tts_drain(voice_tts_t* tts, int16_t* out, size_t max_frames);
```

### 5. voice_session.h/c

**职责**：
- 会话状态机
- 会话上下文管理
- 状态转换
- 超时处理

**导出接口**：

```c
// voice_session.h
typedef enum {
    VOICE_SESSION_STANDBY,
    VOICE_SESSION_CONNECTING,
    VOICE_SESSION_LISTENING,
    VOICE_SESSION_DETECTING,
    VOICE_SESSION_SENDING,
    VOICE_SESSION_THINKING,
    VOICE_SESSION_SPEAKING,
    VOICE_SESSION_ERROR,
} voice_session_state_t;

typedef struct voice_session voice_session_t;

voice_session_t* voice_session_init(void);
void voice_session_destroy(voice_session_t* sess);

void voice_session_reset(voice_session_t* sess);
voice_session_state_t voice_session_get_state(voice_session_t* sess);
void voice_session_set_state(voice_session_t* sess, voice_session_state_t state);

void voice_session_mark_active(voice_session_t* sess, uint64_t now_ms);
bool voice_session_is_expired(voice_session_t* sess, uint64_t now_ms, uint64_t timeout_ms);
```

### 6. voice_protocol.h/c

**职责**：
- 消息构建和解析
- 会话更新构建
- `response.create` 发送
- 消息解析
- 工具调用处理

**导出接口**：

```c
// voice_protocol.h
typedef struct voice_protocol voice_protocol_t;

voice_protocol_t* voice_protocol_init(void);
void voice_protocol_destroy(voice_protocol_t* proto);

cJSON* voice_protocol_build_session_update(voice_protocol_t* proto, const char* model, bool enable_tools);
cJSON* voice_protocol_build_response_create(voice_protocol_t* proto);
cJSON* voice_protocol_build_audio_append(voice_protocol_t* proto, const int16_t* pcm, size_t frames);

bool voice_protocol_parse_message(voice_protocol_t* proto, const char* message, size_t len,
                                  void (*on_audio)(const uint8_t* pcm, size_t len),
                                  void (*on_text)(const char* text),
                                  void (*on_function_call)(const char* name, const char* call_id, const char* args));
```

### 7. realtime_voice.h/c

**职责**：
- 顶层协调模块
- 主循环
- 任务管理
- 生命周期管理

---

## 实施步骤

| 步骤 | 动作 | 风险 |
|------|------|
| 1 | 创建所有模块的头文件定义 | 低 |
| 2 | 实现 voice_ws 模块 | 中 |
| 3 | 实现 voice_audio 模块 | 高 |
| 4 | 实现 voice_vad 模块 | 中 |
| 5 | 实现 voice_tts 模块 | 中 |
| 6 | 实现 voice_session 模块 | 中 |
| 7 | 实现 voice_protocol 模块 | 高 |
| 8 | 重构 realtime_voice.c 为顶层 | 高 |
| 9 | 更新 CMakeLists.txt | 低 |
| 10 | 构建和验证 | 中 |

---

## 风险与注意事项

- 保持现有功能和接口不变，重构内部实现
- 逐步引入模块间通过指针传递上下文，避免过多全局静态
- 保留 CONFIG_* 配置不变
- 验证编译通过后再替换到实际硬件测试
- 确保和现有 AFE/VADNet 改动（phase1) 兼容

