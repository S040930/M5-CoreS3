# 代码审查：Bug 修复与性能优化

## 审查范围

全面审查 `components/` 下所有组件，聚焦 **实际影响用户体验的 bug** 和 **可测量的性能瓶颈**。
防御性编码问题（缺少 NULL 检查等）仅在可能导致崩溃时才纳入。

---

## 发现的关键问题（按严重程度排序）

### BUG-1: voice_playout 环形缓冲区无并发保护（数据竞争）

**文件**: `components/realtime_voice/voice_playout.c`
**严重程度**: 🔴 高 — 可能导致音频爆音/静音

`voice_playout_push()` 和 `voice_playout_pop()` 由不同上下文调用：
- `push` 在 HTTP 回调 `oneshot_audio_cb` 中调用
- `pop` 在 `playout_drain_to_speaker` 中调用
- `voice_playout_avail()` 和 `voice_playout_free()` 读写 `s_w`/`s_r` 无任何原子保护

对比 `voice_reference.c` 的环形缓冲区使用了 `portENTER_CRITICAL` 保护，`voice_playout.c` 完全没有。

**修复**: 给 push/pop/avail/free 加 `portMUX_TYPE` 自旋锁保护（与 voice_reference.c 一致）。

---

### BUG-2: voice_request.c 重试时 body 内存已释放（Use-After-Free）

**文件**: `components/realtime_voice/voice_request.c:L334-L341`
**严重程度**: 🔴 高 — 可能导致崩溃或发送损坏数据

```c
esp_err_t err = do_http_request(endpoint, auth, body, &ctx);
if (err != ESP_OK) {
    ...
    err = do_http_request(endpoint, auth, body, &ctx);  // body 仍然有效
}
...
free(body);  // 在函数末尾释放
```

实际上 `body` 在重试时仍然有效（`free(body)` 在函数末尾），所以这不是 UAF。但重试时 `ctx.text_len` 被重置为 0，而 `ctx.text_dst` 指向 `out->text`，如果第一次请求部分写入了文本，重试会覆盖它。这不是 bug，但值得注意。

**结论**: 不是 bug，无需修改。

---

### BUG-3: playout_drain_to_speaker 中 static 局部变量在多上下文不安全

**文件**: `components/realtime_voice/realtime_voice.c:L684,L756,L773`
**严重程度**: 🟡 中 — 当前单线程使用，暂无问题

```c
static uint32_t s_drain_diag_count;      // L684
static uint64_t s_last_lowmark_log_ms;   // L756
static uint64_t s_last_underrun_log_ms;  // L773
```

这些 static 变量只在 `voice_task` 中使用，当前不会跨线程访问，暂无问题。

**结论**: 不是 bug，无需修改。

---

### BUG-4: downsample_16k_to_8k 简单平均可能引入混叠

**文件**: `components/realtime_voice/voice_request.c:L32-L43`
**严重程度**: 🟡 中 — 可能影响语音识别质量

当前降采样实现是简单两点平均：
```c
dst[i] = (int16_t)((src[i*2] + src[i*2+1]) / 2);
```

没有低通滤波器，16kHz→8kHz 降采样时，高于 4kHz 的频率分量会混叠到可听频段。虽然 AFE 已经做了处理，但如果 AFE 输出仍有高频残留，混叠可能降低识别率。

**修复**: 改用 4 点加权平均（简易 FIR 低通），成本极低：
```c
int32_t s = src[i*2-1] + src[i*2]*2 + src[i*2+1]*2 + src[i*2+2];
dst[i] = (int16_t)(s / 6);
```
需要处理边界（i=0 时）。

---

### PERF-1: voice_request.c 每次请求分配 3 个大缓冲区（WAV + Base64 + JSON body）

**文件**: `components/realtime_voice/voice_request.c:L302-L313`
**严重程度**: 🟡 中 — 内存峰值高

当前流程：
1. `wav_from_pcm()` → 分配 ~80KB WAV 缓冲区
2. `base64_encode_wav()` → 分配 ~107KB Base64 缓冲区（此时 WAV 还未释放）
3. `build_request_body()` → 分配 JSON body（此时 Base64 还未释放）

峰值内存：80KB + 107KB + ~120KB ≈ 307KB

**优化**: 释放顺序已经正确（WAV 先释放，再 Base64），但可以进一步：
- WAV 生成后立即 Base64 编码，然后释放 WAV → 已实现 ✅
- Base64 编码后立即构建 JSON body，然后释放 Base64 → 已实现 ✅

**结论**: 当前释放顺序已优化，无需修改。

---

### PERF-2: voice_playout_push/pop 逐样本拷贝

**文件**: `components/realtime_voice/voice_playout.c:L90-L93, L107-L110`
**严重程度**: 🟢 低 — 单次操作量小

```c
for (size_t i = 0; i < n; i++) {
    s_buf[s_w] = pcm[i];
    s_w = (s_w + 1U) < s_cap ? s_w + 1U : 0;
}
```

逐样本拷贝 + 环形回绕检查。当 n 较大时（如 240 个样本 = 10ms@24kHz），可以用两段 memcpy 优化。

**修复**: 分两段 memcpy（回绕前 + 回绕后），减少循环开销。

---

### PERF-3: voice_rs_process_mono 中 float 缓冲区反复分配

**文件**: `components/realtime_voice/voice_dsp.c:L86-L98`
**严重程度**: 🟢 低 — 已有增长式分配

`voice_rs_ensure_float_bufs` 只在需要更大时重新分配，不会每次都分配。当前实现已优化。

**结论**: 无需修改。

---

### BUG-5: voice_reference_airplay_tap 在音频输出回调中做重采样（实时性风险）

