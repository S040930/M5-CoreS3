# 语音助手优化方案

## 零、当前模型分析：qwen3.5-omni-flash-realtime

### 0.1 模型定位与能力边界

当前接入的 `qwen3.5-omni-flash-realtime` 是 DashScope Realtime API 的 **Flash** 级别模型，定位为低延迟、高性价比的实时语音交互模型。

**模型能力矩阵**：

| 能力                   | qwen3.5-omni-flash-realtime | qwen3.5-omni-plus-realtime | 说明             |
| -------------------- | --------------------------- | -------------------------- | -------------- |
| server\_vad          | ✅ 支持                        | ✅ 支持                       | 基于声学特征的 VAD    |
| semantic\_vad        | ❌ 不支持                       | ✅ 支持                       | 基于语义有效性过滤无意义语音 |
| enable\_search       | ❌ 不支持                       | ✅ 支持                       | 联网搜索           |
| smooth\_output       | ✅ 支持                        | ✅ 支持                       | 口语化回复风格        |
| function\_calling    | ✅ 支持                        | ✅ 支持                       | 工具调用           |
| input\_transcription | ✅ 支持                        | ✅ 支持                       | 用户语音转文字        |
| 延迟                   | \~300-500ms                 | \~500-800ms                | Flash 更快       |
| 价格                   | 低                           | 中                          | Flash 更便宜      |

**关键发现**：

1. **semantic\_vad 不可用**：`qwen3.5-omni-flash-realtime` 不支持 `semantic_vad`，仅支持 `server_vad`。`semantic_vad` 能过滤"嗯""啊"等无意义回应语和背景噪声，只有 `qwen3.5-omni-plus-realtime` 支持。
2. **server\_vad 与本地唤醒词冲突**：当前代码中 `CONFIG_VOICE_TURN_SERVER_VAD` 与 `CONFIG_VOICE_ACTIVATION_PHRASE_ENABLE` 互斥——启用唤醒词时只能用客户端 VAD。这是因为 server\_vad 模式下服务端自动检测语音起止，与本地唤醒词的"先唤醒再上传"逻辑矛盾。
3. **turn\_detection 参数可调**：
   - `turn_detection_threshold`：-1.0\~1.0，默认 0.5，嘈杂环境增大、安静环境降低
   - `turn_detection_silence_duration_ms`：200\~6000ms，默认 800ms
4. **smooth\_output**：Flash 模型支持此参数，可让回复更口语化，适合语音交互场景。

### 0.2 当前 VAD 策略的困境

```
┌──────────────────────────────────────────────────────────────┐
│ 当前 VAD 策略选择困境                                        │
├──────────────────────────────────────────────────────────────┤
│                                                              │
│ 方案 A：客户端 VAD (RMS) + 本地唤醒词 ← 当前默认             │
│   ✅ 唤醒词可用                                              │
│   ❌ RMS VAD 极不准确                                        │
│   ❌ 无 AEC，打断不可靠                                      │
│   ❌ 需要手动 commit + response.create                       │
│                                                              │
│ 方案 B：server_vad ← 当前可选                                │
│   ✅ 服务端 VAD 更准确                                       │
│   ✅ 自动检测语音起止                                        │
│   ❌ 与本地唤醒词互斥                                        │
│   ❌ 无 AEC 时回声会触发服务端 VAD                           │
│   ❌ 全程上传音频，耗流量                                    │
│                                                              │
│ 方案 C：semantic_vad ← Flash 模型不可用                      │
│   ✅ 过滤无意义语音                                          │
│   ✅ 最智能的 VAD                                            │
│   ❌ qwen3.5-omni-flash-realtime 不支持                     │
│                                                              │
│ ★ 目标方案：本地 AFE(VADNet+AEC) + 客户端 VAD + 本地唤醒词  │
│   ✅ 唤醒词可用                                              │
│   ✅ VADNet 神经网络 VAD 准确率高                            │
│   ✅ AEC 消除回声，打断可靠                                  │
│   ✅ 仅唤醒后上传音频，省流量                                │
│   ✅ 不依赖服务端 VAD 能力                                   │
│                                                              │
└──────────────────────────────────────────────────────────────┘
```

