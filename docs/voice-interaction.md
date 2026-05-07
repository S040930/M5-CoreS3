# Voice Interaction Integration

## Purpose
本项目的语音链路已经收口为“最小稳定语音”路径，只保留：
- 本地 WakeNet `Hi ESP`
- 单轮录音
- 一次性 HTTP Chat Completions
- 流式音频播报
- 独立的 `realtime_voice_speak_text()` 系统播报入口

本次设计目标是稳定优先，不再保留追问窗口、多轮上下文续写、模型工具调用递归链。

## Runtime Design
- `realtime_voice` 是单一编排器，只走 4 个阶段：
  - `idle_wait_wake`
  - `recording_turn`
  - `uploading_request`
  - `playing_reply`
- `voice_frontend` 只负责：
  - 本地 AFE / WakeNet / VAD
  - 产出 wake 事件
  - 产出单轮录音所需的音频块和语音活动标记
- `voice_session` 只负责单轮 turn controller：
  - 开始录音
  - 结束录音
  - 发起一次请求
  - 接收并播放一次回复
  - 播放结束后立刻回到未 armed 的 wake-wait
- `voice_request` 只保留两种 one-shot 请求：
  - `voice_request_send_audio()`：用户单轮问答
  - `voice_request_send_text()`：系统播报
- `voice_request` 会在首包解析云端音频的实际采样率 / 声道 / 位深，并把它作为当前轮的流格式元数据，供播放端做单点边界适配。
- `voice_playout` / `voice_playback_task` / `voice_playout_drain` 负责独立播放子系统，但不再承担会话续期或 tool followup 语义。

## Session Lifecycle
1. 空闲态只监听 WakeNet。
2. 检测到 `Hi ESP` 后，进入当前轮录音。
3. 录音因超时或静音结束后，发送一次 HTTP 请求。
4. 服务端 SSE 返回 `delta.content` 与 `delta.audio.data`。
5. 音频流播放完成后，当前轮立即结束，系统回到未 armed 状态。
6. 若用户还想继续问，必须重新说 `Hi ESP`。

这里的 `realtime_voice_is_activation_armed()` 只表示“当前轮是否已打开”，不再代表跨轮追问窗口。

## HTTP Flow
单轮请求固定为 DashScope OpenAI-compatible Chat Completions：
- `model`: `CONFIG_VOICE_MODEL`
- `modalities`: `["text", "audio"]`
- `stream`: `true`
- `audio.voice`: `"Tina"`
- `audio.format`: `"wav"`
- `messages`: `system + user`

SSE 处理也收口为最小集合：
- `delta.content`：更新回复文本
- `delta.audio.data`：base64 解码后送入 playout，首包会解析 WAV 头或回退到当前约定格式
- `finish_reason: "stop"`：正常结束

不再解析或执行：
- `tool_calls`
- tool result followup request
- assistant followup recursion

## Playback Policy
- 回复播放继续走共享 `audio_output` 外部 owner 路径，与 AirPlay 共用扬声器仲裁。
- 启动播放采用更高预缓冲阈值，优先稳定，不追求最短首包时延。
- 中途供给不足时，策略是 `silence + rebuffer`，不再回放 stale frame。
- 播放链只保留一条边界适配：收到的云端 PCM/WAV 先归一化成当前轮流格式，再交给共享 speaker 路径做最终写出。
- `voice_playback` 在双核下运行于 `CPU1`，并使用协作式 drain，避免长期占住 `CPU0` 导致 `task_wdt: IDLE0`。

## AirPlay And Announcements
- AirPlay 与语音仍共享 speaker ownership。
- `realtime_voice_speak_text()` 是独立 one-shot 播报接口，不进入用户问答会话态。
- ENV.3 / timer reminder 只能把它当作“尽力播报接口”：
  - 语音当前不在录音 / 上传 / 播报
  - AirPlay 当前未占用 speaker
- 若条件不满足，应拒绝或延后一次，而不是打开会话态。

## Configuration
仍然保留的核心配置：
- `CONFIG_VOICE_REALTIME_URL`
- `CONFIG_VOICE_API_KEY`
- `CONFIG_VOICE_MODEL`
- `CONFIG_VOICE_ACTIVATION_PHRASE_ENABLE`
- `CONFIG_VOICE_ACTIVATION_PHRASE`
- `CONFIG_VOICE_INPUT_SAMPLE_RATE`
- `CONFIG_VOICE_OUTPUT_SAMPLE_RATE`
- `CONFIG_VOICE_UPLINK_FRAME_MS`
- `CONFIG_VOICE_VAD_MIN_SPEECH_MS`
- `CONFIG_VOICE_MIC_IN_GAIN_DB`

已不再作为当前语义核心的旧能力：
- follow-up window
- context accumulation
- tool calling session protocol

如果旧配置项仍暂时保留在 Kconfig / TOML，它们也不应再驱动最小稳定语音的主链行为。

## Verification
1. 构建：
   - `~/.platformio/penv/bin/pio run -e m5cores3`
2. 快速检查：
   - `bash scripts/check-fast.sh`
3. Lint：
   - `bash scripts/lint.sh`
4. 刷写与串口：
   - `~/.platformio/penv/bin/pio run -e m5cores3 -t upload --upload-port /dev/cu.usbmodemXXXX`
   - `~/.platformio/penv/bin/pio run -e m5cores3 -t monitor --upload-port /dev/cu.usbmodemXXXX`

运行验收重点：
- 冷启动后 frontend 稳定初始化，不出现 `voice_fe_fch` 栈溢出。
- 空闲监听期间无 followup restart 自旋、无 WDT。
- `Hi ESP` 后只发起一轮录音和一轮 HTTP 请求。
- 回复播完后立即回到未 armed 状态。
- 串口不再出现 `followup record_start`。
- 串口不再出现 `finish_reason=tool_calls` 驱动的 followup 链。
- `realtime_voice_speak_text()` 可在空闲时独立播报，但不会保留上下文或继续对话。
