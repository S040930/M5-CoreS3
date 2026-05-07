# CPU 核心分配优化方案

## 现状分析

### 当前任务-核心-优先级映射表

| 任务名 | 核心 | 优先级 | 栈大小 | 所属模块 | 实时性要求 |
|--------|------|--------|--------|----------|-----------|
| `voice_fe_cap` | CPU 1 | 5 | 10240 | 语音前端-采集 | **极高**（I2S RX 读取，丢帧不可恢复） |
| `voice_fe_fch` | CPU 0 | 4 | 4096 | 语音前端-AFE处理 | 高（VAD/WakeNet 推理） |
| `voice_playback` | CPU 0 | 2 | 3072 | 语音播放 | 中（I2S TX 写入，可容忍短暂延迟） |
| `realtime_voice` | CPU 0 | 4 | 20480 | 语音会话主控 | 中（状态机，非实时） |
| `audio_play` (AirPlay) | CPU 1 | 7 | - | AirPlay 音频输出 | **极高**（I2S TX，AirPlay 时序） |
| `audio_recv` | CPU 1 | 8 | - | AirPlay 实时接收 | **极高**（网络接收+解密） |
| `ctrl_recv` | CPU 1 | 7 | - | AirPlay 控制通道 | 高 |
| `buff_audio` | CPU 1 | 5 | - | AirPlay 缓冲播放 | 高 |
| `rtsp_server` | CPU 0 | 5 | - | RTSP 服务 | 低 |
| `rtsp_client` | CPU 0 | 5 | - | RTSP 客户端 | 低 |
| `net_mon` | CPU 0 | 5 | - | 网络监控 | 低 |
| `ntp_clock` | CPU 0 | 5 | - | NTP 时钟 | 低 |
| `env_monitor` | CPU 0 | 3 | - | 环境监测 | 低 |
| `sedentary` | CPU 0 | 3 | - | 久坐监测 | 低 |
| `auto_bright` | CPU 1 | 2 | 3072 | 自动亮度 | 低 |

### 核心负载分布

```
CPU 0（重载）                        CPU 1（较轻）
┌────────────────────────────┐      ┌────────────────────────────┐
│ voice_fe_fch    (prio 4)   │      │ voice_fe_cap    (prio 5)   │
│ voice_playback  (prio 2)   │      │ audio_play      (prio 7)   │
│ realtime_voice  (prio 4)   │      │ audio_recv      (prio 8)   │
│ rtsp_server     (prio 5)   │      │ ctrl_recv       (prio 7)   │
│ rtsp_client     (prio 5)   │      │ buff_audio      (prio 5)   │
│ net_mon         (prio 5)   │      │ auto_bright     (prio 2)   │
│ ntp_clock       (prio 5)   │      │                            │
│ env_monitor     (prio 3)   │      │                            │
│ sedentary       (prio 3)   │      │                            │
│ IDLE0           (prio 0)   │      │ IDLE1           (prio 0)   │
└────────────────────────────┘      └────────────────────────────┘
```

### 问题诊断

1. **CPU 0 严重过载**：9 个任务挤在 CPU 0，CPU 1 只有 6 个（且 AirPlay 任务互斥不会同时运行）
2. **voice_playback 与 IDLE0 同核**：playback 阻塞时饿死 IDLE0 → WDT 超时
3. **voice_fe_fch 与 voice_playback 同核**：AFE 处理和 I2S 写入在同一个核心竞争，可能导致 VAD 延迟
4. **低优先级 I/O 任务占据 CPU 0**：ntp、env_monitor、sedentary 等低实时性任务全在 CPU 0，与语音任务争抢

### 互斥约束

- **AirPlay 与 voice 互斥**：voice 获取 speaker ownership 时 AirPlay audio 停止，反之亦然
- **I2S 共享外设**：RX（mic）和 TX（speaker）共享同一 I2S 控制器，但 ESP-IDF 驱动内部有锁保护
- **voice_fe_cap 必须在 CPU 1**：已正确分配，因为 I2S RX 读取需要及时

## 优化方案

### 设计原则

