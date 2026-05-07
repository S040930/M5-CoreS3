# 唤醒词无响应诊断与修复计划

## 问题描述
用户说了唤醒词 "Hi ESP"，但设备没有反应。日志中 `wake=0/0` 始终为零，说明 WakeNet 从未检测到唤醒词。

## 当前状态分析

### 日志关键指标
| 指标 | 值 | 含义 |
|------|-----|------|
| `wake=0/0` | 0 检测 / 0 转发 | WakeNet 从未触发 |
| `mic_read_ok` | 持续增长 | 麦克风硬件正常 |
| `feed_ok` | 持续增长 | AFE feed 正常 |
| `fetch_ok` | 持续增长 | AFE fetch 正常 |
| `loop_hz` | 11 | 前端循环频率正常 |
| `age(read/feed/fetch)` | 66ms/0ms/10ms | feed/fetch 延迟正常 |
| `feed_pending_frames` | 0~320 | 偶有积压但会消化 |
| `fetch_timeout=0` | 0 | 无 fetch 超时 |

### ESP-SR enum 实际定义（已确认）

来自 `managed_components/espressif__esp-sr/include/esp32s3/esp_wn_iface.h`:
```c
typedef enum {
    DET_MODE_90 = 0,       // Normal（90% 精确率，正常模式）
    DET_MODE_95 = 1,       // Aggressive（95% 精确率，更严格）
    DET_MODE_2CH_90 = 2,
    DET_MODE_2CH_95 = 3,
    DET_MODE_3CH_90 = 4,
    DET_MODE_3CH_95 = 5,
    DET_MODE_90_COPY_PARAMS = 6,
} det_mode_t;
```

来自 `esp_afe_config.h`:
```c
typedef enum {
    AFE_NS_MODE_WEBRTC = 0,
    AFE_NS_MODE_NET = 1,
} afe_ns_mode_t;

typedef enum {
    AFE_AGC_MODE_WEBRTC = 0,
    AFE_AGC_MODE_WAKENET = 1,
} afe_agc_mode_t;
```

### ⚠️ 关键发现：`afe_config_check()` 覆盖了设置

日志显示的配置与代码设置不一致：

| 参数 | 代码设置 | 日志实际值 | enum 值 |
|------|----------|-----------|---------|
| NS 模式 | `AFE_NS_MODE_NET` (1) | `WEBRTC` (0) | 被 check 覆盖 |
| wakenet_mode | `DET_MODE_90` (0) | `0` | 一致（90% 本身就是默认） |
| AGC 模式 | `AFE_AGC_MODE_WEBRTC` (0) | `WAKENET` (1) | 被 check 覆盖 |
| vad_model_name | 尝试加载 VADNet | `NULL` | 分区中无 VADNet 模型 |

**`afe_config_check()` 的文档明确说**："If there is a configuration conflict, this function will modify some parameters." 它将 NS 改为 WEBRTC、AGC 改为 WAKENET 模式。

### 根因分析（按可能性排序）

#### 1. 🔴 NS（降噪）干扰唤醒词识别 — 最可能的主因
日志明确警告：
> `W (5277) AFE_CONFIG: Noise Supression may reduce the accuracy of speech recognition. It is not recommended to turn it on.`

- 代码设置 `AFE_NS_MODE_NET`（神经网络降噪），但 `afe_config_check()` 将其改为 `WEBRTC`
- 无论哪种 NS，ESP-SR 自己都警告会降低识别率
- NS 会过滤掉部分语音频率成分，直接损害 WakeNet 的特征提取
- **解决方案：关闭 NS**

#### 2. 🔴 AGC 模式被改为 WAKENET — 形成死锁
- 代码设置 `AFE_AGC_MODE_WEBRTC`，但 `afe_config_check()` 改为 `AFE_AGC_MODE_WAKENET`
- `AFE_AGC_MODE_WAKENET` 的含义是"AGC 增益由 wakenet 模型计算，仅在 wakenet 激活时生效"
- 这意味着：在唤醒词检测之前，AGC 不工作 → 音频信号可能太弱 → WakeNet 无法检测 → AGC 永远不启动
- **这是一个"鸡生蛋"的死锁问题**
- **解决方案：保持 `AFE_AGC_MODE_WEBRTC`，或在 check 之后重新设置**

