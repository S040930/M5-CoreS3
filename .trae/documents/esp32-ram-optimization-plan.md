# ESP32 内部 RAM 优化 — 详细实施计划

## 目标

将内部 RAM 可用量从当前 ~2.2 KB 提升到 ~60 KB，使 network_monitor 等任务能正常创建，并为 VADNet 等功能预留空间。

## 当前状态

- 内部 RAM 可用：2263 bytes，最大连续块：928 bytes
- SPIRAM 可用：~7 MB（几乎空闲）
- 根因：语音助手 3 个任务（共 36.9 KB 栈）使用 `xTaskCreate`，栈从内部 RAM 分配
- AirPlay 侧已全部使用 `xTaskCreateStatic` + SPIRAM 栈，语音侧未跟进

---

## 改动 1：禁用 sedentary_monitor

**释放**：~8 KB 内部 RAM（任务栈 8192B + 摄像头相关内存）

**文件**：`config/config.toml`

**当前值**：`enabled = false`（已经是 false，确认生效即可）

**验证**：检查 `pio_prebuild.py` 是否正确将 `sedentary.enabled = false` 映射为 `CONFIG_SEDENTARY_ENABLE=n`

**说明**：代码中已有 `#if CONFIG_SEDENTARY_ENABLE` 保护，禁用后不会创建任务。config.toml 中已经是 `enabled = false`，此步无需改动，仅确认。

---

## 改动 2：`realtime_voice` 任务栈迁移到 SPIRAM

**释放**：~20 KB 内部 RAM

**文件**：`components/realtime_voice/realtime_voice.c`

### 当前代码（L45-46, L475）

```c
#define VOICE_TASK_STACK 20480
// ...
BaseType_t ret = xTaskCreate(voice_task, "realtime_voice", VOICE_TASK_STACK, NULL,
                              VOICE_TASK_PRIO, &s_ctx.task);
```

### 改为

1. 新增静态变量（文件作用域，L79 附近）：

```c
static StaticTask_t s_voice_tcb;
static StackType_t *s_voice_stack = NULL;
```

2. 在 `realtime_voice_start()` 中，`xTaskCreate` 调用前（L474 附近）分配 SPIRAM 栈：

```c
if (!s_voice_stack) {
    s_voice_stack = heap_caps_malloc(VOICE_TASK_STACK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_voice_stack) {
        s_voice_stack = heap_caps_malloc(VOICE_TASK_STACK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
}
if (!s_voice_stack) {
    ESP_LOGE(TAG, "Failed to allocate voice task stack");
    // ... cleanup and return ESP_FAIL
}
s_ctx.task = xTaskCreateStatic(voice_task, "realtime_voice",
                                VOICE_TASK_STACK / sizeof(StackType_t), NULL,
                                VOICE_TASK_PRIO, s_voice_stack, &s_voice_tcb);
if (s_ctx.task == NULL) {
    heap_caps_free(s_voice_stack);
    s_voice_stack = NULL;
    // ... cleanup and return ESP_FAIL
}
```

3. 在 `realtime_voice_stop()` 和 `voice_task` 退出清理处（L396 附近），释放栈：

```c
// 在 voice_task 的 vTaskDelete(NULL) 之前不能 free 自己的栈
// 改为：在 realtime_voice_stop() 中等待任务退出后释放
```

**注意**：任务不能 free 自己的栈。需要在 `realtime_voice_stop()` 中等待任务退出后释放，或采用延迟释放策略。参考 `rtsp_server.c` 的做法：栈只分配一次，不释放（静态生命周期）。

**最终方案**：栈分配一次后不释放，与 RTSP/NTP/audio_recv 的模式一致。`s_voice_stack` 在整个程序生命周期内保持。

---

## 改动 3：`voice_fe_cap` + `voice_fe_fch` 任务栈迁移到 SPIRAM

**释放**：~16 KB 内部 RAM（10240 + 6144）

**文件**：`components/realtime_voice/voice_frontend.c`

### 当前代码（L577, L592）