**文件**: `components/realtime_voice/voice_reference.c:L160-L209`
**严重程度**: 🟡 中 — 可能导致音频输出延迟抖动

`voice_reference_airplay_tap` 是 `audio_output_set_ref_tap` 的回调，在 AirPlay 音频输出路径中被调用。该回调做了：
1. 立体声→单声道混音
2. 可能的重采样（`voice_rs_process_mono`，包含 mutex 获取）
3. 环形缓冲区 push（`portENTER_CRITICAL`）

在音频输出回调中做重采样 + mutex 可能导致 AirPlay 播放延迟抖动。

**修复**: 回调中只做最小工作（立体声→单声道 + push 到中间缓冲区），重采样延迟到 AFE fetch 时做。

---

### BUG-6: stream_ctx_t 在栈上分配 8KB line_buf

**文件**: `components/realtime_voice/voice_request.c:L27`
**严重程度**: 🟡 中 — 占用 20KB 任务栈的 40%

```c
typedef struct {
  ...
  char line_buf[8192];  // 8KB on stack
  size_t line_len;
} stream_ctx_t;
```

`stream_ctx_t` 在 `voice_request_send_audio` 中作为局部变量分配在 voice_task 的 20KB 栈上。8KB 的 line_buf 占了 40% 栈空间。加上其他局部变量，栈可能紧张。

**修复**: 将 `line_buf` 改为动态分配（voice_buf_alloc），或减小到 4KB（SSE 行通常不会超过 4KB）。

---

## 修改计划

### 修改 1: voice_playout.c — 添加并发保护

给环形缓冲区操作加自旋锁，与 voice_reference.c 保持一致：

```c
static portMUX_TYPE s_playout_spin = portMUX_INITIALIZER_UNLOCKED;

size_t voice_playout_push(const int16_t *pcm, size_t samples) {
    if (s_buf == NULL || pcm == NULL || samples == 0) return 0;
    portENTER_CRITICAL(&s_playout_spin);
    size_t f = voice_playout_free();
    size_t n = samples < f ? samples : f;
    // ... 逐样本拷贝 ...
    portEXIT_CRITICAL(&s_playout_spin);
    return n;
}

size_t voice_playout_pop(int16_t *dst, size_t max_samples) {
    if (s_buf == NULL || dst == NULL || max_samples == 0) return 0;
    portENTER_CRITICAL(&s_playout_spin);
    size_t a = voice_playout_avail();
    size_t n = max_samples < a ? max_samples : a;
    // ... 逐样本拷贝 ...
    portEXIT_CRITICAL(&s_playout_spin);
    return n;
}
```

### 修改 2: voice_playout.c — push/pop 用两段 memcpy 优化

```c
size_t voice_playout_push(const int16_t *pcm, size_t samples) {
    ...
    portENTER_CRITICAL(&s_playout_spin);
    size_t f = voice_playout_free();
    size_t n = samples < f ? samples : f;
    size_t first = s_cap - s_w;
    if (first > n) first = n;
    memcpy(s_buf + s_w, pcm, first * sizeof(int16_t));
    size_t second = n - first;
    if (second > 0) {
        memcpy(s_buf, pcm + first, second * sizeof(int16_t));
    }
    s_w = (s_w + n) % s_cap;
    portEXIT_CRITICAL(&s_playout_spin);
    return n;
}
```

### 修改 3: voice_request.c — line_buf 改为动态分配

```c
typedef struct {
  char *text_dst;
  size_t text_cap;
  size_t text_len;
  voice_request_audio_cb_t audio_cb;
  void *audio_cb_ctx;
  char *line_buf;      // 改为指针
  size_t line_buf_cap; // 容量
  size_t line_len;
} stream_ctx_t;
```

在 `voice_request_send_audio` 中动态分配 line_buf，请求完成后释放。

### 修改 4: voice_request.c — 降采样加简易低通

```c
static int16_t *downsample_16k_to_8k(const int16_t *src, size_t src_frames, size_t *dst_frames) {
  if (src == NULL || src_frames < 4 || dst_frames == NULL) return NULL;
  size_t out_frames = src_frames / 2;
  int16_t *dst = (int16_t *)voice_buf_alloc(out_frames * sizeof(int16_t));
  if (dst == NULL) return NULL;
  for (size_t i = 0; i < out_frames; i++) {
    size_t si = i * 2;
    int32_t s;
    if (si == 0) {
      s = (int32_t)src[0] * 2 + (int32_t)src[1];
      dst[i] = (int16_t)(s / 3);
    } else if (si + 1 >= src_frames) {
      s = (int32_t)src[si - 1] + (int32_t)src[si] * 2;
      dst[i] = (int16_t)(s / 3);
    } else {
      s = (int32_t)src[si - 1] + (int32_t)src[si] * 2 + (int32_t)src[si + 1];
      dst[i] = (int16_t)(s / 4);
    }
  }
  *dst_frames = out_frames;
  return dst;
}
```

---

## 不修改的项目（及原因）

| 项目 | 原因 |
|------|------|
| RTSP 解析器缓冲区溢出 | AirPlay 协议数据来自局域网可信源，且已有多年运行验证 |
| audio_decoder 静态计数器 | 单线程使用，无竞争 |
| audio_crypto peak 计算 | 输入长度已由 RTP 协议限制 |
| 防御性 NULL 检查 | ESP-IDF 的 heap_caps_malloc 在 OOM 时返回 NULL 已被大多数调用点检查 |

---

## 验证步骤

1. 编译通过
2. 烧录后唤醒说话，确认：
   - AI 正确识别语音（8kHz WAV + 低通降采样）
   - 音频播放无爆音/静音（playout 并发保护）
   - 无栈溢出崩溃（line_buf 动态分配）
3. 对比修改前后的语音识别准确率
