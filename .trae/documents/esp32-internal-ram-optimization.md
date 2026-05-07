# ESP32 内部 RAM 优化方案

## 问题概述

ESP32 设备启动后内部 RAM 仅剩 **2263 字节**，最大连续块仅 **928 字节**，导致：
- `network_monitor` 任务（需 4096 字节栈）无法创建
- 系统稳定性风险高，任何新分配都可能失败
- 无法启用 VADNet 等需要额外 RAM 的功能

SPIRAM 充足（~7MB），但大量分配仍走内部 RAM。

---

## 当前内部 RAM 消耗分析

### 任务栈（通过 `xTaskCreate` 动态创建 — 栈从内部 RAM 分配）

| 任务 | 栈大小 (bytes) | 创建方式 | 文件 |
|------|---------------|----------|------|
| `realtime_voice` | 20480 | `xTaskCreate` | `realtime_voice.c:475` |
| `voice_fe_cap` | 10240 | `xTaskCreate` | `voice_frontend.c:577` |
| `voice_fe_fch` | 6144 | `xTaskCreate` | `voice_frontend.c:592` |
| `net_mon` | 4096 | `xTaskCreate` | `app_core.c:457` |
| `sedentary` | 8192 | `xTaskCreate` | `sedentary_monitor.c:224` |
| `audio_play` | 3584 | `xTaskCreatePinnedToCore` | `audio_output_common.c:356` |

**小计：~52.7 KB 内部 RAM 仅用于任务栈**

### 任务栈（通过 `xTaskCreateStatic` — 栈已优先分配到 SPIRAM）

| 任务 | 栈大小 | 分配策略 | 文件 |
|------|--------|---------|------|
| `rtsp_server` | 4096 | SPIRAM 优先 | `rtsp_server.c:472` |
| `rtsp_client` ×2 | 8192×2 | SPIRAM 优先 | `rtsp_server.c:404` |
| `ntp_clock` | 3072 | SPIRAM 优先 | `ntp_clock.c:364` |
| `audio_recv` | 8192 | SPIRAM 优先 | `audio_stream_realtime.c:56` |
| `ctrl_recv` | 3072 | SPIRAM 优先 | `audio_stream_realtime.c:68` |
| `buff_audio` | (动态) | SPIRAM 优先 | `audio_stream_buffered.c:185` |

### 其他内部 RAM 消耗

| 来源 | 估算大小 | 说明 |
|------|---------|------|
| `rtsp_server` client buffer | 4096 初始 | `malloc(buf_capacity)` — 内部 RAM |
| `rtsp_handler_common` SDP buffer | ~数百 | `calloc` — 内部 RAM |
| `audio_decoder` 结构体 | ~数百 | `calloc` — 内部 RAM |
| `rtsp_conn` 结构体 | ~数百 | `calloc` — 内部 RAM |
| `voice_request` JSON copy | ~数 KB | `malloc` — 内部 RAM |
| `audio_resample` float buffers | ~数 KB | `malloc` — 内部 RAM |
| `resampler` 内部状态 | ~数 KB | `calloc/malloc` — 内部 RAM |
| FreeRTOS 内核 + WiFi + LWIP + mDNS | ~100+ KB | 框架级，不可控 |

---

## 优化方案

### 策略 1：将动态任务栈迁移到 SPIRAM（最大收益）

**原理**：`xTaskCreate` 从内部 RAM 分配栈；`xTaskCreateStatic` 允许指定栈内存位置。将所有大栈任务改为 Static 创建，栈从 SPIRAM 分配。

#### 1.1 `realtime_voice` 任务（20480 bytes → SPIRAM）

**文件**：`components/realtime_voice/realtime_voice.c`

- 新增静态 TCB 和 SPIRAM 栈指针
- `realtime_voice_start()` 中将 `xTaskCreate` 改为 `xTaskCreateStatic`
- 栈分配策略：`heap_caps_malloc(VOICE_TASK_STACK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)`，回退 `MALLOC_CAP_INTERNAL`
- `realtime_voice_stop()` 中释放栈内存

**预计释放**：~20 KB 内部 RAM

#### 1.2 `voice_fe_cap` 任务（10240 bytes → SPIRAM）