```c
if (xTaskCreate(fe_capture_task, "voice_fe_cap", VOICE_FE_CAP_TASK_STACK, NULL,
                VOICE_FE_CAP_TASK_PRIO, &s_fe.cap_task) != pdPASS) {
// ...
if (xTaskCreate(fe_fetch_task, "voice_fe_fch", VOICE_FE_FETCH_TASK_STACK, NULL,
                VOICE_FE_FETCH_TASK_PRIO, &s_fe.fetch_task) != pdPASS) {
```

### 改为

1. 新增静态变量（L83 附近）：

```c
static StaticTask_t s_cap_tcb;
static StackType_t *s_cap_stack = NULL;
static StaticTask_t s_fetch_tcb;
static StackType_t *s_fetch_stack = NULL;
```

2. 在 `voice_frontend_start()` 中，创建 cap 任务前分配栈：

```c
if (!s_cap_stack) {
    s_cap_stack = heap_caps_malloc(VOICE_FE_CAP_TASK_STACK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_cap_stack) {
        s_cap_stack = heap_caps_malloc(VOICE_FE_CAP_TASK_STACK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
}
if (!s_cap_stack) {
    // ... log error, cleanup, return ESP_FAIL
}
s_fe.cap_task = xTaskCreateStatic(fe_capture_task, "voice_fe_cap",
                                   VOICE_FE_CAP_TASK_STACK / sizeof(StackType_t), NULL,
                                   VOICE_FE_CAP_TASK_PRIO, s_cap_stack, &s_cap_tcb);
if (s_fe.cap_task == NULL) {
    // ... cleanup and return ESP_FAIL
}
```

3. 同样处理 fetch 任务。

4. 栈不释放（与改动 2 相同策略）。

---

## 改动 4：`audio_play` 任务栈迁移到 SPIRAM

**释放**：~3.5 KB 内部 RAM

**文件**：`components/audio_core/audio/audio_output_common.c`

### 当前代码（L356）

```c
xTaskCreatePinnedToCore(playback_task, name, 3584, NULL, 7,
                        &playback_task_handle, AO_PLAYBACK_CORE);
```

### 改为

1. 新增静态变量（L28 附近）：

```c
static StaticTask_t s_playback_tcb;
static StackType_t *s_playback_stack = NULL;
```

2. 在 `audio_output_common_start()` 中分配栈并使用 `xTaskCreateStaticPinnedToCore`：

```c
#define PLAYBACK_STACK_SIZE 3584

if (!s_playback_stack) {
    s_playback_stack = heap_caps_malloc(PLAYBACK_STACK_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_playback_stack) {
        s_playback_stack = heap_caps_malloc(PLAYBACK_STACK_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
}
if (!s_playback_stack) {
    ESP_LOGE(TAG, "Failed to allocate playback stack");
    return;
}
playback_task_handle = xTaskCreateStaticPinnedToCore(
    playback_task, name, PLAYBACK_STACK_SIZE / sizeof(StackType_t), NULL, 7,
    s_playback_stack, &s_playback_tcb, AO_PLAYBACK_CORE);
```

3. 在 `audio_output_common_stop()` 中不释放栈（静态生命周期）。

---

## 改动 5：`net_mon` 任务栈迁移到 SPIRAM

**释放**：~4 KB 内部 RAM

**文件**：`components/app_core/app_core.c`

### 当前代码（L456-457）

```c
BaseType_t ok =
    xTaskCreate(network_monitor_task, "net_mon", 4096, NULL, 5, NULL);
```

### 改为

1. 新增静态变量（文件作用域顶部）：

```c
static StaticTask_t s_net_mon_tcb;
static StackType_t *s_net_mon_stack = NULL;
```

2. 替换任务创建：

```c
if (!s_net_mon_stack) {
    s_net_mon_stack = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_net_mon_stack) {
        s_net_mon_stack = heap_caps_malloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
}
if (s_net_mon_stack) {
    TaskHandle_t net_mon_handle = xTaskCreateStatic(
        network_monitor_task, "net_mon", 4096 / sizeof(StackType_t), NULL, 5,
        s_net_mon_stack, &s_net_mon_tcb);
    if (net_mon_handle == NULL) {
        ESP_LOGW(TAG, "Failed to start network monitor ...");
        // fallback to inline execution
    }
} else {
    ESP_LOGW(TAG, "Failed to allocate net_mon stack ...");
    // fallback to inline execution
}
```

