# 修复音频环形缓冲区溢出问题

## 问题分析

从日志可以看到：
1. **音频正常接收**：15个音频块，每个7680帧（约320ms @ 24kHz）
2. **扬声器正常打开**：`spk: codec open success, rate=44100 resampler=1`
3. **缓冲区溢出**：从chunk 12开始出现大量 `audio_ringbuf: overflow dropped 160 samples`

### 根本原因

**播放任务优先级太低，无法及时消耗音频数据**

查看 `voice_playback_task.c`:
```c
s_playback_task = xTaskCreateStatic(
    voice_playback_task_run, "voice_playback",
    VOICE_PLAYBACK_TASK_STACK_BYTES / sizeof(StackType_t), NULL, tskIDLE_PRIORITY + 2,  // 优先级太低
    s_playback_stack, &s_playback_tcb);
```

同时，HTTP 客户端在接收数据时占用 CPU，导致播放任务无法及时运行。

### 数据流分析

1. **数据到达速度**：每 ~500ms 收到 7680 帧（24kHz）= ~320ms 音频
2. **消耗速度**：每次 drain 441 帧（10ms @ 44100Hz），但任务优先级低
3. **Ring buffer 容量**：3秒 @ 44100Hz = 132300 samples
4. **溢出时间点**：chunk 12（约 3.5 秒后）开始溢出

## 修复方案

### 方案1：提高播放任务优先级（推荐）

将播放任务优先级从 `tskIDLE_PRIORITY + 2` 提升到 `tskIDLE_PRIORITY + 5` 或更高，确保它能及时运行。

**修改文件**: `components/realtime_voice/voice_playback_task.c`

```c
// 修改前
s_playback_task = xTaskCreateStatic(
    voice_playback_task_run, "voice_playback",
    VOICE_PLAYBACK_TASK_STACK_BYTES / sizeof(StackType_t), NULL, tskIDLE_PRIORITY + 2,
    s_playback_stack, &s_playback_tcb);

// 修改后
s_playback_task = xTaskCreateStatic(
    voice_playback_task_run, "voice_playback",
    VOICE_PLAYBACK_TASK_STACK_BYTES / sizeof(StackType_t), NULL, tskIDLE_PRIORITY + 5,
    s_playback_stack, &s_playback_tcb);
```

### 方案2：增加 drain 批量处理

在 `voice_playout_drain.c` 中，增加每次 drain 的数据量，减少任务切换开销。

**修改文件**: `components/realtime_voice/voice_playout_drain.c`

```c
// 修改前
const size_t drain_chunk = output_rate * 10 / 1000;  // 10ms

// 修改后
const size_t drain_chunk = output_rate * 30 / 1000;  // 30ms
```

### 方案3：增加 ring buffer 容量

将 `VOICE_PLAYOUT_RING_MS` 从 3000 增加到 5000，提供更大的缓冲空间。

**修改文件**: `components/realtime_voice/voice_playout.h`

```c
// 修改前
#define VOICE_PLAYOUT_RING_MS         3000  /* total ring capacity in ms */

// 修改后
#define VOICE_PLAYOUT_RING_MS         5000  /* total ring capacity in ms */
```

## 推荐实施顺序

1. **首先实施方案1**：提高播放任务优先级，这是最直接的修复
2. **如果仍有问题，叠加方案2**：增加 drain 批量处理
3. **最后考虑方案3**：增加缓冲区容量（会占用更多内存）

## 验证步骤

1. 编译并烧录固件
2. 触发语音交互（说 "Hi ESP"）
3. 观察日志，确认没有 `audio_ringbuf: overflow dropped` 警告
4. 确认能听到完整的语音回复

## 修改文件清单

| 文件 | 修改内容 |
|------|----------|
| `components/realtime_voice/voice_playback_task.c` | 提高任务优先级 |
| `components/realtime_voice/voice_playout_drain.c` | 增加 drain chunk 大小（可选） |
| `components/realtime_voice/voice_playout.h` | 增加 ring buffer 容量（可选） |