**文件**：`components/realtime_voice/voice_frontend.c`

- 新增静态 TCB 和 SPIRAM 栈指针（作为 `s_fe` 结构体成员或静态变量）
- `voice_frontend_start()` 中将 `xTaskCreate` 改为 `xTaskCreateStatic`
- `voice_frontend_stop()` 中释放栈内存

**预计释放**：~10 KB 内部 RAM

#### 1.3 `voice_fe_fch` 任务（6144 bytes → SPIRAM）

**文件**：`components/realtime_voice/voice_frontend.c`（同 1.2）

**预计释放**：~6 KB 内部 RAM

#### 1.4 `audio_play` 任务（3584 bytes → SPIRAM）

**文件**：`components/audio_core/audio/audio_output_common.c`

- 新增静态 TCB 和 SPIRAM 栈指针
- `audio_output_common_start()` 中将 `xTaskCreatePinnedToCore` 改为 `xTaskCreateStaticPinnedToCore`
- `audio_output_common_stop()` 中释放栈内存

**预计释放**：~3.5 KB 内部 RAM

#### 1.5 `net_mon` 任务（4096 bytes → SPIRAM）

**文件**：`components/app_core/app_core.c`

- 新增静态 TCB 和 SPIRAM 栈指针
- 将 `xTaskCreate` 改为 `xTaskCreateStatic`

**预计释放**：~4 KB 内部 RAM

### 策略 2：禁用 sedentary_monitor（立即收益）

**文件**：`components/sedentary_monitor/sedentary_monitor.c`

- 在 `sdkconfig` 或 menuconfig 中设置 `CONFIG_SEDENTARY_ENABLE=n`
- 代码中已有 `#if CONFIG_SEDENTARY_ENABLE` 保护，禁用后不会创建任务和初始化摄像头

**预计释放**：~8 KB 内部 RAM（任务栈）+ 摄像头相关内存

### 策略 3：将裸 `malloc/calloc` 调用迁移到 SPIRAM 优先

以下调用当前从内部 RAM 分配，应改为 `heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)` 并回退到 `MALLOC_CAP_8BIT`：

#### 3.1 RTSP client 初始 buffer

**文件**：`components/airplay_core/rtsp/rtsp_server.c:189`

```c
// 当前
uint8_t *buffer = malloc(buf_capacity);  // 4096 bytes from internal RAM
// 改为
uint8_t *buffer = heap_caps_malloc(buf_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
if (!buffer) buffer = malloc(buf_capacity);
```

#### 3.2 RTSP header_str 临时分配

**文件**：`components/airplay_core/rtsp/rtsp_server.c:117`

```c
// 当前
char *header_str = malloc(header_len + 1);
// 改为 SPIRAM 优先
```

#### 3.3 RTSP message response

**文件**：`components/airplay_core/rtsp/rtsp_message.c:164`

```c
uint8_t *response = malloc(total_len);
// 改为 SPIRAM 优先
```

#### 3.4 RTSP SDP buffer

**文件**：`components/airplay_core/rtsp/rtsp_handler_common.c:314`

```c
char *sdp_buf = calloc(1, len + 1);
// 改为 SPIRAM 优先
```

#### 3.5 audio decoder 结构体

**文件**：`components/audio_core/audio/audio_decoder.c:152`

```c
audio_decoder_t *decoder = calloc(1, sizeof(*decoder));
// 改为 SPIRAM 优先
```

#### 3.6 RTSP conn 结构体

**文件**：`components/airplay_core/rtsp/rtsp_conn.c:15`

```c
rtsp_conn_t *conn = calloc(1, sizeof(rtsp_conn_t));
// 改为 SPIRAM 优先
```

#### 3.7 voice_request JSON copy

**文件**：`components/realtime_voice/voice_request.c:125`

```c
char *json_copy = (char *)malloc(json_len + 1);
// 改为 SPIRAM 优先
```

#### 3.8 audio_resample float buffers

**文件**：`components/audio_core/audio/audio_resample.c:35,40`

```c
float_in = malloc(float_in_cap * sizeof(float));
float_out = malloc(float_out_cap * sizeof(float));
// 改为 SPIRAM 优先
```

#### 3.9 test tone buffer

**文件**：`components/audio_core/audio/audio_output_cores3.c:660`