1. **实时任务优先上 CPU 1**：CPU 1 的 AirPlay 任务与 voice 互斥，不会同时运行
2. **I2S 收发分离**：RX 在 CPU 1，TX 也应在 CPU 1，减少跨核 I2S 驱动锁竞争
3. **低实时性 I/O 任务迁至 CPU 0**：网络监控、NTP 等不需要精确时序
4. **IDLE 任务必须能运行**：每个核心的高优先级任务必须有阻塞让出点

### 优化后的核心分配

| 任务名 | 原核心 | 新核心 | 原优先级 | 新优先级 | 变更原因 |
|--------|--------|--------|---------|---------|---------|
| `voice_fe_cap` | CPU 1 | CPU 1 | 5 | 5 | 不变，已是最优 |
| `voice_fe_fch` | CPU 0 | **CPU 1** | 4 | 4 | AFE处理靠近I2S RX数据源，减少跨核延迟 |
| `voice_playback` | CPU 0 | **CPU 1** | 2 | **5** | I2S TX与I2S RX同核减少锁竞争；提升优先级确保连续写入 |
| `realtime_voice` | CPU 0 | CPU 0 | 4 | 4 | 状态机任务，不涉及I2S直接操作，留在CPU 0 |
| `rtsp_server` | CPU 0 | **CPU 1** | 5 | 5 | 网络I/O，与AirPlay共享CPU 1更合理 |
| `rtsp_client` | CPU 0 | **CPU 1** | 5 | 5 | 同上 |
| `net_mon` | CPU 0 | CPU 0 | 5 | 5 | 不变，低频轮询 |
| `ntp_clock` | CPU 0 | CPU 0 | 5 | 5 | 不变，低频网络操作 |
| `env_monitor` | CPU 0 | CPU 0 | 3 | 3 | 不变，低频I2C操作 |
| `sedentary` | CPU 0 | CPU 0 | 3 | 3 | 不变，低频摄像头操作 |
| `auto_bright` | CPU 1 | CPU 1 | 2 | 2 | 不变 |
| `audio_play` | CPU 1 | CPU 1 | 7 | 7 | 不变，AirPlay专用 |
| `audio_recv` | CPU 1 | CPU 1 | 8 | 8 | 不变 |
| `ctrl_recv` | CPU 1 | CPU 1 | 7 | 7 | 不变 |
| `buff_audio` | CPU 1 | CPU 1 | 5 | 5 | 不变 |

### 优化后的核心负载

```
CPU 0（轻载-控制面）                  CPU 1（重载-数据面）
┌────────────────────────────┐      ┌────────────────────────────┐
│ realtime_voice  (prio 4)   │      │ voice_fe_cap    (prio 5)   │
│ net_mon         (prio 5)   │      │ voice_fe_fch    (prio 4)   │
│ ntp_clock       (prio 5)   │      │ voice_playback  (prio 5)   │
│ env_monitor     (prio 3)   │      │ rtsp_server     (prio 5)   │
│ sedentary       (prio 3)   │      │ rtsp_client     (prio 5)   │
│ IDLE0           (prio 0)   │      │ audio_play      (prio 7)   │ ← 互斥
│                            │      │ audio_recv      (prio 8)   │ ← 互斥
│                            │      │ ctrl_recv       (prio 7)   │ ← 互斥
│                            │      │ buff_audio      (prio 5)   │ ← 互斥
│                            │      │ auto_bright     (prio 2)   │
│                            │      │ IDLE1           (prio 0)   │
└────────────────────────────┘      └────────────────────────────┘
```

### 关键变更详解

#### 1. voice_playback: CPU 0 → CPU 1, prio 2 → 5

**原因**：
- I2S TX 和 I2S RX 共享同一 I2S 控制器，放在同一核心减少跨核锁竞争
- voice_fe_cap (prio 5) 在 CPU 1，voice_playback 提升到 prio 5 同级，两者交替运行不会互相饿死
- AirPlay audio_play 与 voice_playback 互斥（speaker ownership），不会同时运行
- **彻底解决 WDT 问题**：CPU 0 上不再有 voice_playback，IDLE0 不再被饿死

**优先级提升到 5 的理由**：
- prio 2 太低，任何 prio 3+ 的任务都能抢占它，导致 I2S TX underrun
- 与 voice_fe_cap 同级 (prio 5)，两者都是 I2S 驱动的实时消费者，应平等调度
- FreeRTOS 同优先级任务时间片轮转，cap 和 playback 会交替执行

#### 2. voice_fe_fch: CPU 0 → CPU 1

