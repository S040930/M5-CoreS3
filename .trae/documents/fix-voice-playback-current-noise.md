# 修复语音播放电流声 — 彻底重构方案

## 问题总结

设备播放 DashScope API 返回的语音时只发出电流声，根本原因是 **voice_player.c 的播放路径存在多个严重 bug**。

## 日志分析

从 `/Users/mac/Desktop/airplay/log.md` 分析：
- API 成功返回 49 个 audio chunks（b64_len=20480），数据正常
- 第一个 audio chunk 在 t=15540ms 到达，player 成功启动
- `omni_client: first audio chunk: 7680 frames, 24000 Hz` — API 返回 24kHz mono PCM
- `voice_player_v2: Started` / `Play task started` — 播放任务成功创建
- **没有任何播放帧级别的日志**（说明播放循环在运行但输出音频错误）

## 根因分析

### Bug 1: 重采样后缺少 mono→stereo 转换（**主因**）

`voice_player_task` 第 129-144 行：

```c
if (s_current_rate != hw_rate) {
    // 重采样 24kHz → 44.1kHz，输出是 mono
    out_frames = voice_rs_process_mono(rs, ratio, s_hw_buf, read, s_stereo_buf, s_stereo_cap / VOICE_HW_CHANNELS);
    out_samples = s_stereo_buf;  // ← mono 数据在 s_stereo_buf 中
}
// 然后直接写入：
size_t write_bytes = out_frames * VOICE_HW_CHANNELS * sizeof(int16_t);
audio_output_external_write(out_samples, write_bytes, ...);
```

**问题**：`voice_rs_process_mono` 输出 mono PCM 到 `s_stereo_buf`，但 `write_bytes` 按 stereo 计算（`out_frames * 2 * 2`），导致 I2S 把 mono 数据当作 interleaved stereo 播放 — 每 2 个 sample 被拆成左右声道，音频完全错乱，表现为电流声。

**对比 else 分支**（无重采样时）正确地做了 mono→stereo：
```c
for (size_t i = 0; i < read; i++) {
    s_stereo_buf[i * 2] = s_hw_buf[i];      // L
    s_stereo_buf[i * 2 + 1] = s_hw_buf[i];  // R
}
```

### Bug 2: ringbuf 容量基于错误采样率

`voice_player_init` 第 191-193 行：
```c
uint32_t sample_rate = voice_hw_codec_rate_hz();  // 44100
size_t samples = sample_rate * VOICE_PLAYER_RING_MS / 1000;  // 44100*5=220500
```

ringbuf 存储的是 24kHz mono 数据（`voice_player_feed` 直接 push 24kHz PCM），但容量按 44.1kHz 计算。虽然偏大不是错误，但浪费内存。更严重的是 `prefill` 计算：
```c
s_prefill_ms * s_current_rate / 1000  // 500 * 24000 / 1000 = 12000 samples
```
这个计算是正确的（因为 `s_current_rate` 在 feed 时被设置为 24000）。

### Bug 3: 重采样输出缓冲区可能溢出

`s_stereo_buf` 大小 = `chunk_frames * VOICE_HW_CHANNELS` = 882 * 2 = 1764 samples

`voice_rs_process_mono` 的 `out_cap` 参数 = `s_stereo_cap / VOICE_HW_CHANNELS` = 882

但 24kHz → 44.1kHz 重采样比率 = 1.8375，输入 882 frames → 输出约 1621 frames，而 out_cap 只有 882！**重采样输出会被截断**，导致音频丢失。

### Bug 4: 重采样器状态在多次请求间未重置

`voice_rs_play_rs()` 返回的 Resample 对象在第一次创建后不会重置。多次请求之间，resampler 的内部滤波器状态可能残留上一轮的数据，导致新请求开头出现噪声。

## 修复方案

### 修改文件：`components/realtime_voice/voice_player.c`

#### 修复 1: 重采样后添加 mono→stereo 转换

将重采样路径改为两步：
1. 重采样 mono → mono（输出到 `s_hw_buf` 或专用缓冲区）
2. mono → stereo 转换（输出到 `s_stereo_buf`）

需要新增一个 `s_rs_out_buf` 缓冲区用于存放重采样后的 mono 数据。