---

## 改动 6：裸 malloc/calloc → SPIRAM 优先

**释放**：~5-10 KB 内部 RAM

### 统一模式

所有改动遵循相同模式：

```c
// 之前
void *p = malloc(size);
void *p = calloc(count, size);

// 之后
void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
if (!p) p = heap_caps_malloc(size, MALLOC_CAP_8BIT);

// calloc 替换为：
size_t total = count * size;
void *p = heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
if (!p) p = heap_caps_malloc(total, MALLOC_CAP_8BIT);
if (p) memset(p, 0, total);
```

### 具体文件和行号

| # | 文件 | 行号 | 当前代码 | 说明 |
|---|------|------|---------|------|
| 6.1 | `rtsp_server.c` | L189 | `malloc(buf_capacity)` | RTSP client 初始 buffer 4KB |
| 6.2 | `rtsp_server.c` | L117 | `malloc(header_len + 1)` | RTSP header 临时分配 |
| 6.3 | `rtsp_message.c` | L164 | `malloc(total_len)` | RTSP response 构建 |
| 6.4 | `rtsp_handler_common.c` | L314 | `calloc(1, len + 1)` | SDP buffer |
| 6.5 | `audio_decoder.c` | L152 | `calloc(1, sizeof(*decoder))` | decoder 结构体 |
| 6.6 | `rtsp_conn.c` | L15 | `calloc(1, sizeof(rtsp_conn_t))` | conn 结构体 |
| 6.7 | `voice_request.c` | L125 | `malloc(json_len + 1)` | JSON copy |
| 6.8 | `audio_resample.c` | L35,40 | `malloc(...*sizeof(float))` | float 重采样缓冲区 |
| 6.9 | `audio_output_cores3.c` | L660 | `calloc(...)` | test tone buffer |
| 6.10 | `rtsp_rsa.c` | L191 | `malloc(rsa_len)` | RSA 输出 buffer |

---

## 实施顺序

1. **改动 2**：`realtime_voice` 栈→SPIRAM（最大收益 20KB）
2. **改动 3**：`voice_fe_cap` + `voice_fe_fch` 栈→SPIRAM（收益 16KB）
3. **改动 4**：`audio_play` 栈→SPIRAM（收益 3.5KB）
4. **改动 5**：`net_mon` 栈→SPIRAM（收益 4KB）
5. **改动 6**：裸 malloc→SPIRAM 优先（收益 5-10KB）

改动 1（sedentary）已在 config.toml 中禁用，无需代码改动。

---

## 风险与注意事项

1. **SPIRAM 栈 + I2S DMA**：`voice_fe_cap` 任务调用 `esp_codec_dev_read()`，内部走 I2S DMA。DMA 本身使用 DMA-capable buffer（已在别处分配），任务栈不涉及 DMA 操作，放 SPIRAM 安全。

2. **SPIRAM 栈 + Flash 访问**：SPIRAM 与 Flash 共享总线，Flash 操作（写入/擦除）期间 SPIRAM 不可访问。但语音任务不直接操作 Flash，安全。

3. **任务不能 free 自己的栈**：所有 SPIRAM 栈采用"分配一次、不释放"策略，与现有 RTSP/NTP/audio_recv 模式一致。

4. **`xTaskCreateStaticPinnedToCore`**：ESP-IDF 支持，已在 `audio_stream_realtime.c:475` 使用，模式已验证。

---

## 验证步骤

1. 编译烧录后，检查启动日志中 `internal_free` 和 `internal_largest` 值
2. 确认 `network_monitor` 任务创建成功（不再出现 "Failed to start network monitor" 警告）
3. 测试 AirPlay 播放：iPhone 连接 → 播放音乐 → 确认音频正常
4. 测试语音唤醒：说 "Hi ESP" → 录音 → 上传 → TTS 播放 → 确认全链路正常
5. 测试 AirPlay + 语音切换：播放音乐时唤醒 → 确认门控逻辑正常
6. 运行 1 小时稳定性测试，观察是否有栈溢出或异常重启
7. 检查各任务栈 watermark，确认无溢出风险