### 0.3 模型升级建议

| 场景   | 推荐模型                          | 理由                                       |
| ---- | ----------------------------- | ---------------------------------------- |
| 当前默认 | `qwen3.5-omni-flash-realtime` | 低延迟、低成本，配合本地 AFE 足够                      |
| 高端体验 | `qwen3.5-omni-plus-realtime`  | 支持 semantic\_vad + enable\_search，语义理解更强 |
| 成本敏感 | `qwen3.5-omni-flash-realtime` | Flash 价格更低，延迟更小                          |

**建议**：保持 `qwen3.5-omni-flash-realtime` 作为默认模型，在 `config.toml` 中支持配置切换到 `qwen3.5-omni-plus-realtime`。当检测到 plus 模型时，自动启用 `semantic_vad` 作为 VAD 策略选项。

***

## 一、现有语音流程分析

### 1.1 当前架构总览

```
麦克风 (ES7210) → I2S 采集 → 立体声转单声道 → 重采样(硬件频率→16kHz)
  → [AFE 可选: AEC+NS+AGC+VAD] → [WakeNet: "Hi ESP" 唤醒]
  → 客户端 VAD (RMS 阈值检测) → Base64 编码 → WebSocket 上行
  → DashScope qwen3.5-omni-flash-realtime
  → WebSocket 下行 → Base64 解码 → TTS 环形缓冲 → 重采样(24kHz→硬件频率)
  → 软限幅 → 单声道转立体声 → I2S 输出 → 扬声器 (AW88298)
```

### 1.2 当前状态机

```
OFF → STANDBY → CONNECTING → LISTENING → DETECTING_SPEECH → SENDING → THINKING → SPEAKING
                                                                                   ↑         ↓
                                                                              INTERRUPTED ←─┘
                                                                                   ↓
                                                                                 ERROR
```

### 1.3 现有组件清单

| 组件     | 文件                  | 行数     | 职责                                     |
| ------ | ------------------- | ------ | -------------------------------------- |
| 核心引擎   | realtime\_voice.c   | \~2820 | 状态机、WebSocket、音频采集/播放、VAD、TTS          |
| 唤醒词    | wakeword\_omi.c     | \~152  | WakeNet wn9\_hiesp 封装                  |
| AFE 前端 | afe\_bridge.c       | \~157  | ESP-SR AFE 桥接 (AEC+NS+AGC+VAD+WakeNet) |
| 工具调用   | voice\_tools.c      | \~227  | DashScope Function Calling             |
| 定时器    | voice\_timers.c     | \~185  | esp\_timer 单次定时器                       |
| 重采样    | resampler\_bridge.c | \~20   | 重采样器桥接                                 |

### 1.4 关键问题识别

#### 问题 1：客户端 VAD 过于原始（严重）

**现状**：使用 RMS 阈值检测语音活动，这是最简单的能量检测方法。

```c
uint32_t rms = rms_of_i16(pcm, api_frames);
bool voice = rms >= (uint32_t)CONFIG_VOICE_VAD_RMS_THRESHOLD;
```

**问题**：

- 无法区分人声和其他声音（空调、风扇、敲击声等都会误触发）
- 对环境噪声极其敏感，阈值难以调优
- 远距离语音信噪比低，RMS 无法有效检测
- 连续帧门控（`vad_consecutive_hits`）只是简单计数，没有语音特征判断

**业界对比**：主流智能音响全部使用神经网络 VAD：

- ESP-SR V2.0 已发布 **VADNet**，基于 15000 小时数据训练，远超 WebRTC VAD
- Amazon Echo 使用云端+本地混合 VAD
- Google Nest 使用 Neural VAD + 端点检测器

#### 问题 2：AFE 非默认启用，AEC 缺失导致打断不可靠（严重）