```c
int16_t *pcm = calloc(chunk_frames * CORES3_CHANNELS, sizeof(int16_t));
// 改为 SPIRAM 优先
```

#### 3.10 sedentary 相关分配

**文件**：`components/sedentary_monitor/sedentary_camera.c:125`, `sedentary_alert.c:75`

如果 sedentary 被禁用则无需修改。

### 策略 4：引入统一 SPIRAM 优先分配辅助函数

**文件**：新建 `components/common/mem_helpers.h`（或放入现有公共头文件）

```c
static inline void *spiram_alloc(size_t size) {
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    return p;
}

static inline void *spiram_calloc(size_t count, size_t size) {
    size_t total = count * size;
    void *p = heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_malloc(total, MALLOC_CAP_8BIT);
    if (p) memset(p, 0, total);
    return p;
}
```

将所有策略 3 中的 `malloc/calloc` 替换为 `spiram_alloc/spiram_calloc`。

### 策略 5：延迟初始化 — 语音子系统

当前语音子系统在启动时即初始化 AFE、WakeNet 等重量级模块，占用大量内部 RAM。可以延迟到首次需要时初始化。

**文件**：`components/app_core/app_core.c`

- 将 `start_voice_services()` 从 WiFi 连接后立即调用，改为按需调用
- 或在 `realtime_voice_start()` 中才初始化 AFE（当前已部分如此）

**预计释放**：取决于 AFE 模型加载时机，可能释放数 KB 临时峰值

---

## 预计收益汇总

| 策略 | 预计释放内部 RAM | 风险 | 优先级 |
|------|-----------------|------|--------|
| 1.1 realtime_voice 栈→SPIRAM | ~20 KB | 低（已有 SPIRAM 栈先例） | P0 |
| 1.2-1.3 voice_frontend 栈→SPIRAM | ~16 KB | 低 | P0 |
| 1.4 audio_play 栈→SPIRAM | ~3.5 KB | 低 | P0 |
| 1.5 net_mon 栈→SPIRAM | ~4 KB | 低 | P0 |
| 2. 禁用 sedentary | ~8 KB | 无（用户确认可禁用） | P0 |
| 3. malloc→SPIRAM 优先 | ~5-10 KB | 低 | P1 |
| 4. 统一分配函数 | 0（工程规范） | 无 | P1 |
| 5. 延迟初始化 | ~数 KB | 中（需测试时序） | P2 |

**总计预计释放：~56-62 KB 内部 RAM**

当前可用 2263 bytes → 优化后预计可用 **~58-64 KB**

---

## 实施顺序

1. **策略 2**：禁用 sedentary — 最简单，立即见效
2. **策略 4**：引入 `spiram_alloc/spiram_calloc` 辅助函数
3. **策略 1.1-1.5**：逐个将任务栈迁移到 SPIRAM
4. **策略 3.1-3.10**：逐个将裸 malloc 改为 SPIRAM 优先
5. **策略 5**：延迟初始化（可选，视前几步效果决定）

---

## 验证方法

1. 编译烧录后，观察启动日志中的 `internal_free` 和 `internal_largest` 值
2. 确认 `network_monitor` 任务创建成功
3. 运行 AirPlay 播放 + 语音唤醒全链路测试
4. 监控运行时栈 watermark，确认 SPIRAM 栈任务无异常
5. 长时间运行稳定性测试（>1小时）

---

## 注意事项

- ESP32 的 SPIRAM 不能用于 DMA 传输和中断处理，任务栈放 SPIRAM 需确保任务中不直接操作 DMA 缓冲区或在中断上下文中执行
- FreeRTOS 任务栈放 SPIRAM 在 ESP-IDF 中是官方支持的用法（`xTaskCreateStatic` + SPIRAM 栈），已在 RTSP/NTP/audio_recv 等任务中验证
- `voice_fe_cap` 和 `voice_fe_fch` 任务中调用 `esp_codec_dev_read`，该函数内部可能使用 I2S DMA，需验证栈在 SPIRAM 时是否正常工作
- `audio_play` 任务已使用 `xTaskCreatePinnedToCore` 绑核，改为 Static 版本时需使用 `xTaskCreateStaticPinnedToCore`
