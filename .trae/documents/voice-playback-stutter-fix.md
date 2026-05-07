# 语音播放卡顿与电流声修复方案

## 问题

语音助手播放时扬声器卡顿、不清晰、有电流声，但 AirPlay 播放完全正常。

## 根因分析

### AirPlay vs Voice 音频路径对比

| 维度 | AirPlay (`audio_output_common`) | Voice (`voice_playout_drain`) |
|------|------|------|
| **I2S 写入方式** | `s_ops->write_pcm()` → `codec_write_checked()` → `esp_codec_dev_write()` | `audio_output_external_write()` → `codec_write_checked()` → `esp_codec_dev_write()` |
| **写入粒度** | `AO_FRAME_SAMPLES` = 352 帧 ≈ 8ms | `drain_chunk` = 240 帧 (24kHz×10ms) ≈ 10ms |
| **无数据时** | 写静音 `silence_buf`，保持 I2S 连续 | **写 gap concealment 重复帧** 或关闭 speaker |
| **循环结构** | `while(playback_running)` 无 break，持续写 | 内层 while 有多个 break 退出条件 |
| **优先级** | 7 | 5 (已改) |
| **核心** | CPU 1 | CPU 1 (已改) |
| **gap concealment** | 最多重复 16 次，然后写静音 | 重复旧帧 500ms，增益从 1.0→0.5，然后关闭 speaker |

### 关键差异导致的问题

#### 1. Voice 内层循环频繁退出 → I2S underrun → 卡顿

`voice_playback_task_run` 内层循环有三个 break 退出条件：
- `avail_before == 0 && !ctx->response_audio_closed` → ring 无数据且未关闭
- `voice_playout_avail() == avail_before` → drain 没消耗数据

退出后回到外层，`ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20))` 最多等 20ms 才重新进入。这 20ms 空窗 I2S DMA 耗尽 → underrun → 卡顿。

**AirPlay 不存在此问题**：它的循环 `while(playback_running)` 无 break，无数据时写静音保持 I2S 连续。

#### 2. Gap concealment 机制不同 → 电流声

Voice 的 gap concealment（[voice_playout_drain.c:136-155](file:///Users/mac/Desktop/airplay/components/realtime_voice/voice_playout_drain.c#L136-L155)）：
- 重复最后一帧立体声数据
- 增益从 1.0 线性衰减到 0.5（50ms 内）
- **之后持续以 0.5 增益重复长达 500ms**（`VOICE_PLAYOUT_GAP_TIMEOUT_MS`）
- 短周期重复 + 衰减不充分 → 听起来像嗡嗡的电流声

AirPlay 的 gap concealment（[audio_output_common.c:104-114](file:///Users/mac/Desktop/airplay/components/audio_core/audio/audio_output_common.c#L104-L114)）：
- 最多重复 16 次（`GAP_CONCEAL_REPEAT_MAX`），每次衰减 0.8x
- 16 次后写纯静音
- 重复次数有限 + 快速衰减 → 几乎不可察觉

#### 3. Voice drain 每次只写 10ms → 调度间隙导致 underrun

`drain_chunk = output_rate * 10 / 1000 = 240 帧`，仅 10ms 音频。如果任务被抢占或调度延迟，10ms 的数据很快耗尽。

AirPlay 每次写 `AO_FRAME_SAMPLES = 352 帧` ≈ 8ms，但循环紧密无间隙，I2S DMA 始终有数据。

## 修复方案

### 修改1：内层循环退出后写静音填充（防 underrun）

**文件**：`components/realtime_voice/voice_playback_task.c`

内层循环 break 退出后，不直接回到外层等 20ms，而是写一小段静音到 I2S 保持 DMA 不空，然后短暂等待（1 tick）后重新检查数据。

```c
// 内层循环退出后，如果播放仍在进行，写静音保持 I2S 连续
if (s_enabled && voice_speaker_playback_active()) {
    int16_t silence_buf[480]; // 10ms @ 24kHz stereo
    memset(silence_buf, 0, sizeof(silence_buf));
    audio_output_external_write(silence_buf, sizeof(silence_buf), pdMS_TO_TICKS(10));
}
```

### 修改2：Gap concealment 改为有限次数 + 静音（消除电流声）

**文件**：`components/realtime_voice/voice_playout_drain.c`

参照 AirPlay 的成熟方案：
- 最多重复 8 次（而非无限重复 500ms）
- 每次衰减 0.7x（而非 0.5x 固定增益）
- 超过次数后写静音
- 超时后关闭 speaker

### 修改3：Prefill 时间过长导致首字延迟

**文件**：`components/realtime_voice/voice_playout.h`

当前 `VOICE_PLAYOUT_PREFILL_MS = 2500`，即等 2.5 秒数据才开始播放。从日志看 `startup_wait_ms=2153`，用户等了 2 秒多才听到声音。

降低到 500ms，配合静音填充机制，可以在数据不足时也保持播放连续性。

## 需要修改的文件

### 1. `components/realtime_voice/voice_playback_task.c`

内层循环退出后添加静音填充逻辑。

### 2. `components/realtime_voice/voice_playout_drain.c`

- 添加 gap concealment 重复计数器（static）
- 限制最大重复次数为 8
- 每次衰减 0.7x
- 超过次数后写静音而非继续重复
- 超时（500ms）后关闭 speaker

### 3. `components/realtime_voice/voice_playout.h`

- `VOICE_PLAYOUT_PREFILL_MS` 从 2500 降到 500

## 验证步骤

1. `pio run` 编译通过
2. 语音播放无卡顿、无电流声
3. 首字响应时间从 ~2s 降到 ~0.5s
4. AirPlay 播放不受影响
5. 看门狗不触发