**现状**：AFE 是编译时可选项（`CONFIG_VOICE_AFE_ENABLE`），默认关闭。关闭时：

- 无回声消除（AEC），TTS 播放期间麦克风拾取扬声器声音
- 打断机制依赖 RMS 阈值 × 12 倍乘数，极不可靠
- 300ms 回声冷却期只是简单延迟，无法真正消除回声

```c
#define VOICE_PLAYBACK_INTERRUPT_MULTIPLIER 12U
#define VOICE_PLAYBACK_INTERRUPT_MIN_RMS 1200U
// ...
if (!s_ctx.speaking && s_ctx.speaking_ended_ms > 0 &&
    (read_now_ms - s_ctx.speaking_ended_ms) < 300) {
  // 简单延迟，不处理回声
  continue;
}
```

**问题**：

- TTS 播放时用户打断成功率极低
- 播放结束后 300ms 内无法响应（冷却期）
- 回声残留导致 VAD 误触发

**业界对比**：所有主流智能音响 AEC 都是**始终开启**的核心组件，不是可选项。

#### 问题 3：单文件巨型引擎，职责不清晰（中等）

**现状**：`realtime_voice.c` 约 2820 行，包含：

- WebSocket 连接管理
- 音频采集与播放
- VAD 状态机
- TTS 环形缓冲
- 重采样管理
- Base64 编解码
- JSON 消息处理
- Function Calling 调度
- AirPlay 仲裁
- 会话管理

**问题**：

- 修改任何功能都需要理解整个文件
- 测试困难，无法单独测试各模块
- 新增功能容易引入回归

#### 问题 4：无本地语音命令识别（中等）

**现状**：所有语音输入都发送到云端 DashScope，即使是最简单的"音量大一点"也需要：

1. WebSocket 连接
2. 音频上传
3. 云端 ASR + NLU
4. 云端 TTS
5. 音频下载播放

**问题**：

- 网络延迟 500ms-2s，体验差
- 网络断开时完全不可用
- 简单命令浪费云端资源和带宽

**业界对比**：

- Amazon Echo：Local Voice Control (LVC) 支持离线基本命令
- Google Nest：Local Actions 支持离线设备控制
- 小爱同学：本地命令词识别，快速响应常见操作

ESP-SR 已提供 **MultiNet7** 命令词识别引擎，可在本地识别"音量大一点""暂停播放"等命令。

#### 问题 5：无持续对话模式（中等）

**现状**：每次唤醒后只支持一轮对话，用户需要再次说"Hi ESP"才能继续。

```c
if (s_ctx.session_state == VOICE_SESSION_ACTIVE &&
    s_ctx.session_last_active_ms > 0 &&
    (read_now_ms - s_ctx.session_last_active_ms) >=
        (uint64_t)CONFIG_VOICE_SESSION_IDLE_TIMEOUT_MS) {
  session_arm_set(false);
  realtime_voice_reset_session();
}
```

**问题**：

- 多轮对话需要反复唤醒，体验割裂
- 上下文记忆仅 256 字符，容易丢失

**业界对比**：

- Google Assistant：Continued Conversation 模式，回答后继续监听
- Alexa：Follow-up Mode，8 秒窗口内无需重新唤醒
- 小爱同学：连续对话模式

#### 问题 6：ESP-SR 版本利用不充分（中等）

**现状**：项目依赖 `espressif/esp-sr: 2.3.1`，但未使用 V2.0 新特性：

- 未使用 **VADNet**（仍用 WebRTC VAD 或 RMS）
- 未使用 **BSS**（盲源分离/波束成形，需双麦）
- 未使用 **DOA**（声源定位）
- 未使用 **MultiNet7**（本地命令词识别）
- 未使用 **nsnet1**（深度噪声抑制）

#### 问题 7：唤醒词单一且固定（低）

**现状**：仅支持 "Hi ESP" 唤醒词（`wn9_hiesp`）。

**问题**：

- 用户体验无法个性化
- ESP-SR 已支持 30+ 唤醒词模型，包括"小爱同学""Alexa""Jarvis"等