#### 3. 🟡 wakenet_mode = DET_MODE_90 (0) — 灵敏度偏低
- `DET_MODE_90` = 90% 精确率阈值，这是"正常"模式
- `DET_MODE_95` = 95% 精确率阈值，更严格（更难触发）
- 90% 模式本身不算太严格，但结合 NS 干扰和 AGC 死锁，可能不足以检测到
- 可以尝试降低阈值或使用 `set_wakenet_threshold` API 动态调整
- **解决方案：在 AFE 初始化后调用 `set_wakenet_threshold` 降低检测阈值**

#### 4. 🟡 AEC 参考信号全零
- 无播放时，ref 通道填充零
- AEC 在参考信号全零时可能产生异常输出
- 但这通常不是致命问题

#### 5. 🟡 麦克风信号处理
- ES7210 双麦克风（MIC1+MIC2）左右声道平均为单声道
- 平均可能引入相位抵消
- 增益 39dB 较高，可能削波

## 修复方案

### 改动 1: 关闭 NS（[afe_bridge.c:44-46](file:///Users/mac/Desktop/airplay/components/realtime_voice/afe_bridge.c#L44-L46)）

```c
// 修改前:
cfg->ns_init       = true;
cfg->afe_ns_mode   = AFE_NS_MODE_NET;

// 修改后:
cfg->ns_init       = false;
```

### 改动 2: 在 afe_config_check() 之后重新设置 AGC 模式（[afe_bridge.c:79](file:///Users/mac/Desktop/airplay/components/realtime_voice/afe_bridge.c#L79)）

```c
// 修改前:
afe_config_check(cfg);
afe_config_print(cfg);

// 修改后:
afe_config_check(cfg);
cfg->agc_mode = AFE_AGC_MODE_WEBRTC;  // 覆盖 check 的修改，避免 AGC 死锁
afe_config_print(cfg);
```

原因：`afe_config_check()` 会将 AGC 模式改为 `AFE_AGC_MODE_WAKENET`，导致在唤醒前 AGC 不工作，形成死锁。必须在 check 之后重新设置。

### 改动 3: 在 AFE 创建后降低唤醒词检测阈值（[afe_bridge.c:100-108](file:///Users/mac/Desktop/airplay/components/realtime_voice/afe_bridge.c#L100-L108)）

```c
// 在 s_afe_data 创建成功后添加:
s_afe_iface->set_wakenet_threshold(s_afe_data, 1, 0.5f);  // 降低阈值到 0.5，默认约 0.6
```

原因：默认阈值约 0.6（对应 90% 精确率），降低到 0.5 可以提高触发率，代价是误触发略增。对于本地唤醒词场景，这是合理的权衡。

## 修改文件清单

| 文件 | 改动 | 原因 |
|------|------|------|
| `components/realtime_voice/afe_bridge.c` | 关闭 NS | ESP-SR 警告 NS 降低识别率 |
| `components/realtime_voice/afe_bridge.c` | check 后重设 AGC 为 WEBRTC 模式 | 避免 AGC-WAKENET 死锁 |
| `components/realtime_voice/afe_bridge.c` | 添加 set_wakenet_threshold 调用 | 降低检测阈值提高触发率 |

## 验证步骤

1. 编译烧录后，观察 `afe_config_print` 输出：
   - `ns_init` 应为 `false`
   - `agc_mode` 应为 `WEBRTC` (0) 而非 `WAKENET` (1)
2. 在安静环境下距离设备 30cm 说 "Hi ESP"
3. 观察 `wake=` 计数是否增长
4. 如果仍不工作，进一步排查：
   - 尝试关闭 AEC（`cfg->aec_init = false`）
   - 检查麦克风信号质量（添加 debug hook 打印 feed 前的音频峰值）
