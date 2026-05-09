# 修复播放器过早停止问题

## 问题分析

### 日志时间线

| 时间 | 事件 |
|------|------|
| 3750ms | PTT 开始录音 |
| 5250ms | 录音结束（24000 frames @ 8kHz） |
| 5250-18300ms | OSS 上传（约 13 秒） |
| 18300-19850ms | DashScope 请求（约 1.5 秒） |
| 19850ms | SSE 开始接收 |
| **21410ms** | **第一个 audio chunk 到达（7680 frames @ 24000Hz）** |
| 21410ms | 播放器启动 |
| **24150ms** | **Silence timeout，播放器停止** |
| 24880ms | 后续 audio chunk 到达，但播放器已停止 |

### 根本原因

**播放器只播放了 2.7 秒就因 silence timeout 停止！**

问题链：
1. 第一个 audio chunk 到达 → 播放器启动
2. 预缓冲 100ms 后开始播放
3. 播放消费速度 = 实时（20ms chunk）
4. 但 SSE 音频数据是 burst 到达，之间有间隙
5. 2.7 秒后 ring buffer 被消费完
6. Silence timeout（1000ms）触发 → 播放器停止
7. 后续 audio chunk 继续到达，但播放器已停止 → 电流声/无声

### 关键参数

- `SILENCE_TIMEOUT_MS = 1000` — 1 秒无数据就停止
- `VOICE_PLAYER_PREFILL_MS = 100` — 只预缓冲 100ms
- `VOICE_PLAYER_RING_MS = 3000` — ring buffer 3 秒

## 修复方案

### 方案 1：增加 Silence Timeout（推荐）

**文件**: `components/realtime_voice/voice_player.c`

**修改**：将 `SILENCE_TIMEOUT_MS` 从 1000ms 增加到 5000ms

**理由**：
- DashScope SSE 音频数据是流式到达的，有 burst 和间隙
- 1 秒 timeout 太短，容易导致播放中断
- 5 秒 timeout 可以容忍更大的数据间隙
- 如果 API 调用失败，最多等待 5 秒，可接受

### 方案 2：增加预缓冲时间

**文件**: `components/realtime_voice/voice_player.c`

**修改**：将 `VOICE_PLAYER_PREFILL_MS` 从 100ms 增加到 500ms

**理由**：
- 更多预缓冲可以平滑 burst 到达的数据
- 减少播放开始后的 underflow 风险
- 但会增加启动延迟（从 100ms 到 500ms）

### 方案 3：增加 Ring Buffer 大小

**文件**: `components/realtime_voice/voice_player.c`

**修改**：将 `VOICE_PLAYER_RING_MS` 从 3000ms 增加到 5000ms

**理由**：
- 更大的缓冲区可以容纳更多数据
- 减少 overflow 和 underflow 的风险
- 但会增加内存占用

## 实施计划

### 步骤 1：增加 Silence Timeout

**文件**: `components/realtime_voice/voice_player.c`

```c
// 修改前
#define SILENCE_TIMEOUT_MS 1000

// 修改后
#define SILENCE_TIMEOUT_MS 5000
```

### 步骤 2：增加预缓冲时间

**文件**: `components/realtime_voice/voice_player.c`

```c
// 修改前
#define VOICE_PLAYER_PREFILL_MS 100U

// 修改后
#define VOICE_PLAYER_PREFILL_MS 500U
```

### 步骤 3：增加 Ring Buffer 大小

**文件**: `components/realtime_voice/voice_player.c`

```c
// 修改前
#define VOICE_PLAYER_RING_MS 3000U

// 修改后
#define VOICE_PLAYER_RING_MS 5000U
```

## 验证标准

1. **播放不中断**：日志中无 "Silence timeout, stopping playback" 或至少在音频全部到达后才出现
2. **音频完整播放**：所有 audio chunk 都被播放，无 ring buffer overflow
3. **无明显延迟**：启动延迟 < 1 秒（预缓冲 500ms + 其他开销）

## 风险评估

1. **Silence timeout 增加**：如果 API 调用失败，播放器会等待更久才停止。但可以通过其他超时机制（如 REQUESTING state timeout）来控制。
2. **内存占用增加**：Ring buffer 从 3 秒增加到 5 秒，多占用约 2 * 48000 * 2 = 192KB 内存（对于 24kHz 16bit 立体声）。

## 决策

- **主要方案**：同时实施三个修改
  - SILENCE_TIMEOUT_MS: 1000 → 5000
  - VOICE_PLAYER_PREFILL_MS: 100 → 500
  - VOICE_PLAYER_RING_MS: 3000 → 5000
- **预期效果**：播放器可以容忍 5 秒的数据间隙，确保音频完整播放
