# 语音交互流程优化方案（更新版）

## 目标
响应快、识别准、稳定性高

## 当前状态

### 已完成
- ✅ 步骤1：`voice_request.h` 已重写 — 添加 `voice_request_audio_cb_t` 回调，移除全量音频字段
- ✅ 步骤2：`voice_request.c` 已重写 — 增量 SSE 解析 + 流式音频回调 + 网络重试

### 待完成
- ❌ 步骤3：`realtime_voice.c` 修改 — oneshot 对接 playout ring + 缩短静音检测
- ❌ 步骤4：编译验证

### 关键阻塞问题

**当前代码无法编译**：`realtime_voice.c` 行 1714 仍引用 `result.audio_pcm`，但 `voice_request_result_t` 已移除该字段。

## 步骤3 详细方案：修改 realtime_voice.c

### 3.1 修改常量

```c
#define VOICE_ONESHOT_SILENCE_MS      500   // 从 900 降到 500
#define VOICE_ONESHOT_RECORD_MAX_MS   5000  // 从 3000 恢复到 5000
```

- 缩短静音检测：用户说完话后 500ms 即判定结束（原来 900ms）
- 延长最大录音：配合更短静音检测，短句更快结束，长句不会被截断

### 3.2 添加流式音频回调函数

在 `realtime_voice.c` 中添加一个静态回调函数，将 HTTP 流式返回的 PCM 推送到 playout ring：

```c
static void oneshot_audio_cb(void *ctx, const int16_t *pcm, size_t frames,
                              uint32_t sample_rate) {
  (void)ctx;
  (void)sample_rate;
  if (pcm == NULL || frames == 0) return;
  size_t pushed = voice_playout_push(pcm, frames);
  if (pushed > 0) {
    voice_playout_set_last_write_ms(now_ms());
  }
  if (pushed < frames) {
    ESP_LOGW(TAG, "oneshot audio_cb: ring overflow, dropped %lu frames",
             (unsigned long)(frames - pushed));
  }
}
```

### 3.3 重写 oneshot 请求+播放流程

替换当前行 1693-1778 的全量播放代码。核心变化：

**之前**（阻塞式全量播放）：
```
voice_request_send_audio() → 阻塞等待完整响应 → result.audio_pcm 全量播放
```

**之后**（流式边收边播）：
```
voice_playout_reset() → spk_open() → voice_request_send_audio(audio_cb=oneshot_audio_cb)
→ 请求期间 audio_cb 持续推送到 ring → 请求完成后 drain 剩余 → spk_close()
```

#### 关键问题：HTTP 阻塞 vs playout drain

`esp_http_client_perform()` 是阻塞调用，在 HTTP 请求期间，`voice_task` 主循环不会执行 `voice_loop_drain_and_monitor()`，playout ring 不会被 drain。

**解决方案**：在 `oneshot_audio_cb` 回调中，每次推送音频后主动调用 drain。因为 `esp_http_client` 的 `HTTP_EVENT_ON_DATA` 回调在 `esp_http_client_perform` 的上下文中执行（实际上是 `http_event_handler` → `stream_handle_sse_line` → `audio_cb`），所以回调在 voice_task 线程中执行，可以直接调用 `playout_drain_to_speaker()`。

修改后的回调：
```c
static void oneshot_audio_cb(void *ctx, const int16_t *pcm, size_t frames,
                              uint32_t sample_rate) {
  (void)ctx;
  (void)sample_rate;
  if (pcm == NULL || frames == 0) return;
  size_t pushed = voice_playout_push(pcm, frames);
  if (pushed > 0) {
    voice_playout_set_last_write_ms(now_ms());
  }
  // 主动 drain，因为 HTTP 阻塞期间主循环不会执行 drain
  if (s_ctx.playback_active) {
    playout_drain_to_speaker();
  }
}
```

这样每次收到一段音频数据，就立即推送到 ring 并 drain 到扬声器，实现真正的边收边播。

