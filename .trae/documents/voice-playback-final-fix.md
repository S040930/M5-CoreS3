# 语音播放卡顿与电流声修复方案

## 日志分析

### 关键时间线

```
T=11251  唤醒词 "Hi ESP" 检测
T=12801  录音结束，开始上传
T=18171  第一个音频块到达 (7680 frames)
T=43711  Prefill 完成，开始播放！startup_wait_ms=25534 (25.5秒！)
T=45191  第一个 pcm peak 输出 (input=10879)
T=46111  pcm peak input=104  ← 极低，gap concealment
T=46911  pcm peak input=18047
T=48511  pcm peak input=36   ← 极低，gap concealment
T=51071  播放结束 playout_done_ms=7361
```

### 问题1：startup_wait_ms=25534（25.5秒！）

日志显示 `startup_wait_ms=25534`，即从录音开始到播放开始等了 25.5 秒。这是因为 `VOICE_PLAYOUT_PREFILL_MS=2500`，但网络延迟导致音频数据到达非常慢：

- 第1个音频块 (7680 frames): T=18171，距录音开始 5.3 秒
- 第2个音频块: T=20931，间隔 2.8 秒
- 第3个音频块: T=24871，间隔 3.9 秒
- 第4个音频块: T=31711，间隔 6.8 秒
- 第5个音频块: T=35901，间隔 4.2 秒
- 第6个音频块: T=40951，间隔 5.0 秒
- 第7个音频块: T=43711，间隔 2.8 秒 → 终于凑够 prefill

**网络延迟导致音频块间隔 3-7 秒**，prefill 2500ms 的数据需要等 25 秒才凑齐。

### 问题2：播放期间 peak 值剧烈波动

```
peak=10879 → peak=104 → peak=18047 → peak=36 → peak=14463 → peak=13311 → peak=4031
```

peak=104 和 peak=36 是 gap concealment 写入的衰减旧帧，听起来就是电流声。

### 问题3：vTaskDelay(1) 导致 I2S underrun

当前内层循环每次 drain 后 `vTaskDelay(1)` = 10ms。drain 只写 10ms 音频到 I2S，然后等 10ms。I2S DMA 在这 10ms 空窗期耗尽 → underrun → 卡顿 + 电流声。

### 问题4：内层循环 break 条件过于激进

```c
if (avail_before == 0 && !ctx->response_audio_closed) break;  // 无数据且未关闭就退出
if (voice_playout_avail() == avail_before) break;              // drain没消耗数据就退出
```

退出后外层 `ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20))` 最多等 20ms。在网络慢速传输场景下，ring buffer 经常为空，频繁退出内层循环 → 播放断断续续。

## 修复方案

### 修改1：voice_playback_task.c — 移除 vTaskDelay(1)，改用 esp_codec_dev_write 的自然阻塞

`esp_codec_dev_write()` 内部通过 I2S DMA 队列阻塞，当 DMA 缓冲区满时自动让出 CPU。不需要额外的 `vTaskDelay(1)`。

**修改前**：
```c
if (voice_playout_avail() == avail_before) {
  break;
}
vTaskDelay(1);
```

**修改后**：
```c
if (voice_playout_avail() == avail_before) {
  break;
}
```

### 修改2：voice_playback_task.c — 内层循环退出后写静音保持 I2S 连续

内层循环 break 退出后，如果播放仍在进行，写一小段静音到 I2S 保持 DMA 不空，避免 underrun。

**修改位置**：内层 while 循环结束后，外层 for 循环内

```c
while (s_enabled && voice_speaker_playback_active()) {
  // ... drain loop ...
}
// 内层循环退出后，如果仍在播放，写静音保持 I2S 连续
if (s_enabled && voice_speaker_playback_active()) {
  int16_t silence[480];
  memset(silence, 0, sizeof(silence));
  audio_output_external_write(silence, sizeof(silence), pdMS_TO_TICKS(10));
}
```

### 修改3：voice_playout.h — 降低 prefill 时间

`VOICE_PLAYOUT_PREFILL_MS` 从 2500 降到 800。配合静音填充机制，即使数据不足也能保持播放连续性，不需要等 2.5 秒数据才开始。

### 修改4：voice_playout_drain.c — gap concealment 后写静音而非关闭

超过 8 次重复后写静音（已实现），但超时 800ms 后直接关闭 speaker 导致音频突然中断。改为：超时后继续写静音直到 `response_audio_closed`。

## 需要修改的文件

### 1. `components/realtime_voice/voice_playback_task.c`

- 移除 `vTaskDelay(1)`
- 内层循环退出后写静音保持 I2S 连续
- 需要添加 `#include <string.h>` 和 `audio/audio_output.h`

### 2. `components/realtime_voice/voice_playout.h`

- `VOICE_PLAYOUT_PREFILL_MS` 从 2500 降到 800

### 3. `components/realtime_voice/voice_playout_drain.c`

- gap concealment 超时后不立即关闭 speaker，改为写静音等待 `response_audio_closed`

## 验证步骤

1. `pio run` 编译通过
2. 语音播放无卡顿、无电流声
3. 首字响应时间大幅缩短（从 25s 降到 <5s）
4. 看门狗不触发
5. AirPlay 播放不受影响