#### 问题 8：无 AirPlay 播放控制集成（低）

**现状**：语音工具仅支持定时器、音量、设备状态查询，无法控制 AirPlay 播放。

**问题**：

- 无法通过语音说"暂停音乐""下一首"
- 智能音响最核心的音乐控制功能缺失

***

## 二、主流智能音响语音流程对比

### 2.1 典型智能音响语音管线

```
┌─────────────────────────────────────────────────────────────┐
│                    主流智能音响语音管线                        │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  ┌──────┐   ┌─────┐   ┌──────┐   ┌───────┐   ┌──────┐    │
│  │ 麦克 │→→→│ AEC │→→→│ BSS  │→→→│  NS   │→→→│ AGC  │    │
│  │ 阵列 │   │回声消│   │波束成│   │降噪   │   │增益  │    │
│  └──────┘   └─────┘   └──────┘   └───────┘   └──────┘    │
│                                       │                     │
│                    ┌──────────────────┤                     │
│                    ↓                  ↓                     │
│              ┌──────────┐      ┌──────────┐                │
│              │ WakeNet  │      │  VADNet  │                │
│              │ 唤醒词   │      │ 神经VAD  │                │
│              └──────────┘      └──────────┘                │
│                    │                  │                     │
│                    ↓                  ↓                     │
│              ┌──────────┐      ┌──────────┐                │
│              │ MultiNet │      │ 端点检测 │                │
│              │ 本地命令 │      │ Endpointer│                │
│              └──────────┘      └──────────┘                │
│                    │                  │                     │
│           本地执行 ←┘                  └→→→ 云端 ASR+NLU    │
│                                             │              │
│                                             ↓              │
│                                       ┌──────────┐        │
│                                       │   TTS    │        │
│                                       │ 语音合成 │        │
│                                       └──────────┘        │
│                                             │              │
│                    ┌────────────────────────┘              │
│                    ↓                                       │
│              ┌──────────┐                                  │
│              │  AEC Ref │ ← 扬声器输出同时作为回声参考      │
│              └──────────┘                                  │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

### 2.2 对比表

| 维度       | 当前项目          | Amazon Echo    | Google Nest     | 小爱同学        | 目标方案           |
| -------- | ------------- | -------------- | --------------- | ----------- | -------------- |
| **AEC**  | 可选(默认关)       | 始终开启           | 始终开启            | 始终开启        | **始终开启**       |
| **VAD**  | RMS 阈值        | 神经网络           | 神经网络            | 神经网络        | **VADNet**     |
| **降噪**   | WebRTC NS(可选) | 深度降噪           | 深度降噪            | 深度降噪        | **nsnet1**     |
| **波束成形** | 无             | 多麦 BSS         | 多麦 BSS          | 多麦 BSS      | 暂不适用(单麦)       |
| **唤醒词**  | Hi ESP(固定)    | Alexa(可自定义)    | Hey Google      | 小爱同学        | **多模型可选**      |
| **本地命令** | 无             | LVC            | Local Actions   | 本地命令词       | **MultiNet7**  |
| **持续对话** | 无             | Follow-up Mode | Continued Conv. | 连续对话        | **持续监听窗口**     |
| **打断**   | RMS×12(不可靠)   | AEC+VAD(可靠)    | AEC+VAD(可靠)     | AEC+VAD(可靠) | **AEC+VADNet** |
| **TTS**  | 纯云端           | 云端+本地缓存        | 云端+本地缓存         | 云端+本地       | **云端+本地提示音**   |
| **音乐控制** | 仅音量           | 完整控制           | 完整控制            | 完整控制        | **播放控制**       |
| **上下文**  | 256字符         | 多轮对话           | 多轮对话            | 多轮对话        | **扩展上下文**      |

***

## 三、优化方案

### 阶段 1：核心音频前端升级（优先级：高）

#### 1.1 AFE 默认启用 + VADNet 替换 RMS VAD

**目标**：将 AFE 从可选升级为默认开启的核心组件，用 VADNet 替换 RMS 阈值 VAD。

**具体改动**：

1. **修改 Kconfig**：`CONFIG_VOICE_AFE_ENABLE` 默认值改为 `y`
2. **修改 afe\_bridge.c**：
   - 升级到 ESP-SR V2.0 AFE API（当前使用的 V1.x 接口）
   - 将 `afe_ns_mode` 从 `AFE_NS_MODE_WEBRTC` 改为 `AFE_NS_MODE_NSNET`（深度降噪）
   - 将 `vad_mode` 配置为使用 VADNet 模型
   - 添加 `vad_cache` 配置解决首帧延迟截断问题
3. **修改 realtime\_voice.c**：
   - 移除 RMS VAD 逻辑（`rms_of_i16` + 阈值比较）
   - 改用 AFE fetch 结果中的 `vad_state` 作为 VAD 判断
   - 移除 `CONFIG_VOICE_VAD_RMS_THRESHOLD` 配置项
   - 移除 300ms 回声冷却期 hack（AEC 会处理）
   - 移除 `VOICE_PLAYBACK_INTERRUPT_MULTIPLIER` 和 `VOICE_PLAYBACK_INTERRUPT_MIN_RMS`
4. **修改 send\_session\_update()**：
   - 启用 `smooth_output: true`（qwen3.5-omni-flash-realtime 支持，让 TTS 回复更口语化自然）
   - 添加 `turn_detection_threshold` 和 `turn_detection_silence_duration_ms` 可配置项
5. **模型切换支持**：
   - 在 `config.toml` 中 `voice.model` 支持填写 `qwen3.5-omni-plus-realtime`
   - 当检测到 plus 模型时，`session.update` 中 `turn_detection.type` 可选 `semantic_vad`
   - Flash 模型保持 `turn_detection: null`（客户端 VAD + AFE VADNet）

**预期效果**：

- VAD 准确率从 \~60% 提升到 \~95%（VADNet 基于 15000 小时训练）
- 打断可靠性从 \~30% 提升到 \~90%（AEC 消除回声后 VADNet 可准确检测用户语音）
- 噪声环境误触发率降低 80%+

#### 1.2 AFE 参考信号路径优化

**目标**：确保 TTS 播放音频正确作为 AEC 参考信号送入 AFE。

**当前问题**：

- 参考信号通过 `ref_ring` 环形缓冲间接传递，存在延迟
- AFE 需要的参考信号与 TTS 播放之间可能不同步

**具体改动**：

1. 在 TTS 环形缓冲写入时同步写入参考信号环形缓冲
2. 确保 AFE feed 和 fetch 的时序对齐
3. 添加参考信号延迟校准机制

### 阶段 2：模块化重构（优先级：高）

#### 2.1 拆分 realtime\_voice.c 为独立模块

**目标**：将 2820 行的单文件拆分为职责清晰的模块。

**拆分方案**：

```
components/realtime_voice/
├── realtime_voice.c/h          # 顶层状态机 + 任务入口 (~400行)
├── voice_ws.c/h                # WebSocket 连接/发送/接收/重连 (~350行)
├── voice_audio.c/h             # 麦克风采集 + 扬声器播放 + 重采样 (~400行)
├── voice_vad.c/h               # VAD 状态机（VADNet/RMS 兼容）(~200行)
├── voice_tts.c/h               # TTS 环形缓冲 + 播放管线 (~250行)
├── voice_session.c/h           # 会话管理 + 上下文 + 超时 (~200行)
├── voice_protocol.c/h          # DashScope 协议消息处理 (~300行)
├── wakeword_omi.c/h            # WakeNet 唤醒词 (保持不变)
├── afe_bridge.c/h              # AFE 桥接 (保持不变)
├── voice_tools.c/h             # 工具调用 (保持不变)
├── voice_timers.c/h            # 定时器 (保持不变)
├── resampler_bridge.c          # 重采样桥接 (保持不变)
├── CMakeLists.txt
└── idf_component.yml
```

**模块职责**：

| 模块               | 职责                          | 依赖                      |
| ---------------- | --------------------------- | ----------------------- |
| `realtime_voice` | 顶层状态机、任务循环、AirPlay 仲裁       | 所有子模块                   |
| `voice_ws`       | WebSocket 生命周期、JSON 收发、重连策略 | voice\_protocol         |
| `voice_audio`    | 麦克风/扬声器 codec 操作、重采样        | audio\_core             |
| `voice_vad`      | VAD 状态机、语音段检测、端点判断          | afe\_bridge             |
| `voice_tts`      | TTS 环形缓冲、预填充、间隙隐藏           | voice\_audio            |
| `voice_session`  | 会话激活/超时、上下文管理               | -                       |
| `voice_protocol` | DashScope Realtime 协议编解码    | voice\_ws, voice\_tools |

**接口设计原则**：

- 每个模块通过头文件暴露初始化、处理、清理接口
- 模块间通过回调或函数指针通信，不直接访问彼此内部状态
- 保持现有的 `realtime_voice.h` 公共 API 不变

### 阶段 3：本地命令词识别（优先级：中）

#### 3.1 集成 MultiNet7 本地命令词

**目标**：唤醒后优先使用 MultiNet7 识别本地命令，未命中再上传云端。

**支持的本地命令**：

- 音量控制："音量大一点""音量小一点""静音""取消静音
- 播放控制："暂停""继续""下一首""上一首"（AirPlay 场景）
- 定时器："设置 X 分钟定时器""取消定时器"
- 设备控制："关机""重启"

**架构变更**：

```
唤醒 → MultiNet7 命令识别
         │
         ├─ 命中本地命令 → 本地执行 → 本地提示音反馈（<100ms）
         │
         └─ 未命中/需要复杂语义 → WebSocket → DashScope Omni（500ms-2s）
