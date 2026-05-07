# Opus 编码问题修复计划

## 问题诊断

### 问题1：AI 无法识别用户语音内容
- **根因**：`input_audio.format` 设置为 `"opus"`，但发送的是裸 Opus 帧（无 OGG 容器），Qwen-Omni API 不支持这种格式
- API 收到无法解析的音频后，退化为纯文本对话模式，返回通用问候

### 问题2：没有声音输出
- **根因**：API 无法理解音频输入时，只返回文本不返回音频数据

## 解决方案：8kHz WAV

将 16kHz PCM 降采样到 8kHz，生成 WAV 文件上传。体积从 ~160KB 降到 ~80KB，API 完全兼容。

## 修改文件

### 1. `voice_request.c` — 主要改动
- **删除** `#include "esp_opus_enc.h"`
- **删除** `opus_encode_pcm()` 函数
- **删除** `base64_encode_opus()` 函数
- **新增** `downsample_16k_to_8k()` — 简单线性插值降采样
- **新增** `wav_from_pcm()` — 生成 WAV 文件（支持任意采样率）
- **新增** `base64_encode_wav()` — 带 `data:audio/wav;base64,` 前缀的 Base64 编码
- **修改** `build_request_body()`：
  - `input_audio.format` 改回 `"wav"`
- **修改** `voice_request_send_audio()`：
  - 先降采样 16kHz→8kHz
  - 生成 WAV
  - Base64 编码
  - 日志改为 `wav=`

### 2. `realtime_voice.c` — 回退栈大小
- `VOICE_TASK_STACK` 从 40960 改回 20480
- `xTaskCreateStatic` + PSRAM 改回 `xTaskCreate`
- 删除 `StaticTask_t task_tcb` 和 `StackType_t *task_stack` 字段
- 删除 `heap_caps_malloc(MALLOC_CAP_SPIRAM)` 和 `heap_caps_free()` 相关代码

### 3. `CMakeLists.txt` — 移除依赖
- 从 REQUIRES 列表移除 `esp_audio_codec`

## 验证步骤
1. 编译通过
2. 烧录到设备
3. 唤醒后说话，确认 AI 正确识别语音 + 有声音输出