#### 具体代码替换

将行 1693-1778 的代码替换为：

```c
s_ctx.waiting_response = true;
set_state(REALTIME_VOICE_STATE_THINKING, NULL);

voice_playout_reset();

voice_request_config_t req_cfg = {
    .url = s_voice_config.url,
    .api_key = s_voice_config.api_key,
    .model = "qwen3.5-omni-flash",
    .audio_cb = oneshot_audio_cb,
    .audio_cb_ctx = NULL,
};

bool spk_ok = spk_open();
if (spk_ok) {
  s_ctx.speaking = true;
  set_state(REALTIME_VOICE_STATE_SPEAKING, NULL);
}

voice_request_result_t result = {0};
esp_err_t req_err = voice_request_send_audio(&req_cfg, record_pcm, record_frames,
                                              VOICE_ONESHOT_SAMPLE_RATE, &result);
s_ctx.waiting_response = false;

if (audio_output_is_active()) {
  ESP_LOGI(TAG, "oneshot result discarded: airplay output active");
  if (spk_ok) spk_close();
} else if (req_err == ESP_OK) {
  size_t n = strlen(result.text);
  if (n >= sizeof(reply_text)) n = sizeof(reply_text) - 1;
  memcpy(reply_text, result.text, n);
  reply_text[n] = '\0';
  ESP_LOGI(TAG, "AI回复: \"%s\"", reply_text);

  if (spk_ok) {
    // drain 剩余音频
    uint64_t drain_start = now_ms();
    while (voice_playout_avail() > 0 && s_ctx.playback_active &&
           (now_ms() - drain_start) < 10000) {
      playout_drain_to_speaker();
      vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_ctx.playback_active) {
      spk_close();
    }
  }

  keep_reply_text = true;
} else {
  snprintf(s_err_detail, sizeof(s_err_detail), "REQ FAILED %d", result.status_code);
  set_state(REALTIME_VOICE_STATE_ERROR, s_err_detail);
  if (spk_ok) spk_close();
  vTaskDelay(pdMS_TO_TICKS(1000));
  set_state(REALTIME_VOICE_STATE_LISTENING, NULL);
}
```

### 3.4 状态管理注意事项

- `spk_open()` 在请求前调用，确保 ring 有消费者
- 如果 `spk_open()` 失败，仍然发送请求（只是没有播放），至少能获取文本回复
- HTTP 请求期间，`audio_cb` 中的 `playout_drain_to_speaker()` 保证 ring 不会溢出
- 请求完成后，循环 drain 剩余音频直到 ring 为空
- `spk_close()` 内部会 `voice_playout_reset()` 和 `speaker_release()`

### 3.5 移除不再需要的代码

- 行 1714-1770：整个 `result.audio_pcm` 全量播放代码块（已由流式播放替代）
- 行 1768-1769：`voice_buf_free(result.audio_pcm)` （字段已不存在）

## 修改文件清单

| 文件 | 修改类型 | 说明 |
|------|---------|------|
| `realtime_voice.c` | 修改 | 常量调整 + 添加 audio_cb + 重写 oneshot 播放流程 |

## 预期效果

| 指标 | 优化前 | 优化后 |
|------|--------|--------|
| 首字音频延迟 | 7~17s | 3~5s |
| 短句录音等待 | 900ms 静音 | 500ms 静音 |
| 内存峰值 | ~1.2MB | ~600KB |
| 网络失败恢复 | 无重试 | 自动重试1次 |
| 播放方式 | 全量接收后播放 | 边收边播 |

## 验证步骤

1. `pio run` 编译通过
2. 烧录后唤醒词正常触发
3. 说短句（<2s），观察首字音频延迟是否降低
4. 说长句（>3s），观察录音截断和识别准确度
5. 观察日志中 `playout ttfb_ms` 指标
6. 监控内存使用（spiram_free）