#### 修复 2: 正确计算重采样输出缓冲区大小

`s_stereo_buf` 需要能容纳重采样后的最大输出帧数 × 2（stereo）。
`s_rs_out_buf` 需要能容纳重采样后的最大输出帧数。

最大输出帧数 = `chunk_frames * (hw_rate / 24000) + 滤波器余量`
= 882 * 1.8375 + 128 ≈ 1749 frames

#### 修复 3: 每次播放开始时重置重采样器

在 `voice_player_start` 或 `voice_player_task` 开头调用 `voice_play_rs_reset()`。

#### 修复 4: ringbuf 容量基于正确的采样率

使用 `CONFIG_VOICE_OUTPUT_SAMPLE_RATE` (24000) 而非 `voice_hw_codec_rate_hz()` (44100) 来计算 ringbuf 容量。

## 具体代码修改

### voice_player.c

1. **新增重采样输出缓冲区**：
```c
static int16_t* s_rs_out_buf = NULL;    // 重采样 mono 输出
static size_t s_rs_out_cap = 0;
```

2. **修改 `voice_player_task` 缓冲区分配**：
```c
// 重采样输出缓冲区：最大输出帧数
size_t max_rs_frames = voice_rs_output_cap(chunk_frames, (double)hw_rate / 24000.0);
if (s_rs_out_cap < max_rs_frames) {
    if (s_rs_out_buf) playout_free(s_rs_out_buf);
    s_rs_out_buf = playout_alloc(max_rs_frames * sizeof(int16_t));
    s_rs_out_cap = max_rs_frames;
}
// stereo 缓冲区：需要容纳最大重采样输出 × 2
size_t stereo_need = max_rs_frames * VOICE_HW_CHANNELS;
if (s_stereo_cap < stereo_need) {
    if (s_stereo_buf) playout_free(s_stereo_buf);
    s_stereo_buf = playout_alloc(stereo_need * sizeof(int16_t));
    s_stereo_cap = stereo_need;
}
```

3. **修复重采样+mono→stereo 路径**：
```c
if (s_current_rate != hw_rate) {
    if (voice_rs_play_rs() == NULL) {
        voice_pcm_format_t fmt = {
            .sample_rate_hz = s_current_rate,
            .channels = 1,
            .bits_per_sample = 16
        };
        voice_playout_set_stream_format(&fmt);
        voice_rs_play_ensure();
    }
    // Step 1: 重采样 mono→mono
    out_frames = voice_rs_process_mono(voice_rs_play_rs(), voice_rs_play_ratio(),
                                       s_hw_buf, read, s_rs_out_buf, s_rs_out_cap);
    // Step 2: mono→stereo
    for (size_t i = 0; i < out_frames; i++) {
        s_stereo_buf[i * 2] = s_rs_out_buf[i];
        s_stereo_buf[i * 2 + 1] = s_rs_out_buf[i];
    }
    out_samples = s_stereo_buf;
} else {
    // 直接 mono→stereo（无重采样）
    for (size_t i = 0; i < read; i++) {
        s_stereo_buf[i * 2] = s_hw_buf[i];
        s_stereo_buf[i * 2 + 1] = s_hw_buf[i];
    }
    out_samples = s_stereo_buf;
    out_frames = read;
}
```

4. **在播放任务开始时重置重采样器**：
```c
// 在 prefill wait 之前
voice_play_rs_reset();
```

5. **修改 ringbuf 容量计算**：
```c
// voice_player_init 中
uint32_t sample_rate = CONFIG_VOICE_OUTPUT_SAMPLE_RATE;  // 24000，不是 44100
size_t samples = sample_rate * VOICE_PLAYER_RING_MS / 1000;
```

6. **在 deinit 中释放新增缓冲区**：
```c
if (s_rs_out_buf) {
    playout_free(s_rs_out_buf);
    s_rs_out_buf = NULL;
}
```

## 验证步骤

1. 编译：`pio run -e m5cores3`
2. 烧录：`pio run -e m5cores3 -t upload`
3. 测试：按屏幕触发录音，发送语音请求，验证：
   - 设备应该播放清晰的语音（不是电流声）
   - 日志中不应出现 "Write failed" 或 "resampler overflow"
   - 播放应在音频数据结束后正常停止