```

**具体改动**：

1. 新增 `voice_cmd.c/h` 模块，封装 MultiNet7 接口
2. 在 AFE fetch 结果中增加命令词检测
3. 命中本地命令时：
   - 直接调用 `voice_tools_dispatch` 执行
   - 播放本地提示音（短促的"叮"声，无需云端 TTS）
   - 跳过 WebSocket 上传
4. 未命中时走原有云端流程

**预期效果**：

- 常用命令响应时间从 1-2s 降低到 <100ms
- 网络断开时仍可执行基本命令
- 减少云端 API 调用量

### 阶段 4：持续对话模式（优先级：中）

#### 4.1 实现持续监听窗口

**目标**：助手回答完毕后，保持一段时间的活跃监听窗口，用户无需重新唤醒即可继续对话。

**具体改动**：

1. 新增 `CONFIG_VOICE_CONTINUED_CONVERSATION` 配置项
2. 新增 `CONFIG_VOICE_CONTINUED_LISTEN_MS` 配置项（默认 8000ms）
3. 修改状态机：`SPEAKING` → `LISTENING`（而非回到待机等待唤醒）
4. 在持续监听窗口内：
   - VADNet 检测到语音直接上传，无需唤醒词
   - 窗口超时后回到唤醒词监听
   - 用户可随时打断助手回答
5. 上下文管理：
   - 扩展 `CONFIG_VOICE_CONTEXT_MAX_CHARS` 从 256 到 1024
   - 在 `session.update` 中携带历史对话摘要

### 阶段 5：唤醒词扩展 + AirPlay 播放控制（优先级：低）

#### 5.1 多唤醒词支持

**目标**：支持用户选择唤醒词模型。

**具体改动**：

1. 在 `config.toml` 中添加 `voice.activation.model` 配置
2. 在 Kconfig 中添加 `CONFIG_VOICE_WAKE_MODEL` 选项
3. 预置模型列表：`wn9_hiesp`, `wn9_nihaoxiaozhi_tts`, `wn9_alexa`, `wn9_jarvis_tts`
4. 运行时通过 NVS 存储用户选择

#### 5.2 AirPlay 播放控制工具

**目标**：通过语音控制 AirPlay 播放。

**新增工具**：

- `playback_control`：支持 `pause`、`resume`、`skip`、`previous` 操作
- 仅在 AirPlay 流媒体活跃时可用

**具体改动**：

1. 在 `voice_tools.c` 中添加 `playback_control` 工具
2. 在 `airplay_service.h` 中暴露播放控制接口
3. 在 `session.update` 的 tools 列表中条件注册

***

## 四、实施优先级与依赖关系

```
阶段 1.1 AFE 默认启用 + VADNet     ← 最高优先级，独立可做
    │
    ├→ 阶段 1.2 AFE 参考信号优化     ← 依赖 1.1
    │
