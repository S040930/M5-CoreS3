# 修复播放电流声并优化 OSS 上传速度

## 问题分析

### 1. 设备发出电流声（播放问题）

**根本原因**：播放器启动太早，音频数据还没到达就因 silence timeout 停止了。

**时间线**：
- 6180ms: 录音结束，播放器启动
- 13860ms: 第一个 audio chunk 到达（晚了 7.7 秒）
- 17410ms: 播放器因 silence timeout 停止
- 后续：音频数据持续到达，但播放器已停止，ring buffer 溢出

**日志证据**：
```
I (6180) voice_player_v2: Started
I (6180) voice_player_v2: Play task started
...
I (13860) omni_client: first audio chunk: 7680 frames, 24000 Hz
...
I (17410) voice_player_v2: Silence timeout, stopping playback
...
W (29990) voice_player_v2: Ring buffer full: pushed 0/7680
```

### 2. OSS 上传速度优化

**当前状态**：
- 8kHz 降采样已生效（48KB 上传）
- TLS 16KB 分片已生效
- 但上传仍需 8 秒（主要是 TLS 握手和网络延迟）

**瓶颈**：
- Windows ICS 网络极慢（后续会换手机热点）
- 三次 TLS 握手（getPolicy、OSS上传、DashScope API）

## 修复方案

### Phase 1: 修复播放电流声

#### 1.1 延迟启动播放器（推荐）

**文件**: `components/realtime_voice/voice_controller.c`

**修改**：在录音结束后，先发送请求，等待首帧音频到达后再启动播放器。

**当前代码**（第 325 行）：
```c
esp_err_t player_err = voice_player_start();
// ... 错误处理 ...

s.state = CTRL_STATE_PLAYING;

esp_err_t err = omni_client_send_audio(
    req.pcm_data, req.pcm_frames, req.sample_rate);
```

**修改为**：
```c
// 先发送请求，不启动播放器
s.state = CTRL_STATE_REQUESTING;

esp_err_t err = omni_client_send_audio(
    req.pcm_data, req.pcm_frames, req.sample_rate);

// 播放器在第一个 audio chunk 到达时由回调启动
```

**需要修改**：`omni_client.c` 中的 `on_audio_delta` 回调，在第一个 chunk 到达时启动播放器。

#### 1.2 替代方案：增加 silence timeout

**文件**: `components/realtime_voice/voice_player.c`

**修改**：将 `SILENCE_TIMEOUT_MS` 从 1000ms 增加到 10000ms 或更长。

**缺点**：如果 API 调用失败，播放器会等待更久才停止。

### Phase 2: 优化 OSS 上传

#### 2.1 连接复用（Keep-Alive）

**问题**：每次请求都重新建立 TLS 连接，导致三次握手耗时。

**方案**：复用 HTTP 连接，避免重复握手。

**文件**: `components/realtime_voice/omni_client.c`

**修改**：在 `upload_audio_to_oss` 和 `omni_client_send_audio` 之间复用连接。

**难点**：ESP-IDF 的 `esp_http_client` 不支持跨函数复用连接。

#### 2.2 减少请求步骤

**当前流程**：
1. GET getPolicy（获取 OSS 上传策略）
2. POST 上传音频到 OSS
3. POST 发送请求到 DashScope

**优化**：如果 DashScope 支持直接上传，可以省去步骤 1 和 2。

**但**：DashScope `input_audio.data` 只支持 URL，不支持 base64 或二进制数据。

#### 2.3 网络环境优化

**用户已计划**：从 Windows ICS 切换到手机热点。

**预期改善**：
- TLS 握手从 2-3 秒降到 <500ms
- 上传速度从 8 秒降到 <2 秒
- 整体延迟从 50 秒降到 <15 秒

### Phase 3: 代码层面微优化

#### 3.1 减少预缓冲时间

**文件**: `components/realtime_voice/voice_player.c`

**当前**：`VOICE_PLAYER_PREFILL_MS = 500`

**修改**：降低到 100-200ms，减少启动延迟。

#### 3.2 优化 Ring Buffer 大小

**文件**: `components/realtime_voice/voice_player.c`

**当前**：`VOICE_PLAYER_RING_MS = 3000`（3 秒缓冲区）

**分析**：3 秒缓冲区对于 8kHz 音频足够，但对于 24kHz 输出可能不够。

**修改**：根据实际采样率动态调整。

## 实施计划

### 步骤 1: 修复播放电流声（高优先级）

**文件**: `components/realtime_voice/voice_controller.c`
**文件**: `components/realtime_voice/omni_client.c`

1. 移除 `voice_player_start()` 的过早调用
2. 在 `omni_on_audio_delta` 回调中启动播放器
3. 添加状态检查，确保只启动一次

### 步骤 2: 优化预缓冲（中优先级）

**文件**: `components/realtime_voice/voice_player.c`

1. 将 `VOICE_PLAYER_PREFILL_MS` 从 500ms 降到 100ms
2. 测试播放流畅度

### 步骤 3: 网络环境切换（用户操作）

1. 从 Windows ICS 切换到手机热点
2. 重新测试整体延迟

## 验证标准

1. **播放正常**：设备能清晰播放 DashScope 返回的音频，无电流声
2. **无 ring buffer 溢出**：日志中无 `Ring buffer full` 警告
3. **延迟降低**：整体响应时间 < 15 秒（手机热点环境下）

## 风险评估

1. **延迟启动播放器**：如果音频数据永远不到达，播放器不会启动。需要添加超时机制。
2. **预缓冲降低**：可能导致播放卡顿。需要测试。

## 决策

- **主要方案**：延迟启动播放器（Phase 1.1）
- **辅助优化**：降低预缓冲时间（Phase 3.1）
- **网络优化**：切换到手机热点（用户操作）
