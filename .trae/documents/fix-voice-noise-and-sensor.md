# 修复计划：语音电流声修复 + 传感器硬件测试

## 问题概述

1. **语音播放电流声**：播放语音时能听到明显的电流声/杂音
2. **传感器完全不工作**：ENV.III 传感器初始化失败，陷入重试循环

## 当前状态分析

### 音频处理流程
```
API音频(24kHz mono PCM) → Ring Buffer → 重采样(24kHz→44.1kHz mono) → 单声道→立体声复制 → I2S输出
```

**问题分析**：
- 单声道→立体声转换只是简单复制（L=R=mono）
- 如果源音频有 DC 偏移，复制到双声道后可能导致扬声器偏置异常
- 电流声很可能来自 DAC 输出端的 DC 偏移累积

### 传感器问题分析
日志显示：
```
I2C transaction unexpected nack detected
SHT30 bus state error on read, resetting device
sensor init retry pending: err=ESP_ERR_TIMEOUT
```

**需要先进行硬件测试**，排除软件配置问题后再调整代码。

## 修复方案

### 1. 音频电流声修复（软件方案）

**修改文件**：`/Users/mac/Desktop/airplay/components/realtime_voice/voice_player.c`

**修改内容**：添加简单的直流偏移（DC offset）滤波

**原理**：使用一阶低通滤波器跟踪 DC 偏移，然后从每个样本中减去该偏移

```c
// 添加静态变量（函数内部或文件顶部）
static int32_t s_dc_offset = 0;
static const float DC_ALPHA = 0.995f;

// 在播放循环中处理每个立体声样本对
for (size_t i = 0; i < out_frames; i++) {
    int16_t left = out_samples[i * 2];
    int16_t right = out_samples[i * 2 + 1];

    // DC 偏移跟踪（左声道）
    s_dc_offset = (int32_t)(DC_ALPHA * s_dc_offset + (1.0f - DC_ALPHA) * (int32_t)left);

    // 减去 DC 偏移
    int32_t left_corrected = (int32_t)left - (s_dc_offset >> 16);
    int32_t right_corrected = (int32_t)right - (s_dc_offset >> 16);

    // 限幅
    left_corrected = left_corrected > 32767 ? 32767 : left_corrected < -32768 ? -32768 : left_corrected;
    right_corrected = right_corrected > 32767 ? 32767 : right_corrected < -32768 ? -32768 : right_corrected;

    out_samples[i * 2] = (int16_t)left_corrected;
    out_samples[i * 2 + 1] = (int16_t)right_corrected;
}
```

### 2. 传感器硬件测试（优先级更高）

**先进行硬件测试**，确认传感器本身是否正常：

#### 硬件测试步骤

1. **检查物理连接**
   - 确认 Grove 线缆牢固插入 Port A
   - 线缆方向正确（防呆设计通常不会插反）

2. **检查电源**
   - 用万用表测量 ENV.III 传感器红线（5V）和黑线（GND）之间的电压
   - 确认电压在 4.5V-5.5V 范围内

3. **检查 I2C 信号**（如果有示波器或逻辑分析仪）
   - 测量 SDA 和 SCL 线上是否有信号
   - 确认 SCL 频率正确（约 100kHz）

4. **用官方示例测试**
   - 使用 M5Stack 官方 Arduino 库中的 ENV.III 示例
   - 或使用 UIFlow 上传官方固件测试

#### 如果传感器硬件正常但仍不工作

修改 `/Users/mac/Desktop/airplay/components/env_monitor/env_monitor.c`：

```c
// 在 Grove 总线初始化后增加更长的延迟
vTaskDelay(pdMS_TO_TICKS(500));  // 增加延迟到 500ms

// 添加 I2C 总线诊断
ESP_LOGI(TAG, "Grove bus I2C diagnostic:");
for (uint8_t addr = 0x03; addr <= 0x77; addr++) {
    if (env_diag_addr_acked(hw->grove_bus, addr)) {
        ESP_LOGI(TAG, "  Found device at 0x%02X", addr);
    }
}
```

## 实施步骤

### 步骤 1：修复音频 DC 偏移问题
1. 在 `voice_player.c` 中添加 DC 偏移跟踪变量
2. 在播放循环中添加 DC 滤波处理（只处理立体声输出的左右声道）
3. 编译验证

### 步骤 2：硬件测试传感器
1. 检查 Grove 连接和电源
2. 用万用表测量 5V 供电是否正常
3. 如有条件，用示波器检查 I2C 信号

### 步骤 3：根据测试结果调整
- 如果硬件正常但软件不工作，调整 I2C 配置
- 如果硬件有问题，考虑更换传感器或 Grove 线缆

## 验证标准

### 音频验证
- [ ] 语音播放时无明显电流声/杂音
- [ ] 语音结束后无持续电流声
- [ ] 音量正常，无明显失真

### 传感器验证
- [ ] 万用表测量 5V 供电正常
- [ ] I2C 总线上能检测到设备地址
- [ ] 传感器初始化成功（日志显示 "ENV.3 sensor ready"）

## 风险和限制

1. **音频修改**：DC 滤波可能轻微影响低频响应，但语音主要集中在中频，影响很小
2. **硬件测试**：需要用户配合检查硬件连接和电源