阶段 2.1 模块化重构                 ← 高优先级，独立可做
    │
    ├→ 阶段 3.1 MultiNet7 本地命令   ← 依赖 2.1（需要 voice_cmd 模块）
    │
    ├→ 阶段 4.1 持续对话模式         ← 依赖 1.1（需要 VADNet）+ 2.1
    │
    ├→ 阶段 5.1 多唤醒词支持         ← 依赖 1.1
    │
    └→ 阶段 5.2 AirPlay 播放控制     ← 依赖 3.1（本地命令优先）
```

**建议实施顺序**：

1. **阶段 1.1**：AFE 默认启用 + VADNet（解决最核心的 VAD 和打断问题）
2. **阶段 2.1**：模块化重构（为后续功能奠定基础）
3. **阶段 1.2**：AFE 参考信号优化（提升 AEC 效果）
4. **阶段 3.1**：MultiNet7 本地命令（提升响应速度）
5. **阶段 4.1**：持续对话模式（提升交互体验）
6. **阶段 5.1 + 5.2**：唤醒词扩展 + 播放控制（锦上添花）

***

## 五、风险评估

| 风险                      | 影响              | 缓解措施                                                  |
| ----------------------- | --------------- | ----------------------------------------------------- |
| ESP-SR V2.0 API 不兼容     | AFE/VADNet 无法集成 | 先验证 V2.0 API 兼容性，必要时保持 V1.x 降级路径                      |
| AFE 始终开启增加 CPU/内存占用     | 影响整体性能          | AFE 在 ESP32-S3 上仅占 22% CPU，可接受                        |
| MultiNet7 模型占用 Flash 空间 | 分区表需要调整         | MultiNet7 模型约 500KB，当前 model 分区 8MB 充足                |
| 模块化重构引入回归               | 功能异常            | 逐步拆分，每步验证，保持现有测试通过                                    |
| VADNet 替换 RMS 后阈值调优     | 灵敏度不合适          | VADNet 有 `vad_min_speech_ms`、`vad_min_noise_ms` 等参数可调 |

***

## 六、验证方法

### 阶段 1 验证

- AEC 验证：TTS 播放期间说唤醒词，应能可靠检测
- VADNet 验证：安静环境下误触发率 < 1次/小时；噪声环境下（风扇/空调）误触发率 < 3次/小时
- 打断验证：TTS 播放期间正常语音打断成功率 > 90%

### 阶段 2 验证

- 功能回归：所有现有场景（唤醒→对话→打断→AirPlay 仲裁）行为不变
- 编译验证：`~/.platformio/penv/bin/pio run -e m5cores3` 通过

### 阶段 3 验证

- 本地命令响应时间 < 200ms
- 网络断开时本地命令仍可用
- 云端命令走原有流程不受影响

### 阶段 4 验证

- 助手回答后 8 秒内可直接说话，无需唤醒
- 超时后回到唤醒词监听
- 上下文在多轮对话中保持

***

## 七、构建与验证命令

```bash
# 构建
~/.platformio/penv/bin/pio run -e m5cores3

# 刷写
~/.platformio/penv/bin/pio run -e m5cores3 -t upload

# 串口监控
~/.platformio/penv/bin/pio device monitor

# 快速检查
scripts/check-fast.sh
```