**原因**：
- AFE 处理（VAD/WakeNet）需要尽快拿到 cap 采集的数据
- fch 和 cap 在同一核心，数据通过 queue 传递无需跨核同步
- CPU 0 释放一个常驻任务，降低负载

#### 3. rtsp_server + rtsp_client: CPU 0 → CPU 1

**原因**：
- RTSP 是 AirPlay 的子协议，与 AirPlay 音频任务同属一个功能域
- RTSP 只在 AirPlay 活跃时运行，与 voice 互斥
- 释放 CPU 0 上的两个网络 I/O 任务

### 互斥安全性验证

| 场景 | CPU 1 活跃任务 | 是否冲突 |
|------|---------------|---------|
| AirPlay 播放 | audio_recv(8) + audio_play(7) + rtsp(5) + auto_bright(2) | 无冲突，voice 未运行 |
| Voice 对话 | voice_fe_cap(5) + voice_fe_fch(4) + voice_playback(5) + auto_bright(2) | 无冲突，AirPlay 未运行 |
| 空闲待机 | auto_bright(2) | 两个核心都空闲 |

## 需要修改的文件

### 1. `components/realtime_voice/voice_playback_task.c`

```c
// 修改前:
#define VOICE_PLAYBACK_TASK_CORE 0

// 修改后:
#if CONFIG_FREERTOS_UNICORE
#define VOICE_PLAYBACK_TASK_CORE 0
#else
#define VOICE_PLAYBACK_TASK_CORE 1
#endif
```

同时修改优先级：
```c
// 修改前: xTaskCreateStaticPinnedToCore(..., tskIDLE_PRIORITY + 2, ...)
// 修改后: xTaskCreateStaticPinnedToCore(..., tskIDLE_PRIORITY + 5, ...)
```

### 2. `components/realtime_voice/voice_frontend.c`

```c
// 修改前:
#define VOICE_FE_FETCH_TASK_CORE 0

// 修改后:
#if CONFIG_FREERTOS_UNICORE
#define VOICE_FE_FETCH_TASK_CORE 0
#else
#define VOICE_FE_FETCH_TASK_CORE 1
#endif
```

### 3. `components/airplay_core/rtsp/rtsp_server.c`

```c
// 修改前:
#define RTSP_TASK_CORE 0

// 修改后:
#if CONFIG_FREERTOS_UNICORE
#define RTSP_TASK_CORE 0
#else
#define RTSP_TASK_CORE 1
#endif
```

## 不修改的文件

- `realtime_voice.c`：VOICE_TASK_CORE 保持 CPU 0，状态机不需要在数据面核心
- `audio_output_common.h`：AO_PLAYBACK_CORE 保持 CPU 1，已是最优
- `audio_stream_realtime.c`：AUDIO_TASK_CORE 保持 CPU 1，已是最优
- `audio_stream_buffered.c`：AUDIO_BUFFERED_TASK_CORE 保持 CPU 1，已是最优
- `net_mon`、`ntp_clock`、`env_monitor`、`sedentary`：保持 CPU 0，低频 I/O 不需要迁移

## 验证步骤

1. **编译验证**：`pio run` 无错误
2. **WDT 验证**：语音对话播放期间不再出现 `task_wdt: Task watchdog got triggered`
3. **播放流畅性**：AI 语音播放无卡顿、无电流声
4. **AirPlay 兼容**：AirPlay 播放正常，切换 voice/AirPlay 无冲突
5. **VAD 响应**：唤醒词识别延迟不退化
6. **stack watermark**：各任务栈水位在安全范围内

## 风险与缓解

| 风险 | 缓解措施 |
|------|---------|
| CPU 1 上 voice + AirPlay 任务栈总和超 internal RAM | voice 任务栈已在 SPIRAM，AirPlay 也用 SPIRAM；且两者互斥不会同时存在 |
| voice_playback prio 5 与 voice_fe_cap 同级可能影响 cap 调度 | cap 是 I2S RX 驱动回调触发，阻塞时间极短；playback 的 esp_codec_dev_write 阻塞时 cap 仍可运行 |
| RTSP 迁移到 CPU 1 后 AirPlay 延迟变化 | RTSP 本来就在 CPU 1 的 AirPlay 音频链路中，迁移是回归正确位置 |
